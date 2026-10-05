"""Concurrency: idempotent retries, single-winner edits and tombstones (A03)."""

from __future__ import annotations

import threading
import unittest
from typing import Any, List

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import Client, LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import Client, LanlanTestCase


class IdempotentCreateTest(LanlanTestCase):
    def test_sequential_replay_creates_exactly_one_record(self) -> None:
        client = self.caregiver("hehe")
        body = {
            "category": "meal",
            "amount_value": 100.0,
            "amount_unit": "g",
            "client_request_id": "retry-0001",
        }
        first = client.post("/api/v1/records", body)
        self.assertEqual(200, first.status, first.text())
        self.assertFalse(first.json()["idempotent_replay"])

        second = client.post("/api/v1/records", body)
        self.assertEqual(200, second.status, second.text())
        self.assertTrue(second.json()["idempotent_replay"])
        self.assertEqual(first.json()["record"]["id"], second.json()["record"]["id"])
        self.assertEqual(first.json()["record"]["version"], second.json()["record"]["version"])

        listed = client.get("/api/v1/records").json()["records"]
        self.assertEqual(1, len(listed))
        self.assertEqual(1, self.server.record_revision_count(listed[0]["id"]))

    def test_changed_body_with_the_same_request_id_conflicts(self) -> None:
        client = self.caregiver("hehe")
        body = {"category": "water", "amount_value": 50.0, "amount_unit": "ml",
                "client_request_id": "retry-0002"}
        first = client.post("/api/v1/records", body)
        body["amount_value"] = 500.0
        second = client.post("/api/v1/records", body)
        self.assertEqual(409, second.status)
        self.assertEqual("idempotency_conflict", second.json()["error"]["code"])
        self.assertEqual(first.json()["record"]["id"], second.json()["record"]["id"])
        self.assertEqual(50.0, second.json()["record"]["amount_value"])

    def test_parallel_replay_creates_exactly_one_record(self) -> None:
        first = self.caregiver("hehe")
        second = self.caregiver("yangyang")
        body = {"category": "care", "subitem": "bath", "client_request_id": "retry-parallel"}
        results: List[Any] = []
        lock = threading.Lock()
        barrier = threading.Barrier(2)

        def submit(client: Client) -> None:
            barrier.wait(timeout=5)
            response = client.post("/api/v1/records", body)
            with lock:
                results.append(response)

        threads = [threading.Thread(target=submit, args=(first,)),
                   threading.Thread(target=submit, args=(second,))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)

        self.assertEqual(2, len(results))
        statuses = sorted(response.status for response in results)
        self.assertEqual([200, 409], statuses)
        ids = {response.json()["record"]["id"] for response in results}
        self.assertEqual(1, len(ids))
        listed = first.get("/api/v1/records").json()["records"]
        self.assertEqual(1, len(listed))

    def test_many_parallel_creates_all_survive(self) -> None:
        client = self.caregiver("hehe")
        errors: List[int] = []
        lock = threading.Lock()

        def submit(index: int) -> None:
            response = client.post(
                "/api/v1/records",
                {"category": "meal", "client_request_id": "bulk-%03d" % index},
            )
            with lock:
                errors.append(response.status)

        threads = [threading.Thread(target=submit, args=(index,)) for index in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=15)
        self.assertEqual([200] * 12, sorted(errors))
        listed = client.get("/api/v1/records?limit=50").json()["records"]
        self.assertEqual(12, len(listed))


class ConcurrentEditTest(LanlanTestCase):
    def test_two_parallel_edits_produce_one_winner(self) -> None:
        hehe = self.caregiver("hehe")
        yangyang = self.caregiver("yangyang")
        record = self.create_record(hehe, category="meal", amount_value=100.0, amount_unit="g")

        results: List[Any] = []
        lock = threading.Lock()
        barrier = threading.Barrier(2)

        def edit(client: Client, amount: float) -> None:
            barrier.wait(timeout=5)
            response = client.patch(
                "/api/v1/records/%s" % record["id"],
                {"expected_version": 1, "category": "meal", "amount_value": amount,
                 "amount_unit": "g", "performed_by": record["performed_by"]},
            )
            with lock:
                results.append(response)

        threads = [
            threading.Thread(target=edit, args=(hehe, 150.0)),
            threading.Thread(target=edit, args=(yangyang, 200.0)),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)

        statuses = sorted(response.status for response in results)
        self.assertEqual([200, 409], statuses, [response.text() for response in results])

        winner = [response for response in results if response.status == 200][0].json()["record"]
        loser = [response for response in results if response.status == 409][0].json()
        self.assertEqual(2, winner["version"])
        self.assertEqual("version_conflict", loser["error"]["code"])
        self.assertEqual(2, loser["record"]["version"])
        self.assertEqual(winner["amount_value"], loser["record"]["amount_value"])
        self.assertEqual(2, self.server.record_revision_count(record["id"]))

    def test_version_chain_matches_the_number_of_accepted_edits(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="water", amount_value=10.0, amount_unit="ml")
        for version in range(1, 4):
            response = client.patch(
                "/api/v1/records/%s" % record["id"],
                {"expected_version": version, "category": "water",
                 "amount_value": float(version * 10), "amount_unit": "ml"},
            )
            self.assertEqual(200, response.status, response.text())
        history = client.get("/api/v1/records/%s" % record["id"]).json()
        self.assertEqual([4, 3, 2, 1], [row["version"] for row in history["revisions"]])


class TombstoneTest(LanlanTestCase):
    def test_revoked_id_is_delivered_once_and_never_resurrects(self) -> None:
        caregiver = self.caregiver("hehe")
        keep = self.create_record(caregiver, category="meal")
        doomed = self.create_record(caregiver, category="water", amount_value=20.0, amount_unit="ml")

        revoke = caregiver.post(
            "/api/v1/records/%s/revoke" % doomed["id"],
            {"expected_version": 1, "reason": "记错了"},
        )
        self.assertEqual(200, revoke.status, revoke.text())
        self.assertEqual("revoked", revoke.json()["record"]["status"])

        device, token = self.device_client()
        first_batch = device.get("/api/v1/sync/changes?cursor=0&limit=100", token=token).json()
        self.assertIn(doomed["id"], first_batch["revoked"])
        tombstone = [row for row in first_batch["records"] if row["id"] == doomed["id"]][-1]
        self.assertEqual("revoked", tombstone["st"])
        cursor = first_batch["cursor"]
        self.assertFalse(first_batch["has_more"])

        # Catching up again from the same cursor: the tombstone is already applied.
        second_batch = device.get(
            "/api/v1/sync/changes?cursor=%d&limit=100" % cursor, token=token
        ).json()
        self.assertEqual([], second_batch["revoked"])
        self.assertEqual(cursor, second_batch["cursor"])

        # A nested page must not resend the tombstone either.
        paged = device.get("/api/v1/sync/changes?cursor=0&limit=1", token=token).json()
        self.assertEqual(1, paged["cursor"])
        second_page = device.get(
            "/api/v1/sync/changes?cursor=%d&limit=100" % paged["cursor"], token=token
        ).json()
        self.assertEqual([doomed["id"]], second_page["revoked"])

        # The record never comes back into the list or the snapshot.
        listed = [row["id"] for row in caregiver.get("/api/v1/records").json()["records"]]
        self.assertEqual([keep["id"]], listed)
        snapshot = device.get("/api/v1/sync/snapshot?limit=100", token=token).json()
        self.assertNotIn(doomed["id"], [row["id"] for row in snapshot["records"]])

        # Revocation is a revision, not a delete.
        self.assertEqual(2, self.server.record_revision_count(doomed["id"]))

    def test_second_revoke_is_idempotent(self) -> None:
        caregiver = self.caregiver("hehe")
        record = self.create_record(caregiver, category="meal")
        first = caregiver.post(
            "/api/v1/records/%s/revoke" % record["id"], {"expected_version": 1}
        )
        self.assertEqual(200, first.status, first.text())
        second = caregiver.post(
            "/api/v1/records/%s/revoke" % record["id"], {"expected_version": 2}
        )
        self.assertEqual(200, second.status, second.text())
        self.assertTrue(second.json()["idempotent_replay"])
        self.assertEqual(2, self.server.record_revision_count(record["id"]))

    def test_parallel_revokes_produce_one_tombstone(self) -> None:
        hehe = self.caregiver("hehe")
        yangyang = self.caregiver("yangyang")
        record = self.create_record(hehe, category="walk", duration_minutes=30)
        results: List[Any] = []
        lock = threading.Lock()
        barrier = threading.Barrier(2)

        def revoke(client: Client) -> None:
            barrier.wait(timeout=5)
            response = client.post(
                "/api/v1/records/%s/revoke" % record["id"], {"expected_version": 1}
            )
            with lock:
                results.append(response)

        threads = [
            threading.Thread(target=revoke, args=(hehe,)),
            threading.Thread(target=revoke, args=(yangyang,)),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)

        statuses = sorted(response.status for response in results)
        self.assertEqual([200, 409], statuses, [response.text() for response in results])
        # Either way exactly one tombstone revision exists.
        self.assertEqual(2, self.server.record_revision_count(record["id"]))
        connection = self.server.connect()
        try:
            rows = list(
                connection.execute(
                    "SELECT version, status FROM records WHERE id=? ORDER BY version",
                    (record["id"],),
                )
            )
        finally:
            connection.close()
        self.assertEqual([(1, "active"), (2, "revoked")], [(row["version"], row["status"]) for row in rows])


if __name__ == "__main__":
    unittest.main()
