"""Authorization: anonymous access, family scoping, device limits, CSRF, throttling."""

from __future__ import annotations

import unittest
from typing import List

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import CAREGIVER_PASSWORDS, LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import CAREGIVER_PASSWORDS, LanlanTestCase


WRITE_ENDPOINTS = (
    ("POST", "/api/v1/records", {"category": "meal"}),
    ("PATCH", "/api/v1/records/{record}", {"expected_version": 1, "category": "meal"}),
    ("POST", "/api/v1/records/{record}/revoke", {"expected_version": 1}),
    ("PATCH", "/api/v1/reminders/{reminder}", {"enabled": True, "time_local": "08:00"}),
    ("POST", "/api/v1/devices", {"label": "x"}),
    ("POST", "/api/v1/devices/{device}/revoke", {}),
    ("POST", "/api/v1/auth/logout", {}),
    ("POST", "/api/v1/auth/change-password", {"current_password": "x", "new_password": "yyyyyyyy"}),
    ("PATCH", "/api/v1/profile", {"pet_name": "懒懒"}),
)


class AnonymousAccessTest(LanlanTestCase):
    def test_record_endpoints_require_authentication(self) -> None:
        client = self.server.client()
        record = self.create_record(self.caregiver("hehe"), category="meal")
        paths = (
            "/api/v1/records",
            "/api/v1/records/%s" % record["id"],
            "/api/v1/summary/today",
            "/api/v1/reminders",
            "/api/v1/devices",
            "/api/v1/me",
            "/api/v1/export/records.csv",
            "/api/v1/export/records.json",
            "/api/v1/profile",
            "/api/v1/status",
        )
        for path in paths:
            with self.subTest(path=path):
                response = client.get(path)
                self.assertEqual(401, response.status, response.text())
                self.assertNotIn(b"amount_value", response.body)

    def test_anonymous_writes_are_rejected(self) -> None:
        client = self.server.client()
        for method, path, body in (
            ("POST", "/api/v1/records", {"category": "meal"}),
            ("PATCH", "/api/v1/records/x", {"expected_version": 1, "category": "meal"}),
            ("POST", "/api/v1/records/x/revoke", {"expected_version": 1}),
            ("POST", "/api/v1/devices", {"label": "x"}),
            ("POST", "/api/v1/auth/logout", {}),
        ):
            with self.subTest(path=path):
                response = client.request(method, path, body)
                self.assertEqual(401, response.status, response.text())


class FamilyScopingTest(LanlanTestCase):
    def setUp(self) -> None:
        super().setUp()
        # A second family in the same database: the spec allows more than one row.
        self.server.init_database(
            family_name="Other family",
            passwords={"other": "other-password-1"},
            force=True,
            caregivers=[("other", "Other caregiver")],
        )
        self.family_a = self.caregiver("hehe")
        self.record = self.create_record(self.family_a, category="meal", note="家庭 A 的记录")
        self.other = self.server.caregiver_in_family("other", "other-password-1")
        self.assertEqual("Other family", self.other.get("/api/v1/me").json()["family"]["name"])
        self.assertNotEqual(
            self.server.family_id(),
            self.other.get("/api/v1/me").json()["family"]["id"],
        )

    def test_other_family_cannot_read_a_record(self) -> None:
        response = self.other.get("/api/v1/records/%s" % self.record["id"])
        self.assertIn(response.status, (403, 404), response.text())

    def test_other_family_cannot_edit_or_revoke(self) -> None:
        edit = self.other.patch(
            "/api/v1/records/%s" % self.record["id"],
            {"expected_version": 1, "category": "meal"},
        )
        self.assertIn(edit.status, (403, 404), edit.text())
        revoke = self.other.post(
            "/api/v1/records/%s/revoke" % self.record["id"], {"expected_version": 1}
        )
        self.assertIn(revoke.status, (403, 404), revoke.text())
        self.assertEqual(
            "active",
            self.family_a.get("/api/v1/records/%s" % self.record["id"]).json()["record"]["status"],
        )

    def test_lists_are_family_scoped(self) -> None:
        listed = self.other.get("/api/v1/records").json()["records"]
        self.assertEqual([], [row for row in listed if row["id"] == self.record["id"]])
        summary = self.other.get("/api/v1/summary/today").json()
        self.assertEqual(0, summary["counts"]["meal"])

    def test_other_family_cannot_touch_a_device_of_this_family(self) -> None:
        created = self.family_a.post("/api/v1/devices", {"label": "family-a-passport"}).json()
        device_id = created["device"]["id"]
        # The family id is resolved from the session, so the other family sees nothing.
        response = self.other.post("/api/v1/devices/%s/revoke" % device_id, {})
        self.assertEqual(404, response.status, response.text())
        listed = self.family_a.get("/api/v1/devices").json()["devices"]
        self.assertIsNone(
            [row for row in listed if row["id"] == device_id][0]["revoked_at"]
        )
        self.assertEqual(
            404, self.other.patch("/api/v1/reminders/%s" % "missing", {"enabled": False}).status
        )


class DeviceCredentialTest(LanlanTestCase):
    def test_device_can_read_records_and_reminders(self) -> None:
        caregiver = self.caregiver("hehe")
        self.create_record(caregiver, category="meal")
        device, token = self.device_client()
        changes = device.get("/api/v1/sync/changes?cursor=0", token=token)
        self.assertEqual(200, changes.status, changes.text())
        self.assertGreaterEqual(len(changes.json()["records"]), 1)
        reminders = device.get("/api/v1/reminders", token=token)
        self.assertEqual(200, reminders.status, reminders.text())
        self.assertEqual(7, len(reminders.json()["reminders"]))

    def test_device_token_is_rejected_on_every_write_endpoint(self) -> None:
        caregiver = self.caregiver("hehe")
        record = self.create_record(caregiver, category="meal")
        reminder = caregiver.get("/api/v1/reminders").json()["reminders"][0]
        session = caregiver.post("/api/v1/devices", {"label": "victim"}).json()
        device, token = self.device_client("attacker")

        for method, template, body in WRITE_ENDPOINTS:
            path = template.format(record=record["id"], reminder=reminder["id"], device=session["device"]["id"])
            with self.subTest(path=path):
                response = device.request(method, path, body, token=token)
                self.assertEqual(403, response.status, response.text())
                self.assertEqual("caregiver_required", response.json()["error"]["code"])

    def test_revoked_device_token_stops_working(self) -> None:
        caregiver = self.caregiver("hehe")
        created = caregiver.post("/api/v1/devices", {"label": "to-revoke"}).json()
        device_id = created["device"]["id"]
        token = created["token"]
        device, _ = self.device_client("other")
        self.assertEqual(200, device.get("/api/v1/sync/changes?cursor=0", token=token).status)

        revoked = caregiver.post("/api/v1/devices/%s/revoke" % device_id, {})
        self.assertEqual(200, revoked.status, revoked.text())
        after = device.get("/api/v1/sync/changes?cursor=0", token=token)
        self.assertEqual(401, after.status, after.text())

    def test_device_management_needs_a_caregiver(self) -> None:
        device, token = self.device_client()
        self.assertEqual(403, device.get("/api/v1/devices", token=token).status)
        self.assertEqual(403, device.post("/api/v1/devices", {"label": "x"}, token=token).status)
        self.assertEqual(403, device.patch("/api/v1/profile", {"pet_name": "x"}, token=token).status)


class CsrfTest(LanlanTestCase):
    def test_write_without_csrf_header_is_rejected(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="meal")
        for method, template, body in WRITE_ENDPOINTS:
            path = template.format(record=record["id"], reminder="missing", device="missing")
            with self.subTest(path=path):
                response = client.request(method, path, body, csrf="")
                self.assertEqual(403, response.status, response.text())
                self.assertEqual("csrf_invalid", response.json()["error"]["code"])

    def test_write_with_a_wrong_csrf_header_is_rejected(self) -> None:
        client = self.caregiver("hehe")
        response = client.post("/api/v1/records", {"category": "meal"}, csrf="not-the-token")
        self.assertEqual(403, response.status, response.text())

    def test_write_without_origin_is_rejected(self) -> None:
        client = self.caregiver("hehe")
        response = client.post("/api/v1/records", {"category": "meal"}, send_origin=False)
        self.assertEqual(403, response.status, response.text())
        self.assertEqual("origin_required", response.json()["error"]["code"])

    def test_cross_origin_write_is_rejected(self) -> None:
        client = self.caregiver("hehe")
        response = client.post(
            "/api/v1/records", {"category": "meal"}, origin="http://evil.example"
        )
        self.assertEqual(403, response.status, response.text())

    def test_unknown_api_path_is_404(self) -> None:
        client = self.caregiver("hehe")
        self.assertEqual(404, client.get("/api/v1/nope").status)


class LoginThrottleTest(LanlanTestCase):
    def test_repeated_failures_are_throttled(self) -> None:
        """A04: failed logins back off instead of allowing unlimited guessing."""
        client = self.server.client()
        statuses: List[int] = []
        for _ in range(6):
            statuses.append(client.login("hehe", "wrong-password").status)
        self.assertEqual([401, 401, 401], statuses[:3], statuses)
        self.assertTrue(all(status == 429 for status in statuses[3:]), statuses)

        # Even the correct password is refused while the lockout holds.
        blocked = client.login("hehe", CAREGIVER_PASSWORDS["hehe"])
        self.assertEqual(429, blocked.status, blocked.text())

        # A different username from the same address is unaffected.
        other = self.server.client()
        self.assertEqual(200, other.login("yangyang").status)

    def test_successful_login_clears_the_counter(self) -> None:
        client = self.server.client()
        self.assertEqual(401, client.login("hehe", "nope-1").status)
        self.assertEqual(401, client.login("hehe", "nope-2").status)
        self.assertEqual(200, client.login("hehe").status)
        self.assertEqual(200, client.login("hehe").status)

    def test_throttling_still_allows_a_different_username(self) -> None:
        client = self.server.client()
        for _ in range(6):
            client.login("hehe", "bad")
        self.assertEqual(429, client.login("hehe", "bad").status)
        second = self.server.client()
        self.assertEqual(401, second.login("yangyang", "bad").status)


if __name__ == "__main__":
    unittest.main()
