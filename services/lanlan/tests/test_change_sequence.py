"""One global change sequence for records and reminders.

The sync cursor is only meaningful when ``records.seq`` and ``reminders.seq``
share a strictly increasing order. These tests pin that down: a change written
after a sync is always delivered by the previous cursor, limit-1 paging never
skips a change, sequence values are globally unique, concurrent writers cannot
duplicate one, and an older database is renumbered instead of losing history.
"""

from __future__ import annotations

import os
import shutil
import tempfile
import threading
import unittest
from typing import Any, Dict, List, Optional, Tuple

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

from lanlan import admin, api as api_module, db as db_module
from lanlan.config import Config
from lanlan.server import LanlanServer
from services.lanlan.tests.harness import Client, LanlanTestCase, LanlanTestServer


class GlobalSequenceTestCase(LanlanTestCase):
    """Shared helpers for the cursor tests."""

    def setUp(self) -> None:
        super().setUp()
        self.caregiver = self.caregiver("hehe")
        self.device, self.token = self.device_client("seq-passport")

    def changes(self, cursor: int, limit: Optional[int] = None) -> Dict[str, Any]:
        query = "/api/v1/sync/changes?cursor=%d" % cursor
        if limit is not None:
            query += "&limit=%d" % limit
        response = self.device.get(query, token=self.token)
        self.assertEqual(200, response.status, response.text())
        return response.json()

    def drain(self, cursor: int = 0) -> Tuple[int, List[Tuple[str, str, int, int]]]:
        """Page to the end and return the final cursor plus everything seen."""
        seen: List[Tuple[str, str, int, int]] = []
        guard = 0
        while True:
            batch = self.changes(cursor, limit=100)
            for row in batch["records"]:
                seen.append(("record", row["id"], row["v"], row["seq"]))
            for row in batch["reminders"]:
                seen.append(("reminder", row["id"], row["v"], row["seq"]))
            cursor = batch["cursor"]
            guard += 1
            if not batch["has_more"]:
                return cursor, seen
            self.assertLess(guard, 50, "paging did not terminate")

    def reminders(self) -> List[Dict[str, Any]]:
        return self.caregiver.get("/api/v1/reminders").json()["reminders"]

    def patch_reminder(self, reminder_id: str, time_local: str = "07:30") -> Dict[str, Any]:
        response = self.caregiver.patch(
            "/api/v1/reminders/%s" % reminder_id,
            {"enabled": True, "time_local": time_local},
        )
        self.assertEqual(200, response.status, response.text())
        return response.json()["reminder"]


class PreviousCursorDeliveryTest(GlobalSequenceTestCase):
    def test_record_created_after_a_sync_is_delivered(self) -> None:
        previous, _ = self.drain()
        record = self.create_record(self.caregiver, category="walk", duration_minutes=15)

        batch = self.changes(previous)
        delivered = {row["id"]: row for row in batch["records"]}
        self.assertIn(record["id"], delivered, "the new record was skipped by the cursor")
        self.assertEqual(1, delivered[record["id"]]["v"])
        self.assertEqual(record["seq"], delivered[record["id"]]["seq"])
        self.assertGreater(delivered[record["id"]]["seq"], previous)
        self.assertFalse(batch["has_more"])

    def test_reminder_changed_after_a_sync_is_delivered(self) -> None:
        previous, _ = self.drain()
        reminder = self.reminders()[0]
        updated = self.patch_reminder(reminder["id"], "06:45")

        batch = self.changes(previous)
        delivered = {row["id"]: row for row in batch["reminders"]}
        self.assertIn(reminder["id"], delivered, "the reminder change was skipped by the cursor")
        self.assertEqual(2, delivered[reminder["id"]]["v"])
        self.assertEqual(1, delivered[reminder["id"]]["en"])
        self.assertEqual("06:45", delivered[reminder["id"]]["t"])
        self.assertGreater(delivered[reminder["id"]]["seq"], previous)

    def test_only_the_new_change_is_replayed(self) -> None:
        previous, seen = self.drain()
        self.assertEqual(7, len(seen))  # the seven initial reminders
        record = self.create_record(self.caregiver, category="meal")

        batch = self.changes(previous)
        self.assertEqual([record["id"]], [row["id"] for row in batch["records"]])
        self.assertEqual([], batch["reminders"])


class RevocationDeliveryTest(GlobalSequenceTestCase):
    def test_revocation_after_a_sync_is_delivered(self) -> None:
        record = self.create_record(self.caregiver, category="care", subitem="bath")
        previous, seen = self.drain()
        self.assertIn(record["id"], [entry[1] for entry in seen])

        revoked = self.caregiver.post(
            "/api/v1/records/%s/revoke" % record["id"],
            {"expected_version": 1, "reason": "复现用"},
        )
        self.assertEqual(200, revoked.status, revoked.text())

        batch = self.changes(previous)
        self.assertIn(record["id"], batch["revoked"], "the tombstone was skipped by the cursor")
        tombstones = [row for row in batch["records"] if row["id"] == record["id"]]
        self.assertEqual(1, len(tombstones))
        self.assertEqual("revoked", tombstones[0]["st"])
        self.assertEqual(2, tombstones[0]["v"])
        self.assertGreater(tombstones[0]["seq"], previous)

    def test_revocation_is_delivered_after_a_sync_that_included_the_record(self) -> None:
        # Two records, drained, then revoke both and check both tombstones.
        first = self.create_record(self.caregiver, category="meal")
        second = self.create_record(self.caregiver, category="water")
        previous, _ = self.drain()

        for record in (first, second):
            response = self.caregiver.post(
                "/api/v1/records/%s/revoke" % record["id"], {"expected_version": 1}
            )
            self.assertEqual(200, response.status, response.text())

        batch = self.changes(previous)
        self.assertEqual(sorted([first["id"], second["id"]]), sorted(batch["revoked"]))


class LimitOnePagingTest(GlobalSequenceTestCase):
    def test_every_interleaved_change_is_delivered_exactly_once(self) -> None:
        start, _ = self.drain()
        reminders = self.reminders()

        expected: List[Tuple[str, str, int]] = []
        writes = 6
        for index in range(writes):
            record = self.create_record(
                self.caregiver, category="meal", note="交错写入 %d" % index
            )
            expected.append(("record", record["id"], 1))
            reminder = self.patch_reminder(reminders[index]["id"], "%02d:00" % (index + 5))
            expected.append(("reminder", reminder["id"], 2))

        collected: List[Tuple[str, str, int, int]] = []
        cursor = start
        pages = 0
        while True:
            batch = self.changes(cursor, limit=1)
            delivered = (
                len(batch["records"]) + len(batch["reminders"])
            )
            self.assertLessEqual(delivered, 1, "limit=1 returned more than one change")
            for row in batch["records"]:
                collected.append(("record", row["id"], row["v"], row["seq"]))
            for row in batch["reminders"]:
                collected.append(("reminder", row["id"], row["v"], row["seq"]))
            self.assertGreater(batch["cursor"], cursor - 1)
            cursor = batch["cursor"]
            pages += 1
            if not batch["has_more"]:
                break
            self.assertLess(pages, 60, "paging did not terminate")

        self.assertEqual(2 * writes, len(collected), "a change was skipped or duplicated")
        keys = [(kind, identifier, version) for kind, identifier, version, _ in collected]
        self.assertEqual(len(keys), len(set(keys)), "a change was delivered twice")
        self.assertEqual(sorted(expected), sorted(keys))

        seqs = [entry[3] for entry in collected]
        self.assertEqual(sorted(seqs), seqs, "changes were not delivered in seq order")
        self.assertEqual(len(seqs), len(set(seqs)), "a seq value was reused")
        self.assertEqual(max(seqs), cursor)

    def test_has_more_is_true_with_exactly_one_change_left_in_each_table(self) -> None:
        # A boundary that per-table has_more logic gets wrong: fetching limit+1
        # rows from each table finds exactly `limit` in both, yet two changes
        # remain.
        start, _ = self.drain()
        reminder = self.reminders()[0]
        self.create_record(self.caregiver, category="meal")
        self.patch_reminder(reminder["id"], "05:30")

        first = self.changes(start, limit=1)
        self.assertEqual(1, len(first["records"]) + len(first["reminders"]))
        self.assertTrue(first["has_more"], "the second change would be lost")

        second = self.changes(first["cursor"], limit=1)
        self.assertEqual(1, len(second["records"]) + len(second["reminders"]))
        self.assertFalse(second["has_more"])
        kinds = (
            ("record" if first["records"] else "reminder"),
            ("record" if second["records"] else "reminder"),
        )
        self.assertEqual(("record", "reminder"), kinds)


class SequenceUniquenessTest(GlobalSequenceTestCase):
    def test_seq_is_unique_across_records_and_reminders(self) -> None:
        reminders = self.reminders()
        for index in range(4):
            self.create_record(self.caregiver, category="meal", note="第 %d 笔" % index)
            self.patch_reminder(reminders[index]["id"], "%02d:15" % (index + 6))

        connection = self.server.connect()
        try:
            rows = list(
                connection.execute(
                    "SELECT seq FROM records UNION ALL SELECT seq FROM reminders ORDER BY seq"
                )
            )
            maximum = connection.execute(
                "SELECT MAX(value) AS m FROM ("
                " SELECT COALESCE(MAX(seq),0) AS value FROM records"
                " UNION ALL SELECT COALESCE(MAX(seq),0) AS value FROM reminders)"
            ).fetchone()["m"]
        finally:
            connection.close()
        seqs = [int(row["seq"]) for row in rows]
        self.assertEqual(len(seqs), len(set(seqs)), "seq values are not globally unique")
        self.assertEqual(sorted(seqs), seqs)
        self.assertEqual(max(seqs), int(maximum))

        status = self.caregiver.get("/api/v1/status").json()
        self.assertEqual(int(maximum), status["max_seq"])

    def test_a_new_write_always_gets_a_higher_seq(self) -> None:
        connection = self.server.connect()
        try:
            before = connection.execute(
                "SELECT MAX(value) AS m FROM ("
                " SELECT COALESCE(MAX(seq),0) AS value FROM records"
                " UNION ALL SELECT COALESCE(MAX(seq),0) AS value FROM reminders)"
            ).fetchone()["m"]
        finally:
            connection.close()
        record = self.create_record(self.caregiver, category="meal")
        self.assertGreater(record["seq"], int(before))

        reminder = self.patch_reminder(self.reminders()[0]["id"], "04:45")
        connection = self.server.connect()
        try:
            max_seq = connection.execute(
                "SELECT MAX(value) AS m FROM ("
                " SELECT COALESCE(MAX(seq),0) AS value FROM records"
                " UNION ALL SELECT COALESCE(MAX(seq),0) AS value FROM reminders)"
            ).fetchone()["m"]
        finally:
            connection.close()
        self.assertGreater(int(max_seq), record["seq"])
        self.assertLessEqual(record["seq"], int(max_seq))

        batch = self.changes(0, limit=100)
        reminder_rows = {row["id"]: row for row in batch["reminders"]}
        self.assertEqual(2, reminder_rows[reminder["id"]]["v"])


class ConcurrentSequenceTest(GlobalSequenceTestCase):
    def test_parallel_writers_allocate_unique_ordered_seq(self) -> None:
        connection = self.server.connect()
        try:
            baseline = int(
                connection.execute(
                    "SELECT MAX(value) AS m FROM ("
                    " SELECT COALESCE(MAX(seq),0) AS value FROM records"
                    " UNION ALL SELECT COALESCE(MAX(seq),0) AS value FROM reminders)"
                ).fetchone()["m"]
            )
        finally:
            connection.close()

        reminders = self.reminders()
        record_writer = self.server.caregiver("hehe")
        reminder_writer = self.server.caregiver("yangyang")
        failures: List[str] = []
        lock = threading.Lock()
        barrier = threading.Barrier(2)
        rounds = 5

        def write_records() -> None:
            barrier.wait(timeout=10)
            for index in range(rounds):
                response = record_writer.post(
                    "/api/v1/records",
                    {"category": "meal", "note": "并发记录 %d" % index},
                )
                if response.status != 200:
                    with lock:
                        failures.append(response.text())

        def write_reminders() -> None:
            barrier.wait(timeout=10)
            for index in range(rounds):
                response = reminder_writer.patch(
                    "/api/v1/reminders/%s" % reminders[index]["id"],
                    {"enabled": True, "time_local": "%02d:30" % (index + 7)},
                )
                if response.status != 200:
                    with lock:
                        failures.append(response.text())

        threads = [
            threading.Thread(target=write_records),
            threading.Thread(target=write_reminders),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=30)
        self.assertEqual([], failures)

        connection = self.server.connect()
        try:
            rows = list(
                connection.execute(
                    "SELECT seq FROM records UNION ALL SELECT seq FROM reminders ORDER BY seq"
                )
            )
        finally:
            connection.close()
        seqs = [int(row["seq"]) for row in rows]
        self.assertEqual(len(seqs), len(set(seqs)), "two writers reused a seq value")
        self.assertEqual(sorted(seqs), seqs)
        expected = list(range(baseline + 1, baseline + 1 + 2 * rounds))
        self.assertEqual(expected, [value for value in seqs if value > baseline])

        # Every change remains reachable through the cursor.
        _, seen = self.drain()
        self.assertEqual(len(seqs), len(seen))


class LegacyDatabaseMigrationTest(unittest.TestCase):
    """A version-1 database is renumbered into one global order."""

    def setUp(self) -> None:
        self.directory = tempfile.mkdtemp(prefix="lanlan-legacy-")
        self.addCleanup(shutil.rmtree, self.directory, True)
        self.db_path = os.path.join(self.directory, "legacy.sqlite3")
        self.config = Config(
            db_path=self.db_path,
            host="127.0.0.1",
            port=0,
            pbkdf2_iterations=1000,
            db_path_explicit=True,
        )
        self._build_legacy_database()

    def _build_legacy_database(self) -> None:
        """Create the pre-fix layout: no change_seq, colliding seq values."""
        connection = db_module.connect(self.db_path)
        try:
            db_module.initialize(connection)
            # Simulate the old layout: independent per-table counters.
            connection.execute("DROP TABLE change_seq")
            connection.execute("PRAGMA user_version=1")
            connection.execute(
                "INSERT INTO families(id, name, timezone, created_at) VALUES(?,?,?,?)",
                ("family-legacy", "Legacy family", "Asia/Shanghai", "2026-01-01T00:00:00Z"),
            )
            for username in ("hehe", "yangyang"):
                connection.execute(
                    "INSERT INTO users(id, family_id, username, display_name, password_hash,"
                    " created_at) VALUES(?,?,?,?,?,?)",
                    (
                        "user-%s" % username,
                        "family-legacy",
                        username,
                        username,
                        "pbkdf2_sha256$1000$00$00",
                        "2026-01-01T00:00:00Z",
                    ),
                )
            # records seq 1..2 and reminders seq 1..3 overlap on purpose.
            for seq, status, supersedes in ((1, "active", None), (2, "revoked", 1)):
                connection.execute(
                    "INSERT INTO records(seq, id, version, family_id, category, subitem,"
                    " custom_name, occurred_at, occurred_tz, time_confidence, created_at,"
                    " created_by, performed_by, amount_value, amount_unit, duration_minutes,"
                    " note, status, source, client_request_id, supersedes_seq, revoke_reason)"
                    " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        seq,
                        "record-legacy",
                        seq,
                        "family-legacy",
                        "meal",
                        None,
                        None,
                        "2026-01-0%dT00:00:00Z" % seq,
                        "Asia/Shanghai",
                        "trusted",
                        "2026-01-0%dT00:00:00Z" % seq,
                        "user-hehe",
                        "user-hehe",
                        10.0 * seq,
                        "g",
                        None,
                        "legacy revision %d" % seq,
                        status,
                        "manual",
                        None,
                        supersedes,
                        None,
                    ),
                )
            for seq in (1, 2, 3):
                connection.execute(
                    "INSERT INTO reminders(seq, id, version, family_id, category, subitem,"
                    " custom_name, enabled, schedule_type, time_local, created_at, updated_at,"
                    " updated_by) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        seq,
                        "reminder-legacy-%d" % seq,
                        1,
                        "family-legacy",
                        "meal",
                        None,
                        None,
                        0,
                        "daily",
                        None,
                        "2026-01-01T00:00:00Z",
                        "2026-01-01T00:00:00Z",
                        "user-hehe",
                    ),
                )
        finally:
            connection.close()

    def test_migration_renumbers_into_one_global_order(self) -> None:
        connection = db_module.connect(self.db_path)
        try:
            self.assertEqual(1, connection.execute("PRAGMA user_version").fetchone()[0])
            db_module.initialize(connection)
            self.assertEqual(db_module.SCHEMA_VERSION, connection.execute(
                "PRAGMA user_version").fetchone()[0])

            rows = list(
                connection.execute(
                    "SELECT seq FROM records UNION ALL SELECT seq FROM reminders ORDER BY seq"
                )
            )
            seqs = [int(row["seq"]) for row in rows]
            self.assertEqual([1, 2, 3, 4, 5], seqs)

            records = list(
                connection.execute(
                    "SELECT seq, version, supersedes_seq FROM records ORDER BY seq"
                )
            )
            self.assertEqual([1, 3], [int(row["seq"]) for row in records])
            self.assertIsNone(records[0]["supersedes_seq"])
            # The revision link follows the renumbering.
            self.assertEqual(1, int(records[1]["supersedes_seq"]))
            reminders = list(connection.execute("SELECT seq FROM reminders ORDER BY seq"))
            self.assertEqual([2, 4, 5], [int(row["seq"]) for row in reminders])

            # The allocator continues after the renumbered maximum.
            self.assertEqual(6, db_module.allocate_seq(connection, "2026-01-02T00:00:00Z"))
            self.assertEqual(7, db_module.allocate_seq(connection, "2026-01-02T00:00:00Z"))
        finally:
            connection.close()

        # Initialising again is a no-op.
        connection = db_module.connect(self.db_path)
        try:
            db_module.initialize(connection)
            count = connection.execute("SELECT COUNT(*) AS n FROM records").fetchone()["n"]
        finally:
            connection.close()
        self.assertEqual(2, int(count))

    def test_migrated_database_serves_a_working_cursor(self) -> None:
        server = LanlanTestServer(directory=self.directory, db_name="legacy.sqlite3", initialize=False)
        self.addCleanup(server.close)
        # The server initialises (and therefore migrates) the file on startup.
        server.start()

        connection = server.connect()
        try:
            device_id, token = admin_auth_device(connection, "family-legacy", "legacy-passport")
        finally:
            connection.close()

        device = server.client()
        first = device.get("/api/v1/sync/changes?cursor=0&limit=100", token=token)
        self.assertEqual(200, first.status, first.text())
        payload = first.json()
        # Two revisions of the legacy record plus three reminders, all distinct.
        self.assertEqual(2, len(payload["records"]))
        self.assertEqual(3, len(payload["reminders"]))
        self.assertEqual(["record-legacy"], payload["revoked"])
        self.assertFalse(payload["has_more"])
        self.assertEqual(5, payload["cursor"])
        seqs = [row["seq"] for row in payload["records"]] + [
            row["seq"] for row in payload["reminders"]
        ]
        self.assertEqual([1, 2, 3, 4, 5], sorted(seqs))

        # A change made after the sync is delivered by the previous cursor on the
        # migrated database as well.
        connection = db_module.connect(server.db_path)
        try:
            with db_module.transaction(connection):
                api_module.insert_record_revision(
                    connection,
                    seq=db_module.allocate_seq(connection, "2026-01-03T00:00:00Z"),
                    record_id="record-fresh",
                    version=1,
                    family_id="family-legacy",
                    fields={
                        "category": "water",
                        "subitem": None,
                        "custom_name": None,
                        "occurred_at": "2026-01-03T00:00:00Z",
                        "occurred_tz": "Asia/Shanghai",
                        "time_confidence": "trusted",
                        "amount_value": None,
                        "amount_unit": None,
                        "duration_minutes": None,
                        "note": None,
                    },
                    created_at="2026-01-03T00:00:00Z",
                    created_by="user-hehe",
                    performed_by="user-hehe",
                    status="active",
                    client_request_id=None,
                    supersedes_seq=None,
                )
        finally:
            connection.close()

        after = device.get("/api/v1/sync/changes?cursor=5", token=token)
        self.assertEqual(200, after.status, after.text())
        delivered = after.json()
        self.assertEqual(["record-fresh"], [row["id"] for row in delivered["records"]])
        self.assertEqual(6, delivered["cursor"])

        # Paging from the start still finds every migrated change exactly once.
        paged = device.get("/api/v1/sync/changes?cursor=0&limit=2", token=token).json()
        self.assertTrue(paged["has_more"])
        follow_up = device.get(
            "/api/v1/sync/changes?cursor=%d&limit=100" % paged["cursor"], token=token
        ).json()
        seen = [row["seq"] for row in paged["records"] + follow_up["records"]]
        seen += [row["seq"] for row in paged["reminders"] + follow_up["reminders"]]
        self.assertEqual([1, 2, 3, 4, 5, 6], sorted(seen))


def admin_auth_device(connection: Any, family_id: str, label: str) -> Tuple[str, str]:
    from lanlan import auth

    return auth.create_device(connection, family_id, label)


if __name__ == "__main__":
    unittest.main()
