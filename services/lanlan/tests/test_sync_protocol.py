"""Sync protocol: cursors, has_more, compact fields, snapshots and 409 (A06 part)."""

from __future__ import annotations

import re
import unittest
from typing import Any, Dict, List, Optional

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

from lanlan import admin

try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import LanlanTestCase


EXPECTED_COMPACT_RECORD_KEYS = {
    "id", "v", "cat", "sub", "name", "at", "tz", "tc", "by", "perf",
    "unit", "dur", "note", "st", "seq",
}

EXPECTED_COMPACT_REMINDER_KEYS = {"id", "v", "cat", "sub", "name", "en", "sched", "t", "upd", "seq"}


class SyncChangesTest(LanlanTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.caregiver = self.caregiver("hehe")
        self.device, self.token = self.device_client("sync-passport")

    def changes(self, cursor: int, limit: Optional[int] = None) -> Dict[str, Any]:
        query = "/api/v1/sync/changes?cursor=%d" % cursor
        if limit is not None:
            query += "&limit=%d" % limit
        response = self.device.get(query, token=self.token)
        self.assertEqual(200, response.status, response.text())
        return response.json()

    def test_full_batch_shape_and_compact_fields(self) -> None:
        record = self.create_record(
            self.caregiver, category="meal", amount_value=120.0, amount_unit="g",
            note="晚饭",
        )
        payload = self.changes(0)
        self.assertEqual(
            set([
                "server_time", "timezone", "utc_offset_minutes", "cursor", "has_more",
                "records", "reminders", "revoked", "device_id", "members",
            ]),
            set(payload.keys()),
        )
        self.assertEqual(1, len(payload["records"]))
        compact = payload["records"][0]
        self.assertEqual(EXPECTED_COMPACT_RECORD_KEYS | {"amt"}, set(compact.keys()))
        self.assertEqual(record["id"], compact["id"])
        self.assertEqual("meal", compact["cat"])
        self.assertRegex(compact["at"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertEqual(120.0, compact["amt"])
        self.assertEqual("g", compact["unit"])
        self.assertEqual("active", compact["st"])
        self.assertEqual(record["version"], compact["v"])
        self.assertGreaterEqual(compact["seq"], 1)

        self.assertEqual(7, len(payload["reminders"]))
        self.assertEqual(EXPECTED_COMPACT_REMINDER_KEYS, set(payload["reminders"][0].keys()))
        self.assertEqual(0, payload["reminders"][0]["en"])
        self.assertIsNone(payload["reminders"][0]["t"])

    def test_amount_is_omitted_when_unknown(self) -> None:
        unknown = self.create_record(self.caregiver, category="water")
        known = self.create_record(
            self.caregiver, category="meal", amount_value=30.0, amount_unit="scoop"
        )
        payload = self.changes(0)
        by_id = {row["id"]: row for row in payload["records"]}
        self.assertNotIn("amt", by_id[unknown["id"]])
        self.assertEqual(30.0, by_id[known["id"]]["amt"])

    def test_utc_offset_and_server_time_are_present(self) -> None:
        payload = self.changes(0)
        self.assertIn("server_time", payload)
        self.assertEqual("Asia/Shanghai", payload["timezone"])
        self.assertIn(payload["utc_offset_minutes"], (480, 540))

    def test_cursor_ordering_and_has_more(self) -> None:
        for index in range(4):
            self.create_record(self.caregiver, category="meal", note="第 %d 笔" % index)

        page_one = self.changes(0, limit=4)
        # Four records and the seven reminders share one seq stream.
        self.assertEqual(4, len(page_one["records"]) + len(page_one["reminders"]))
        self.assertTrue(page_one["has_more"])
        # Each list is individually ordered by seq.
        record_seqs = [row["seq"] for row in page_one["records"]]
        reminder_seqs = [row["seq"] for row in page_one["reminders"]]
        self.assertEqual(sorted(record_seqs), record_seqs)
        self.assertEqual(sorted(reminder_seqs), reminder_seqs)
        every_seq = record_seqs + reminder_seqs
        self.assertEqual(max(every_seq), page_one["cursor"])

        seen_records: List[Dict[str, Any]] = []
        cursor = 0
        guard = 0
        while True:
            payload = self.changes(cursor, limit=4)
            seen_records.extend(payload["records"])
            cursor = payload["cursor"]
            guard += 1
            if not payload["has_more"] or guard > 10:
                break
        self.assertEqual(4, len([row for row in seen_records if row["id"]]))
        self.assertEqual(sorted(row["seq"] for row in seen_records),
                         [row["seq"] for row in seen_records])

    def test_batch_never_skips_a_write(self) -> None:
        first = self.create_record(self.caregiver, category="meal")
        payload = self.changes(0, limit=1)
        self.assertTrue(payload["has_more"])
        smallest = min(
            [row["seq"] for row in payload["records"]] +
            [row["seq"] for row in payload["reminders"]]
        )
        self.assertEqual(smallest, payload["cursor"])
        known = set(row["id"] for row in payload["records"])
        if first["id"] not in known:
            second = self.changes(payload["cursor"], limit=100)
            self.assertIn(first["id"], [row["id"] for row in second["records"]])

    def test_revoked_ids_are_reported(self) -> None:
        doomed = self.create_record(self.caregiver, category="meal")
        keep = self.create_record(self.caregiver, category="water")
        self.caregiver.post(
            "/api/v1/records/%s/revoke" % doomed["id"], {"expected_version": 1}
        )
        payload = self.changes(0)
        self.assertEqual([doomed["id"]], payload["revoked"])
        self.assertNotIn(keep["id"], payload["revoked"])
        tombstone = [row for row in payload["records"] if row["id"] == doomed["id"]][-1]
        self.assertEqual("revoked", tombstone["st"])

    def test_cursor_past_the_end_is_409(self) -> None:
        for index in range(3):
            self.create_record(self.caregiver, category="meal")
        response = self.device.get("/api/v1/sync/changes?cursor=99999", token=self.token)
        self.assertEqual(409, response.status, response.text())
        self.assertEqual("cursor_invalid", response.json()["error"]["code"])

    def test_cursor_zero_replays_everything(self) -> None:
        created = [self.create_record(self.caregiver, category="meal") for _ in range(3)]
        payload = self.changes(0)
        self.assertEqual(set(record["id"] for record in created),
                         set(row["id"] for row in payload["records"]))

    def test_has_more_is_false_on_the_final_batch(self) -> None:
        for _ in range(2):
            self.create_record(self.caregiver, category="meal")
        payload = self.changes(0, limit=100)
        self.assertFalse(payload["has_more"])

    def test_cursor_advances_past_the_acknowledged_position(self) -> None:
        self.create_record(self.caregiver, category="meal")
        first = self.changes(0, limit=1)
        again = self.changes(first["cursor"], limit=100)
        first_ids = set(row["id"] for row in first["records"])
        again_ids = set(row["id"] for row in again["records"])
        self.assertEqual(set(), first_ids & again_ids)

    def test_invalid_query_parameters_are_422(self) -> None:
        for query in ("cursor=-1", "cursor=abc", "limit=0", "limit=101"):
            with self.subTest(query=query):
                response = self.device.get("/api/v1/sync/changes?%s" % query, token=self.token)
                self.assertEqual(422, response.status, response.text())

    def test_device_acknowledgement(self) -> None:
        self.create_record(self.caregiver, category="meal")
        payload = self.changes(0)
        ack = self.device.post(
            "/api/v1/sync/ack",
            {"cursor": payload["cursor"], "applied_at": payload["server_time"]},
            token=self.token,
        )
        self.assertEqual(200, ack.status, ack.text())
        body = ack.json()
        self.assertTrue(body["ok"])
        self.assertEqual(payload["cursor"], body["max_seq"])
        self.assertIn("utc_offset_minutes", body)
        self.assertIn(body["clock_skew_seconds"], (-1, 0, 1))

    def test_ack_rejects_a_bad_body(self) -> None:
        response = self.device.post("/api/v1/sync/ack", {"cursor": "abc"}, token=self.token)
        self.assertEqual(422, response.status, response.text())
        response = self.device.post(
            "/api/v1/sync/ack", {"cursor": 0, "applied_at": "yesterday"}, token=self.token
        )
        self.assertEqual(422, response.status, response.text())


class SyncSnapshotTest(LanlanTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.caregiver = self.caregiver("hehe")
        self.device, self.token = self.device_client("snapshot-passport")
        self.records = [
            self.create_record(
                self.caregiver,
                category="meal" if index % 2 == 0 else "water",
                amount_value=float(10 * (index + 1)),
                amount_unit="g" if index % 2 == 0 else "ml",
            )
            for index in range(5)
        ]
        self.revoked = self.records.pop()
        self.caregiver.post(
            "/api/v1/records/%s/revoke" % self.revoked["id"], {"expected_version": 1}
        )

    def snapshot(self, offset: int, limit: int) -> Dict[str, Any]:
        response = self.device.get(
            "/api/v1/sync/snapshot?offset=%d&limit=%d" % (offset, limit), token=self.token
        )
        self.assertEqual(200, response.status, response.text())
        return response.json()

    def test_snapshot_pages_equal_the_incremental_result(self) -> None:
        incremental = self.device.get(
            "/api/v1/sync/changes?cursor=0&limit=100", token=self.token
        ).json()
        expected = {}
        for row in incremental["records"]:
            if row["id"] not in incremental["revoked"]:
                expected[row["id"]] = row["v"]
        self.assertEqual(4, len(expected))

        collected: Dict[str, int] = {}
        offset = 0
        pages = 0
        while True:
            page = self.snapshot(offset, 2)
            self.assertEqual(2, page["limit"])
            self.assertEqual(4, page["total"])
            for row in page["records"]:
                collected[row["id"]] = row["v"]
            pages += 1
            if not page["has_more"]:
                self.assertEqual(incremental["cursor"], page["cursor"])
                break
            offset += 2
            self.assertLess(pages, 10)
        self.assertEqual(expected, collected)
        self.assertNotIn(self.revoked["id"], collected)

    def test_snapshot_order_is_stable_and_paged(self) -> None:
        first = self.snapshot(0, 2)
        again = self.snapshot(0, 2)
        self.assertEqual([row["id"] for row in first["records"]],
                         [row["id"] for row in again["records"]])
        # Snapshot order is (occurred_at, id) ascending, as the spec states.
        occurred = [row["at"] for row in first["records"]]
        self.assertEqual(sorted(occurred), occurred)
        self.assertTrue(first["has_more"])
        self.assertEqual(7, len(first["reminders"]))

    def test_snapshot_offset_past_the_end(self) -> None:
        page = self.snapshot(100, 2)
        self.assertEqual([], page["records"])
        self.assertFalse(page["has_more"])

    def test_snapshot_rejects_bad_parameters(self) -> None:
        for query in ("offset=-1", "limit=0", "limit=101"):
            with self.subTest(query=query):
                response = self.device.get("/api/v1/sync/snapshot?%s" % query, token=self.token)
                self.assertEqual(422, response.status, response.text())


class SyncMembersTest(LanlanTestCase):
    """The device learns caregiver labels from the sync response, not from order."""

    def setUp(self) -> None:
        super().setUp()
        self.caregiver = self.server.caregiver("hehe")
        self.device, self.token = self.device_client("members-passport")

    def test_both_sync_endpoints_carry_the_family_caregivers(self) -> None:
        changes = self.device.get("/api/v1/sync/changes?cursor=0", token=self.token).json()
        snapshot = self.device.get("/api/v1/sync/snapshot", token=self.token).json()
        expected_ids = [
            member["id"] for member in self.caregiver.get("/api/v1/me").json()["members"]
        ]

        for name, payload in (("changes", changes), ("snapshot", snapshot)):
            with self.subTest(endpoint=name):
                self.assertIn("members", payload)
                self.assertEqual(2, len(payload["members"]))
                self.assertEqual(expected_ids, [row["id"] for row in payload["members"]])
                self.assertEqual(["赫赫", "羊羊"],
                                 [row["display_name"] for row in payload["members"]])
                self.assertEqual(["hehe", "yangyang"],
                                 [row["username"] for row in payload["members"]])
        self.assertEqual(changes["members"], snapshot["members"])

    def test_member_entries_carry_only_three_fields(self) -> None:
        payload = self.device.get("/api/v1/sync/snapshot", token=self.token).json()
        for member in payload["members"]:
            with self.subTest(member=member.get("username")):
                self.assertEqual({"id", "display_name", "username"}, set(member.keys()))

    def test_members_are_stable_across_pages_and_after_new_changes(self) -> None:
        first = self.device.get("/api/v1/sync/snapshot?offset=0&limit=1", token=self.token).json()
        second = self.device.get("/api/v1/sync/snapshot?offset=1&limit=1", token=self.token).json()
        self.assertEqual(first["members"], second["members"])

        self.create_record(self.caregiver, category="meal")
        self.caregiver.patch(
            "/api/v1/reminders/%s" % self.caregiver.get("/api/v1/reminders").json()["reminders"][0]["id"],
            {"enabled": True, "time_local": "08:15"},
        )
        page = self.device.get("/api/v1/sync/changes?cursor=0&limit=1", token=self.token).json()
        rest = self.device.get(
            "/api/v1/sync/changes?cursor=%d&limit=100" % page["cursor"], token=self.token
        ).json()
        self.assertEqual(first["members"], page["members"])
        self.assertEqual(first["members"], rest["members"])

    def test_foreign_family_members_never_appear(self) -> None:
        self.server.init_database(
            family_name="Other family",
            passwords={"other": "other-password-1"},
            caregivers=[("other", "Other caregiver")],
            force=True,
        )
        connection = self.server.connect()
        try:
            other_family = connection.execute(
                "SELECT id FROM families WHERE name='Other family'"
            ).fetchone()["id"]
            _, other_token = admin.create_device(connection, other_family, "other-passport")
            family_ids = {
                row["id"] for row in connection.execute("SELECT id FROM users")
            }
        finally:
            connection.close()

        own = self.device.get("/api/v1/sync/changes?cursor=0", token=self.token).json()
        own_ids = {row["id"] for row in own["members"]}
        self.assertEqual(2, len(own_ids))
        self.assertNotIn("other", [row["username"] for row in own["members"]])

        other_device = self.server.client()
        foreign = other_device.get(
            "/api/v1/sync/changes?cursor=0", token=other_token
        ).json()
        foreign_ids = {row["id"] for row in foreign["members"]}
        self.assertEqual(1, len(foreign_ids))
        self.assertEqual(["other"], [row["username"] for row in foreign["members"]])
        self.assertEqual(set(), own_ids & foreign_ids)
        self.assertNotIn(own_ids, [foreign_ids])
        self.assertEqual(family_ids, own_ids | foreign_ids)

    def test_disabled_caregivers_leave_the_member_list(self) -> None:
        connection = self.server.connect()
        try:
            connection.execute(
                "UPDATE users SET disabled_at='2026-01-01T00:00:00Z' WHERE username='yangyang'"
            )
        finally:
            connection.close()
        payload = self.device.get("/api/v1/sync/changes?cursor=0", token=self.token).json()
        self.assertEqual(["hehe"], [row["username"] for row in payload["members"]])

    def test_no_account_secret_leaks_into_the_sync_payload(self) -> None:
        response = self.device.get("/api/v1/sync/changes?cursor=0", token=self.token)
        snapshot = self.device.get("/api/v1/sync/snapshot", token=self.token)
        for name, body in (("changes", response.body), ("snapshot", snapshot.body)):
            text = body.decode("utf-8").lower()
            for forbidden in (
                "password", "pbkdf2", "hash", "salt", "csrf", "session",
                "expires_at", "last_seen_at", "disabled_at", "email",
                "token_hash", "user_id",
            ):
                with self.subTest(endpoint=name, forbidden=forbidden):
                    self.assertNotIn(forbidden, text)

        # The device response keeps exactly the documented member keys, so no
        # account column can ride along unnoticed.
        for payload in (response.json(), snapshot.json()):
            for member in payload["members"]:
                self.assertEqual(3, len(member))


class ChangesPayloadUnitTest(LanlanTestCase):
    """Direct checks of the batch helper for the mixed-stream edge cases."""

    def test_trimmed_table_sets_has_more(self) -> None:
        caregiver = self.caregiver("hehe")
        for index in range(3):
            self.create_record(caregiver, category="meal", note="第 %d 笔" % index)
        family_id = self.server.family_id()
        connection = self.server.connect()
        try:
            payload = self.server.server.api._changes_payload(  # type: ignore[union-attr]
                connection, family_id, "Asia/Shanghai", 0, 1
            )
        finally:
            connection.close()
        self.assertEqual(1, len(payload["records"]) + len(payload["reminders"]))
        self.assertTrue(payload["has_more"])
        self.assertEqual(1, payload["cursor"])

    def test_query_rejects_a_malformed_page_cursor(self) -> None:
        client = self.caregiver("hehe")
        response = client.get("/api/v1/records?cursor=not-a-cursor")
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("cursor_invalid", response.json()["error"]["code"])


if __name__ == "__main__":
    unittest.main()
