"""Records API: categories, units, validation, revisions and creator/performer."""

from __future__ import annotations

import unittest
from datetime import timedelta
from typing import Any, Dict

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

from lanlan import model
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import LanlanTestCase


ALL_CATEGORY_CASES = (
    ("meal", None, "g", 120.0),
    ("meal", None, "ml", 1.0),
    ("meal", None, "scoop", 2.0),
    ("meal", None, "cup", 0.5),
    ("meal", None, "piece", 1.0),
    ("meal", None, "bag", 3.0),
    ("water", None, "ml", 200.0),
    ("water", None, "bowl", 1.0),
    ("care", "bath", None, None),
    ("care", "grooming", None, None),
    ("care", "teeth", None, None),
    ("care", "comb", None, None),
    ("care", "other", None, None),
    ("cleaning", "ear", None, None),
    ("cleaning", "paw", None, None),
    ("cleaning", "pad", None, None),
    ("cleaning", "litter", None, None),
    ("cleaning", "other", None, None),
    ("walk", None, None, None),
    ("other", None, None, None),
)


def payload_for(category: str, subitem: Any, unit: Any, amount: Any) -> Dict[str, Any]:
    body: Dict[str, Any] = {"category": category}
    if subitem:
        body["subitem"] = subitem
    if amount is not None:
        body["amount_value"] = amount
        body["amount_unit"] = unit
    if category == "care" and subitem == "other":
        body["custom_name"] = "滴眼药"
    if category == "cleaning" and subitem == "other":
        body["custom_name"] = "洗垫子"
    if category == "other":
        body["custom_name"] = "称体重"
    if category == "walk":
        body["duration_minutes"] = 25
    return body


class CategoryAndUnitTest(LanlanTestCase):
    def test_every_category_and_unit_combination_round_trips(self) -> None:
        client = self.caregiver("hehe")
        for category, subitem, unit, amount in ALL_CATEGORY_CASES:
            with self.subTest(category=category, subitem=subitem, unit=unit):
                body = payload_for(category, subitem, unit, amount)
                response = client.post("/api/v1/records", body)
                self.assertEqual(200, response.status, response.text())
                record = response.json()["record"]
                self.assertEqual(category, record["category"])
                self.assertEqual(subitem, record["subitem"])
                self.assertEqual(amount, record["amount_value"])
                self.assertEqual(unit, record["amount_unit"])
                self.assertEqual(1, record["version"])
                self.assertEqual("active", record["status"])
                self.assertEqual("manual", record["source"])

    def test_walk_duration_and_cleaning_label(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="walk", duration_minutes=90)
        self.assertEqual(90, record["duration_minutes"])

        record = self.create_record(client, category="cleaning", subitem="ear", custom_name="右耳")
        self.assertEqual("ear", record["subitem"])
        self.assertEqual("右耳", record["custom_name"])

    def test_care_other_requires_a_custom_name(self) -> None:
        client = self.caregiver("hehe")
        response = client.post("/api/v1/records", {"category": "care", "subitem": "other"})
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("custom_name", response.json()["error"]["field"])

    def test_other_requires_a_custom_name(self) -> None:
        client = self.caregiver("hehe")
        response = client.post("/api/v1/records", {"category": "other"})
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("custom_name", response.json()["error"]["field"])


class UnknownAmountTest(LanlanTestCase):
    def test_unknown_amount_stays_null_everywhere(self) -> None:
        """A02: an unknown quantity is never stored or reported as 0."""
        client = self.caregiver("hehe")
        response = client.post(
            "/api/v1/records", {"category": "meal", "amount_value": None, "amount_unit": None}
        )
        self.assertEqual(200, response.status, response.text())
        record = response.json()["record"]
        self.assertIsNone(record["amount_value"])
        self.assertIsNone(record["amount_unit"])
        record_id = record["id"]

        detail = client.get("/api/v1/records/%s" % record_id).json()["record"]
        self.assertIsNone(detail["amount_value"])

        listed = client.get("/api/v1/records").json()["records"]
        self.assertIsNone([row for row in listed if row["id"] == record_id][0]["amount_value"])

        connection = self.server.connect()
        try:
            stored = connection.execute(
                "SELECT amount_value, amount_unit FROM records WHERE id=?", (record_id,)
            ).fetchone()
        finally:
            connection.close()
        self.assertIsNone(stored["amount_value"])
        self.assertIsNone(stored["amount_unit"])

        device, token = self.device_client()
        sync = device.get("/api/v1/sync/changes?cursor=0", token=token).json()
        compact = [row for row in sync["records"] if row["id"] == record_id][0]
        self.assertNotIn("amt", compact)

    def test_water_without_amount_is_allowed(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="water")
        self.assertIsNone(record["amount_value"])


class CreatorPerformerTest(LanlanTestCase):
    def test_creator_and_performer_are_separate(self) -> None:
        """A02: the caregiver who records and the one who performed differ."""
        hehe = self.caregiver("hehe")
        yangyang = self.caregiver("yangyang")
        ids = self.members(hehe)

        record = self.create_record(
            yangyang, category="walk", duration_minutes=20, performed_by=ids["hehe"]
        )
        self.assertEqual(ids["yangyang"], record["created_by"])
        self.assertEqual(ids["hehe"], record["performed_by"])

        default = self.create_record(yangyang, category="water", amount_value=100.0, amount_unit="ml")
        self.assertEqual(ids["yangyang"], default["performed_by"])

    def test_performer_must_be_in_the_family(self) -> None:
        client = self.caregiver("hehe")
        response = client.post(
            "/api/v1/records", {"category": "meal", "performed_by": "00000000-0000-4000-8000-000000000000"}
        )
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("performed_by", response.json()["error"]["field"])
        self.assertEqual([], client.get("/api/v1/records").json()["records"])


class RevisionHistoryTest(LanlanTestCase):
    def test_edit_appends_a_revision_without_rewriting_history(self) -> None:
        hehe = self.caregiver("hehe")
        yangyang = self.caregiver("yangyang")
        ids = self.members(hehe)
        record = self.create_record(hehe, category="meal", amount_value=100.0, amount_unit="g")

        response = hehe.patch(
            "/api/v1/records/%s" % record["id"],
            {"expected_version": 1, "category": "meal", "amount_value": 150.0, "amount_unit": "g"},
        )
        self.assertEqual(200, response.status, response.text())
        self.assertEqual(2, response.json()["record"]["version"])

        history = hehe.get("/api/v1/records/%s" % record["id"]).json()
        self.assertEqual([2, 1], [row["version"] for row in history["revisions"]])
        self.assertEqual(100.0, history["revisions"][1]["amount_value"])
        self.assertEqual(150.0, history["revisions"][0]["amount_value"])

        # A third caregiver edit by a different account keeps both identities visible.
        response = yangyang.patch(
            "/api/v1/records/%s" % record["id"],
            {
                "expected_version": 2,
                "category": "meal",
                "amount_value": 150.0,
                "amount_unit": "g",
                "performed_by": ids["hehe"],
            },
        )
        self.assertEqual(200, response.status, response.text())
        self.assertEqual(3, response.json()["record"]["version"])
        self.assertEqual(ids["yangyang"], response.json()["record"]["created_by"])
        self.assertEqual(ids["hehe"], response.json()["record"]["performed_by"])
        self.assertEqual(3, self.server.record_revision_count(record["id"]))

    def test_an_edit_cannot_rewrite_the_performer_silently(self) -> None:
        hehe = self.caregiver("hehe")
        ids = self.members(hehe)
        record = self.create_record(hehe, category="walk", duration_minutes=10)
        response = hehe.patch(
            "/api/v1/records/%s" % record["id"],
            {
                "expected_version": 1,
                "category": "walk",
                "duration_minutes": 20,
                "performed_by": ids["yangyang"],
            },
        )
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("performed_by", response.json()["error"]["field"])
        self.assertEqual(1, self.server.record_revision_count(record["id"]))

        # Omitting the field keeps the stored performer and edits normally.
        ok = hehe.patch(
            "/api/v1/records/%s" % record["id"],
            {"expected_version": 1, "category": "walk", "duration_minutes": 20},
        )
        self.assertEqual(200, ok.status, ok.text())
        self.assertEqual(record["performed_by"], ok.json()["record"]["performed_by"])

    def test_stale_edit_returns_409_with_the_current_record(self) -> None:
        hehe = self.caregiver("hehe")
        record = self.create_record(hehe, category="water", amount_value=50.0, amount_unit="ml")
        first = hehe.patch(
            "/api/v1/records/%s" % record["id"],
            {"expected_version": 1, "category": "water", "amount_value": 60.0, "amount_unit": "ml"},
        )
        self.assertEqual(200, first.status)

        second = hehe.patch(
            "/api/v1/records/%s" % record["id"],
            {"expected_version": 1, "category": "water", "amount_value": 70.0, "amount_unit": "ml"},
        )
        self.assertEqual(409, second.status, second.text())
        body = second.json()
        self.assertEqual("version_conflict", body["error"]["code"])
        self.assertEqual(2, body["record"]["version"])
        self.assertEqual(60.0, body["record"]["amount_value"])
        self.assertEqual(2, self.server.record_revision_count(record["id"]))

    def test_missing_expected_version_is_422(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="meal")
        response = client.patch("/api/v1/records/%s" % record["id"], {"category": "meal"})
        self.assertEqual(422, response.status, response.text())


class ValidationTest(LanlanTestCase):
    def test_unknown_category_is_422(self) -> None:
        client = self.caregiver("hehe")
        response = client.post("/api/v1/records", {"category": "dance"})
        self.assertEqual(422, response.status, response.text())
        self.assertEqual("category", response.json()["error"]["field"])

    def test_subitem_rules(self) -> None:
        client = self.caregiver("hehe")
        cases = (
            ({"category": "care"}, "subitem"),
            ({"category": "care", "subitem": "claw"}, "subitem"),
            ({"category": "cleaning"}, "subitem"),
            ({"category": "cleaning", "subitem": "balcony"}, "subitem"),
            ({"category": "meal", "subitem": "bath"}, "subitem"),
        )
        for body, field in cases:
            with self.subTest(body=body):
                response = client.post("/api/v1/records", body)
                self.assertEqual(422, response.status, response.text())
                self.assertEqual(field, response.json()["error"]["field"])

    def test_bad_unit_is_422(self) -> None:
        client = self.caregiver("hehe")
        cases = (
            {"category": "meal", "amount_value": 10.0, "amount_unit": "bowl"},
            {"category": "water", "amount_value": 10.0, "amount_unit": "g"},
            {"category": "meal", "amount_value": 10.0, "amount_unit": "spoon"},
            {"category": "meal", "amount_value": 10.0},
            {"category": "care", "subitem": "bath", "amount_value": 10.0, "amount_unit": "ml"},
            {"category": "meal", "amount_unit": "g"},
        )
        for body in cases:
            with self.subTest(body=body):
                response = client.post("/api/v1/records", body)
                self.assertEqual(422, response.status, response.text())
                self.assertEqual("amount_unit", response.json()["error"]["field"])

    def test_amount_must_be_positive_and_bounded(self) -> None:
        client = self.caregiver("hehe")
        for value in (0, -1, -0.5, 10000, 9999.5):
            with self.subTest(value=value):
                response = client.post(
                    "/api/v1/records",
                    {"category": "meal", "amount_value": value, "amount_unit": "g"},
                )
                self.assertEqual(422, response.status, response.text())
                self.assertEqual("amount_value", response.json()["error"]["field"])

        response = client.post(
            "/api/v1/records", {"category": "meal", "amount_value": 9999, "amount_unit": "g"}
        )
        self.assertEqual(200, response.status, response.text())

    def test_note_length_limit(self) -> None:
        client = self.caregiver("hehe")
        ok = client.post("/api/v1/records", {"category": "meal", "note": "备" * 200})
        self.assertEqual(200, ok.status, ok.text())
        too_long = client.post("/api/v1/records", {"category": "meal", "note": "备" * 201})
        self.assertEqual(422, too_long.status, too_long.text())
        self.assertEqual("note", too_long.json()["error"]["field"])

    def test_occurred_at_range(self) -> None:
        client = self.caregiver("hehe")
        now = model.now_utc()
        too_old = model.format_utc(now - timedelta(days=401))
        too_new = model.format_utc(now + timedelta(days=2))
        for value in (too_old, too_new, "not-a-time"):
            with self.subTest(value=value):
                response = client.post("/api/v1/records", {"category": "meal", "occurred_at": value})
                self.assertEqual(422, response.status, response.text())
                self.assertEqual("occurred_at", response.json()["error"]["field"])

        backdated = model.format_utc(now - timedelta(days=3))
        response = client.post(
            "/api/v1/records",
            {"category": "meal", "occurred_at": backdated, "time_confidence": "estimated"},
        )
        self.assertEqual(200, response.status, response.text())
        record = response.json()["record"]
        self.assertEqual(backdated, record["occurred_at"])
        self.assertEqual("estimated", record["time_confidence"])

    def test_duration_rules(self) -> None:
        client = self.caregiver("hehe")
        cases = (
            ({"category": "walk", "duration_minutes": 0}, "duration_minutes"),
            ({"category": "walk", "duration_minutes": 1441}, "duration_minutes"),
            ({"category": "meal", "duration_minutes": 10}, "duration_minutes"),
        )
        for body, field in cases:
            with self.subTest(body=body):
                response = client.post("/api/v1/records", body)
                self.assertEqual(422, response.status, response.text())
                self.assertEqual(field, response.json()["error"]["field"])

    def test_custom_name_rules(self) -> None:
        client = self.caregiver("hehe")
        too_long = client.post(
            "/api/v1/records", {"category": "other", "custom_name": "一二三四五六七八九十十一十二十三"}
        )
        self.assertEqual(422, too_long.status, too_long.text())
        self.assertEqual("custom_name", too_long.json()["error"]["field"])

        not_allowed = client.post(
            "/api/v1/records", {"category": "meal", "custom_name": "加餐"}
        )
        self.assertEqual(422, not_allowed.status, not_allowed.text())
        self.assertEqual("custom_name", not_allowed.json()["error"]["field"])

    def test_invalid_state_never_stores_a_row(self) -> None:
        client = self.caregiver("hehe")
        for body in (
            {"category": "dance"},
            {"category": "meal", "amount_value": 0, "amount_unit": "g"},
            {"category": "meal", "note": "x" * 201},
        ):
            self.assertEqual(422, client.post("/api/v1/records", body).status)
        self.assertEqual([], client.get("/api/v1/records").json()["records"])


class RecordListTest(LanlanTestCase):
    def test_keyset_paging_and_filters(self) -> None:
        client = self.caregiver("hehe")
        now = model.now_utc()
        created = []
        for index in range(5):
            created.append(
                self.create_record(
                    client,
                    category="meal" if index % 2 == 0 else "water",
                    occurred_at=model.format_utc(now - timedelta(hours=index)),
                )
            )
        self.create_record(client, category="walk", duration_minutes=10,
                           occurred_at=model.format_utc(now - timedelta(hours=1)))

        page_one = client.get("/api/v1/records?limit=3").json()
        self.assertEqual(3, len(page_one["records"]))
        self.assertIsNotNone(page_one["next_cursor"])
        page_two = client.get(
            "/api/v1/records?limit=2&cursor=%s" % page_one["next_cursor"]
        ).json()
        # A second record with an identical timestamp exercises the id tiebreak.
        created.append(
            self.create_record(
                client, category="meal", occurred_at=model.format_utc(now)
            )
        )
        page_one = client.get("/api/v1/records?limit=3").json()
        page_two = client.get(
            "/api/v1/records?limit=3&cursor=%s" % page_one["next_cursor"]
        ).json()
        page_three = client.get(
            "/api/v1/records?limit=3&cursor=%s" % page_two["next_cursor"]
        ).json()
        seen = [
            row["id"]
            for page in (page_one, page_two, page_three)
            for row in page["records"]
        ]
        self.assertEqual(7, len(seen))
        self.assertEqual(7, len(set(seen)))
        self.assertIsNone(page_three["next_cursor"])

        meals = client.get("/api/v1/records?category=meal").json()["records"]
        self.assertTrue(all(row["category"] == "meal" for row in meals))

        today = now.strftime("%Y-%m-%d")
        filtered = client.get("/api/v1/records?from=%s&to=%s" % (today, today)).json()["records"]
        self.assertTrue(all(row["occurred_at"] >= model.format_utc(model.now_utc() - timedelta(days=1))
                            for row in filtered))

    def test_revoked_records_disappear_from_the_list(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(client, category="meal")
        keep = self.create_record(client, category="water")
        revoked = client.post(
            "/api/v1/records/%s/revoke" % record["id"], {"expected_version": 1, "reason": "误记"}
        )
        self.assertEqual(200, revoked.status, revoked.text())
        listed = [row["id"] for row in client.get("/api/v1/records").json()["records"]]
        self.assertEqual([keep["id"]], listed)
        detail = client.get("/api/v1/records/%s" % record["id"]).json()
        self.assertEqual("revoked", detail["record"]["status"])
        self.assertEqual("误记", detail["record"]["revoke_reason"])

    def test_unknown_record_is_404(self) -> None:
        client = self.caregiver("hehe")
        self.assertEqual(404, client.get("/api/v1/records/does-not-exist").status)

    def test_summary_today(self) -> None:
        client = self.caregiver("hehe")
        self.create_record(client, category="meal", amount_value=80.0, amount_unit="g")
        self.create_record(client, category="meal", amount_value=20.0, amount_unit="g")
        self.create_record(client, category="water", amount_value=100.0, amount_unit="ml")
        revoked = self.create_record(client, category="walk", duration_minutes=15)
        client.post("/api/v1/records/%s/revoke" % revoked["id"], {"expected_version": 1})

        summary = client.get("/api/v1/summary/today").json()
        self.assertEqual(2, summary["counts"]["meal"])
        self.assertEqual(1, summary["counts"]["water"])
        self.assertEqual(0, summary["counts"]["walk"])
        self.assertEqual(100.0, summary["amounts_by_unit"]["g"])
        self.assertIsNotNone(summary["latest"]["meal"])
        self.assertEqual("100.0 ml", "%s ml" % summary["latest"]["water"]["amt"])
        self.assertIsNone(summary["latest"]["walk"])
        self.assertIn("utc_offset_minutes", summary)

    def test_listing_requires_authentication(self) -> None:
        client = self.server.client()
        self.assertEqual(401, client.get("/api/v1/records").status)


if __name__ == "__main__":
    unittest.main()
