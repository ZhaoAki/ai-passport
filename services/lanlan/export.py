"""CSV and JSON exports for the Lanlan service.

Both writers include every revision of every record, revoked tombstones
included, so an export is a lossless copy of the revision log. The JSON form is
the lossless representation; the CSV form is the spreadsheet-friendly one.
"""

from __future__ import annotations

import csv
import io
import json
import sqlite3
from typing import Any, Dict, List, Optional, Tuple

from . import model

CSV_COLUMNS: Tuple[str, ...] = (
    "seq",
    "id",
    "version",
    "status",
    "category",
    "subitem",
    "custom_name",
    "occurred_at",
    "occurred_tz",
    "time_confidence",
    "created_at",
    "created_by",
    "created_by_name",
    "performed_by",
    "performed_by_name",
    "amount_value",
    "amount_unit",
    "duration_minutes",
    "note",
    "source",
    "client_request_id",
    "supersedes_seq",
    "revoke_reason",
)


def _display_names(connection: sqlite3.Connection, family_id: str) -> Dict[str, str]:
    rows = connection.execute(
        "SELECT id, display_name FROM users WHERE family_id=?", (family_id,)
    )
    return {row["id"]: row["display_name"] for row in rows}


def _records(connection: sqlite3.Connection, family_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT * FROM records WHERE family_id=? ORDER BY seq ASC", (family_id,)
        )
    )


def _reminders(connection: sqlite3.Connection, family_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT * FROM reminders WHERE family_id=? ORDER BY seq ASC", (family_id,)
        )
    )


def family_row(connection: sqlite3.Connection, family_id: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT id, name, timezone, created_at FROM families WHERE id=?", (family_id,)
    ).fetchone()


def export_csv(connection: sqlite3.Connection, family_id: str) -> str:
    display_names = _display_names(connection, family_id)
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(CSV_COLUMNS)
    for row in _records(connection, family_id):
        writer.writerow(
            [
                row["seq"],
                row["id"],
                row["version"],
                row["status"],
                row["category"],
                row["subitem"] or "",
                row["custom_name"] or "",
                row["occurred_at"],
                row["occurred_tz"],
                row["time_confidence"],
                row["created_at"],
                row["created_by"],
                display_names.get(row["created_by"], ""),
                row["performed_by"],
                display_names.get(row["performed_by"], ""),
                "" if row["amount_value"] is None else row["amount_value"],
                row["amount_unit"] or "",
                "" if row["duration_minutes"] is None else row["duration_minutes"],
                row["note"] or "",
                row["source"],
                row["client_request_id"] or "",
                "" if row["supersedes_seq"] is None else row["supersedes_seq"],
                row["revoke_reason"] or "",
            ]
        )
    return buffer.getvalue()


def export_json(connection: sqlite3.Connection, family_id: str) -> Dict[str, Any]:
    family = family_row(connection, family_id)
    display_names = _display_names(connection, family_id)
    members = [
        {"id": row["id"], "username": row["username"], "display_name": row["display_name"]}
        for row in connection.execute(
            "SELECT id, username, display_name FROM users WHERE family_id=? ORDER BY created_at",
            (family_id,),
        )
    ]
    records = []
    for row in _records(connection, family_id):
        entry = model.row_to_record(row)
        entry["created_by_name"] = display_names.get(row["created_by"])
        entry["performed_by_name"] = display_names.get(row["performed_by"])
        records.append(entry)
    profile = connection.execute(
        "SELECT pet_name, birthday, breed, weight_grams, notes, updated_at FROM pet_profile"
        " WHERE family_id=?",
        (family_id,),
    ).fetchone()
    return {
        "format": "lanlan-export",
        "format_version": 1,
        "generated_at": model.format_utc(model.now_utc()),
        "family": (
            None
            if family is None
            else {
                "id": family["id"],
                "name": family["name"],
                "timezone": family["timezone"],
                "created_at": family["created_at"],
            }
        ),
        "members": members,
        "records": records,
        "reminders": [model.reminder_to_json(row) for row in _reminders(connection, family_id)],
        "profile": (
            None
            if profile is None
            else {
                "pet_name": profile["pet_name"],
                "birthday": profile["birthday"],
                "breed": profile["breed"],
                "weight_grams": profile["weight_grams"],
                "notes": profile["notes"],
                "updated_at": profile["updated_at"],
            }
        ),
        "revision_count": len(records),
    }


def dumps_json(payload: Dict[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=False) + "\n"
