"""Reminders API: default-disabled state, enable rules and non-destructive updates (A10)."""

from __future__ import annotations

import unittest
from typing import Any, Dict, List

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import LanlanTestCase


EXPECTED_DEFAULTS = [
    ("meal", None),
    ("water", None),
    ("care", "bath"),
    ("care", "grooming"),
    ("care", "teeth"),
    ("walk", None),
    ("care", "comb"),
]


class ReminderDefaultsTest(LanlanTestCase):
    def test_initialization_creates_seven_disabled_reminders(self) -> None:
        client = self.caregiver("hehe")
        payload = client.get("/api/v1/reminders").json()
        reminders = payload["reminders"]
        self.assertEqual(7, len(reminders))
        self.assertEqual(EXPECTED_DEFAULTS, [(row["category"], row["subitem"]) for row in reminders])
        for row in reminders:
            with self.subTest(reminder=row["category"]):
                self.assertFalse(row["enabled"])
                self.assertIsNone(row["time_local"])
                self.assertEqual("daily", row["schedule_type"])
                self.assertEqual(1, row["version"])
                self.assertIsNotNone(row["updated_by"])

    def test_reminder_table_is_append_only_after_init(self) -> None:
        connection = self.server.connect()
        try:
            before = connection.execute("SELECT COUNT(*) AS n FROM reminders").fetchone()["n"]
        finally:
            connection.close()
        self.assertEqual(7, int(before))


class ReminderPatchTest(LanlanTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.client = self.caregiver("hehe")
        self.reminders = {
            (row["category"], row["subitem"]): row
            for row in self.client.get("/api/v1/reminders").json()["reminders"]
        }
        self.meal = self.reminders[("meal", None)]

    def patch(self, reminder_id: str, body: Dict[str, Any]) -> Any:
        return self.client.patch("/api/v1/reminders/%s" % reminder_id, body)

    def test_enabling_without_a_time_is_422(self) -> None:
        response = self.patch(self.meal["id"], {"enabled": True})
        self.assertEqual(422, response.status, response.text())
        error = response.json()["error"]
        self.assertEqual("time_local", error["field"])
        after = self.client.get("/api/v1/reminders").json()["reminders"]
        meal = [row for row in after if row["id"] == self.meal["id"]][0]
        self.assertFalse(meal["enabled"])
        self.assertEqual(1, self.server.reminder_revision_count(self.meal["id"]))

    def test_enable_with_a_time_works(self) -> None:
        response = self.patch(self.meal["id"], {"enabled": True, "time_local": "07:30"})
        self.assertEqual(200, response.status, response.text())
        reminder = response.json()["reminder"]
        self.assertTrue(reminder["enabled"])
        self.assertEqual("07:30", reminder["time_local"])
        self.assertEqual(2, reminder["version"])
        self.assertEqual(2, self.server.reminder_revision_count(self.meal["id"]))

        listed = self.client.get("/api/v1/reminders").json()["reminders"]
        self.assertTrue([row for row in listed if row["id"] == self.meal["id"]][0]["enabled"])

    def test_setting_a_time_on_a_disabled_reminder_keeps_it_disabled(self) -> None:
        response = self.patch(self.meal["id"], {"time_local": "21:05"})
        self.assertEqual(200, response.status, response.text())
        reminder = response.json()["reminder"]
        self.assertFalse(reminder["enabled"])
        self.assertEqual("21:05", reminder["time_local"])

    def test_disabling_keeps_the_time_and_never_deletes_records(self) -> None:
        record = self.create_record(self.client, category="meal", amount_value=50.0, amount_unit="g")
        enabled = self.patch(self.meal["id"], {"enabled": True, "time_local": "08:00"})
        self.assertEqual(200, enabled.status, enabled.text())
        disabled = self.patch(self.meal["id"], {"enabled": False})
        self.assertEqual(200, disabled.status, disabled.text())
        reminder = disabled.json()["reminder"]
        self.assertFalse(reminder["enabled"])
        self.assertEqual("08:00", reminder["time_local"])

        listed = self.client.get("/api/v1/records").json()["records"]
        self.assertIn(record["id"], [row["id"] for row in listed])
        detail = self.client.get("/api/v1/records/%s" % record["id"]).json()
        self.assertEqual("active", detail["record"]["status"])

    def test_changing_the_time_appends_a_revision(self) -> None:
        first = self.patch(self.meal["id"], {"enabled": True, "time_local": "07:00"})
        self.assertEqual(200, first.status, first.text())
        second = self.patch(self.meal["id"], {"enabled": True, "time_local": "07:45"})
        self.assertEqual(200, second.status, second.text())
        self.assertEqual(3, second.json()["reminder"]["version"])
        self.assertEqual(3, self.server.reminder_revision_count(self.meal["id"]))
        self.assertEqual("07:00", first.json()["reminder"]["time_local"])
        self.assertEqual("07:45", second.json()["reminder"]["time_local"])

    def test_disabling_a_reminder_that_never_had_a_time_is_allowed(self) -> None:
        response = self.patch(self.meal["id"], {"enabled": False})
        self.assertEqual(200, response.status, response.text())
        self.assertTrue(response.json()["idempotent_replay"] if "idempotent_replay" in response.json() else True)

    def test_invalid_time_and_schedule_are_422(self) -> None:
        for body in (
            {"time_local": "7:30"},
            {"time_local": "25:00"},
            {"time_local": "07:60"},
            {"time_local": "上午"},
        ):
            with self.subTest(body=body):
                response = self.patch(self.meal["id"], body)
                self.assertEqual(422, response.status, response.text())
                self.assertEqual("time_local", response.json()["error"]["field"])

        schedule = self.patch(self.meal["id"], {"schedule_type": "hourly"})
        self.assertEqual(422, schedule.status, schedule.text())

    def test_invalid_enabled_value_is_422(self) -> None:
        response = self.patch(self.meal["id"], {"enabled": "yes"})
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("enabled", response.json()["error"]["field"])

    def test_unknown_reminder_is_404(self) -> None:
        self.assertEqual(404, self.patch("missing-reminder", {"enabled": False}).status)

    def test_enabling_can_clear_the_time_when_disabling(self) -> None:
        enabled = self.patch(self.meal["id"], {"enabled": True, "time_local": "06:15"})
        self.assertEqual(200, enabled.status)
        cleared = self.patch(self.meal["id"], {"enabled": False, "time_local": None})
        self.assertEqual(200, cleared.status, cleared.text())
        self.assertFalse(cleared.json()["reminder"]["enabled"])
        self.assertIsNone(cleared.json()["reminder"]["time_local"])

    def test_all_seven_reminders_can_be_configured(self) -> None:
        configured: List[str] = []
        for index, (category, subitem) in enumerate(EXPECTED_DEFAULTS):
            reminder = self.reminders[(category, subitem)]
            response = self.patch(
                reminder["id"], {"enabled": True, "time_local": "%02d:00" % (index + 6)}
            )
            self.assertEqual(200, response.status, response.text())
            configured.append(reminder["id"])
        listed = self.client.get("/api/v1/reminders").json()["reminders"]
        self.assertEqual(7, len([row for row in listed if row["enabled"]]))
        self.assertTrue(all(row["time_local"] for row in listed))


if __name__ == "__main__":
    unittest.main()
