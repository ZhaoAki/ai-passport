"""Export and backup/restore: all revisions, tombstones, checksum round trip (A13)."""

from __future__ import annotations

import csv
import io
import json
import os
import sqlite3
import unittest
from typing import Dict

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

from lanlan import admin, db as db_module, export as export_module
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import LanlanTestCase


class ExportTest(LanlanTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.client = self.caregiver("hehe")
        self.meal = self.create_record(
            self.client, category="meal", amount_value=100.0, amount_unit="g", note="早饭"
        )
        self.water = self.create_record(
            self.client, category="water", amount_value=250.0, amount_unit="ml"
        )
        # One edit and one revocation, so the export must contain three revisions.
        edited = self.client.patch(
            "/api/v1/records/%s" % self.meal["id"],
            {"expected_version": 1, "category": "meal", "amount_value": 120.0,
             "amount_unit": "g", "note": "早饭（改）"},
        )
        self.assertEqual(200, edited.status, edited.text())
        revoked = self.client.post(
            "/api/v1/records/%s/revoke" % self.water["id"],
            {"expected_version": 1, "reason": "倒掉了"},
        )
        self.assertEqual(200, revoked.status, revoked.text())

    def test_csv_export_contains_every_revision(self) -> None:
        response = self.client.get("/api/v1/export/records.csv")
        self.assertEqual(200, response.status, response.text())
        self.assertIn("text/csv", response.headers.getheader("Content-Type") or "")
        rows = list(csv.DictReader(io.StringIO(response.text())))
        self.assertEqual(4, len(rows))
        by_version = {(row["id"], row["version"]): row for row in rows}
        self.assertEqual({"1", "2"}, {key[1] for key in by_version if key[0] == self.meal["id"]})
        self.assertIn((self.water["id"], "2"), by_version)
        self.assertEqual("revoked", by_version[(self.water["id"], "2")]["status"])
        self.assertEqual("倒掉了", by_version[(self.water["id"], "2")]["revoke_reason"])
        self.assertEqual("100.0", by_version[(self.meal["id"], "1")]["amount_value"])
        self.assertEqual("120.0", by_version[(self.meal["id"], "2")]["amount_value"])
        self.assertEqual("g", by_version[(self.meal["id"], "1")]["amount_unit"])
        self.assertEqual("早饭（改）", by_version[(self.meal["id"], "2")]["note"])
        self.assertEqual("赫赫", by_version[(self.meal["id"], "1")]["created_by_name"])
        self.assertEqual("manual", by_version[(self.meal["id"], "1")]["source"])
        self.assertEqual("", by_version[(self.meal["id"], "1")]["supersedes_seq"])
        # The revision link is the seq of the revision it replaces; seq values
        # are drawn from the global change allocator, so compare, never assume.
        self.assertEqual(
            by_version[(self.meal["id"], "1")]["seq"],
            by_version[(self.meal["id"], "2")]["supersedes_seq"],
        )

    def test_json_export_is_lossless(self) -> None:
        response = self.client.get("/api/v1/export/records.json")
        self.assertEqual(200, response.status, response.text())
        payload = json.loads(response.text())
        self.assertEqual(4, payload["revision_count"])
        self.assertEqual("lanlan-export", payload["format"])
        self.assertEqual("Asia/Shanghai", payload["family"]["timezone"])
        self.assertEqual(7, len(payload["reminders"]))
        statuses = sorted(row["status"] for row in payload["records"])
        self.assertEqual(["active", "active", "active", "revoked"], statuses)
        meal_versions = [row for row in payload["records"] if row["id"] == self.meal["id"]]
        self.assertEqual([1, 2], [row["version"] for row in meal_versions])
        self.assertEqual(100.0, meal_versions[0]["amount_value"])
        self.assertEqual(120.0, meal_versions[1]["amount_value"])

    def test_exports_match_the_database(self) -> None:
        connection = self.server.connect()
        try:
            family_id = self.server.family_id()
            csv_text = export_module.export_csv(connection, family_id)
            json_payload = export_module.export_json(connection, family_id)
            rows = list(
                connection.execute(
                    "SELECT id, version, status FROM records WHERE family_id=? ORDER BY seq",
                    (family_id,),
                )
            )
        finally:
            connection.close()
        csv_rows = list(csv.DictReader(io.StringIO(csv_text)))
        self.assertEqual(len(rows), len(csv_rows))
        self.assertEqual(
            [(row["id"], row["version"], row["status"]) for row in rows],
            [(row["id"], int(row["version"]), row["status"]) for row in csv_rows],
        )
        self.assertEqual(
            [(row["id"], row["version"], row["status"]) for row in rows],
            [(row["id"], row["version"], row["status"]) for row in json_payload["records"]],
        )

    def test_export_requires_a_caregiver_session(self) -> None:
        anonymous = self.server.client()
        self.assertEqual(401, anonymous.get("/api/v1/export/records.csv").status)
        device, token = self.device_client()
        self.assertEqual(403, device.get("/api/v1/export/records.json", token=token).status)

    def test_cli_export_writes_a_file(self) -> None:
        path = os.path.join(self.server.directory, "cli-export.json")
        connection = self.server.connect()
        try:
            target = admin.export_to_file(connection, self.server.family_id(), "json", path)
        finally:
            connection.close()
        self.assertEqual(path, target)
        with open(path, "r", encoding="utf-8") as handle:
            payload = json.load(handle)
        self.assertEqual(4, payload["revision_count"])


class BackupRestoreTest(LanlanTestCase):
    def test_backup_then_restore_round_trips(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="meal", amount_value=42.0, amount_unit="g")
        client.patch(
            "/api/v1/records/%s" % record["id"],
            {"expected_version": 1, "category": "meal", "amount_value": 43.0, "amount_unit": "g"},
        )
        revoked = self.create_record(client, category="water")
        client.post("/api/v1/records/%s/revoke" % revoked["id"], {"expected_version": 1})

        out_dir = os.path.join(self.server.directory, "backups")
        target = admin.backup_database(self.server.db_path, out_dir)
        self.assertTrue(os.path.isfile(target))
        self.assertFalse(os.path.exists(target + "-wal"))
        self.assertFalse(os.path.exists(target + "-shm"))
        digest = admin.sha256_file(target)
        self.assertTrue(os.path.isfile(target + ".sha256"))
        self.assertTrue(admin.verify_checksum(target))
        with open(target + ".sha256", "r", encoding="utf-8") as handle:
            contents = handle.read()
        self.assertIn(digest, contents)
        self.assertIn(os.path.basename(target), contents)

        restored_path = os.path.join(self.server.directory, "restored.sqlite3")
        admin.restore_database(target, restored_path, force=False)
        connection = db_module.connect(restored_path)
        try:
            rows = list(
                connection.execute("SELECT id, version, status FROM records ORDER BY seq")
            )
            reminders = connection.execute("SELECT COUNT(*) AS n FROM reminders").fetchone()["n"]
            users = connection.execute("SELECT COUNT(*) AS n FROM users").fetchone()["n"]
        finally:
            connection.close()
        self.assertEqual(4, len(rows))
        self.assertEqual("revoked", rows[-1]["status"])
        self.assertEqual(7, int(reminders))
        self.assertEqual(2, int(users))

        # The restored copy exports identically to the live database.
        live = self.server.connect()
        copy = db_module.connect(restored_path)
        try:
            self.assertEqual(
                export_module.export_csv(live, self.server.family_id()),
                export_module.export_csv(copy, self.server.family_id()),
            )
        finally:
            live.close()
            copy.close()

    def test_restore_refuses_to_overwrite_without_force(self) -> None:
        client = self.caregiver("hehe")
        self.create_record(client, category="meal")
        out_dir = os.path.join(self.server.directory, "backups")
        target = admin.backup_database(self.server.db_path, out_dir)

        with self.assertRaises(admin.AdminError) as caught:
            admin.restore_database(target, self.server.db_path, force=False)
        self.assertIn("refusing to overwrite", str(caught.exception))

        # The live database is untouched by the refusal.
        connection = self.server.connect()
        try:
            self.assertEqual(1, connection.execute("SELECT COUNT(*) AS n FROM records").fetchone()["n"])
        finally:
            connection.close()

        # A fresh backup of the empty database then a forced restore both work.
        fresh = admin.backup_database(self.server.db_path, out_dir, label="fresh.sqlite3")
        admin.restore_database(fresh, self.server.db_path, force=True)
        connection = self.server.connect()
        try:
            self.assertEqual(1, connection.execute("SELECT COUNT(*) AS n FROM records").fetchone()["n"])
            self.assertEqual("ok", connection.execute("PRAGMA integrity_check").fetchone()[0])
        finally:
            connection.close()

    def test_backup_refuses_to_overwrite_an_existing_file(self) -> None:
        out_dir = os.path.join(self.server.directory, "backups")
        first = admin.backup_database(self.server.db_path, out_dir, label="copy.sqlite3")
        with self.assertRaises(admin.AdminError):
            admin.backup_database(self.server.db_path, out_dir, label="copy.sqlite3")
        self.assertTrue(os.path.isfile(first))

    def test_backup_of_a_missing_database_fails(self) -> None:
        with self.assertRaises(admin.AdminError):
            admin.backup_database(
                os.path.join(self.server.directory, "missing.sqlite3"),
                os.path.join(self.server.directory, "backups"),
            )

    def test_restore_rejects_a_corrupt_backup(self) -> None:
        broken = os.path.join(self.server.directory, "broken.sqlite3")
        with open(broken, "wb") as handle:
            handle.write(b"this is not a database")
        target = os.path.join(self.server.directory, "target.sqlite3")
        with self.assertRaises((admin.AdminError, sqlite3.DatabaseError)):
            admin.restore_database(broken, target, force=True)

    def test_backup_preserves_wal_configuration(self) -> None:
        out_dir = os.path.join(self.server.directory, "backups")
        target = admin.backup_database(self.server.db_path, out_dir)
        connection = sqlite3.connect(target)
        try:
            self.assertEqual("ok", connection.execute("PRAGMA integrity_check").fetchone()[0])
            tables = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                )
            }
        finally:
            connection.close()
        for expected in (
            "families", "users", "sessions", "devices", "records", "reminders",
            "device_acks", "idempotency",
        ):
            self.assertIn(expected, tables)


class ExportRevokedRevisionTest(LanlanTestCase):
    def test_revoked_revision_stays_in_the_export(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="walk", duration_minutes=20)
        client.post("/api/v1/records/%s/revoke" % record["id"],
                    {"expected_version": 1, "reason": "重复记录"})
        payload = json.loads(client.get("/api/v1/export/records.json").text())
        self.assertEqual(2, len(payload["records"]))
        self.assertEqual(["active", "revoked"], [row["status"] for row in payload["records"]])
        self.assertEqual("重复记录", payload["records"][1]["revoke_reason"])
        # The list still hides it: export keeps history, the app hides tombstones.
        self.assertEqual([], client.get("/api/v1/records").json()["records"])


if __name__ == "__main__":
    unittest.main()
