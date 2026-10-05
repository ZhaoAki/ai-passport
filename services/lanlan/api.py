"""JSON HTTP API and database operations for the Lanlan service.

The module is deliberately framework-free: :class:`Api` holds the routing and
response helpers, and every database helper takes the per-request connection as
its first argument. Only the standard library is used.
"""

from __future__ import annotations

import json
import sqlite3
from datetime import timedelta
from typing import Any, Dict, List, Optional, Sequence, Tuple
from urllib.parse import parse_qs, unquote, urlsplit

from . import auth, db as db_module, export as export_module, model
from .config import Config

# One second, used to keep the "to" filter inclusive of the whole local day.
_ONE_SECOND = timedelta(seconds=1)

MAX_BODY_BYTES = 1024 * 1024
MAX_SYNC_LIMIT = 100
MAX_LIST_LIMIT = 100


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str, field: Optional[str] = None) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.field = field

    def as_error(self) -> Dict[str, Any]:
        payload: Dict[str, Any] = {"code": self.code, "message": self.message}
        if self.field:
            payload["field"] = self.field
        return {"error": payload}


class Response:
    def __init__(
        self,
        status: int,
        body: bytes = b"",
        content_type: str = "application/json; charset=utf-8",
        headers: Optional[Sequence[Tuple[str, str]]] = None,
    ) -> None:
        self.status = status
        self.body = body
        self.content_type = content_type
        self.headers = list(headers or ())


class Request:
    def __init__(
        self,
        method: str,
        target: str,
        headers: Any,
        body: bytes,
        source_address: str = "",
    ) -> None:
        self.method = method.upper()
        self.target = target
        parts = urlsplit(target)
        self.path = parts.path
        self.query = parse_qs(parts.query)
        self.headers = headers
        self.body = body
        self.source_address = source_address

    def query_one(self, name: str) -> Optional[str]:
        values = self.query.get(name)
        if not values:
            return None
        return values[0]


# --------------------------------------------------------------------------
# JSON helpers
# --------------------------------------------------------------------------

def json_bytes(payload: Any) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def parse_json_object(body: bytes) -> Dict[str, Any]:
    if not body:
        raise ApiError(400, "invalid_body", "a JSON object body is required")
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError:
        raise ApiError(400, "invalid_body", "request body must be UTF-8")
    try:
        payload = json.loads(text)
    except ValueError:
        raise ApiError(400, "invalid_body", "request body must be valid JSON")
    if not isinstance(payload, dict):
        raise ApiError(400, "invalid_body", "request body must be a JSON object")
    return payload


def ok(payload: Any, status: int = 200, headers: Optional[Sequence[Tuple[str, str]]] = None) -> Response:
    return Response(status, json_bytes(payload), headers=headers)


def error_response(error: ApiError) -> Response:
    return Response(error.status, json_bytes(error.as_error()))


def field_error(error: model.FieldError) -> Response:
    return Response(422, json_bytes(error.as_error()))


# --------------------------------------------------------------------------
# Database reads
# --------------------------------------------------------------------------

def get_family(connection: sqlite3.Connection, family_id: str) -> Optional[sqlite3.Row]:
    return connection.execute("SELECT id, name, timezone FROM families WHERE id=?", (family_id,)).fetchone()


def family_members(connection: sqlite3.Connection, family_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT id, username, display_name, created_at, disabled_at FROM users"
            " WHERE family_id=? ORDER BY created_at ASC, username ASC, id ASC",
            (family_id,),
        )
    )


def sync_members(connection: sqlite3.Connection, family_id: str) -> List[Dict[str, Any]]:
    """Enabled caregivers of one family, in a stable order.

    This is the only account view a device credential receives: three fields per
    caregiver and nothing else. The list is identical on every sync response, so
    the passport can cache the id -> display name mapping once instead of
    inventing one from first-seen order.
    """
    rows = connection.execute(
        "SELECT id, username, display_name FROM users"
        " WHERE family_id=? AND disabled_at IS NULL"
        " ORDER BY created_at ASC, username ASC, id ASC",
        (family_id,),
    )
    return [
        {"id": row["id"], "display_name": row["display_name"], "username": row["username"]}
        for row in rows
    ]


def get_user_by_username(connection: sqlite3.Connection, username: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT id, family_id, username, display_name, password_hash, created_at, disabled_at"
        " FROM users WHERE username=?",
        (username,),
    ).fetchone()


def current_revision(connection: sqlite3.Connection, family_id: str, record_id: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT * FROM records WHERE family_id=? AND id=? ORDER BY version DESC LIMIT 1",
        (family_id, record_id),
    ).fetchone()


def record_revisions(connection: sqlite3.Connection, family_id: str, record_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT * FROM records WHERE family_id=? AND id=? ORDER BY version ASC, seq ASC",
            (family_id, record_id),
        )
    )


def list_current_records(
    connection: sqlite3.Connection,
    family_id: str,
    occurred_from: Optional[str] = None,
    occurred_to: Optional[str] = None,
    category: Optional[str] = None,
    cursor: Optional[Tuple[str, str]] = None,
    limit: int = 50,
) -> Tuple[List[sqlite3.Row], Optional[Tuple[str, str]]]:
    """Keyset page of current-state records in reverse-chronological order."""
    sql = (
        "SELECT * FROM records r WHERE r.family_id=? AND r.version=("
        " SELECT MAX(version) FROM records x WHERE x.family_id=r.family_id AND x.id=r.id)"
        " AND r.status=?"
    )
    params: List[Any] = [family_id, model.STATUS_ACTIVE]
    if occurred_from:
        sql += " AND r.occurred_at>=?"
        params.append(occurred_from)
    if occurred_to:
        sql += " AND r.occurred_at<?"
        params.append(occurred_to)
    if category:
        sql += " AND r.category=?"
        params.append(category)
    if cursor is not None:
        sql += " AND (r.occurred_at<? OR (r.occurred_at=? AND r.id<?))"
        params.extend([cursor[0], cursor[0], cursor[1]])
    sql += " ORDER BY r.occurred_at DESC, r.id DESC LIMIT ?"
    params.append(limit + 1)
    rows = list(connection.execute(sql, params))
    has_more = len(rows) > limit
    rows = rows[:limit]
    next_cursor = None
    if has_more and rows:
        next_cursor = (rows[-1]["occurred_at"], rows[-1]["id"])
    return rows, next_cursor


def recent_active_records(connection: sqlite3.Connection, family_id: str, limit: int = 5) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT * FROM records r WHERE r.family_id=? AND r.version=("
            " SELECT MAX(version) FROM records x WHERE x.family_id=r.family_id AND x.id=r.id)"
            " AND r.status=? ORDER BY r.occurred_at DESC, r.id DESC LIMIT ?",
            (family_id, model.STATUS_ACTIVE, limit),
        )
    )


def current_reminders(connection: sqlite3.Connection, family_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT * FROM reminders r WHERE r.family_id=? AND r.version=("
            " SELECT MAX(version) FROM reminders x WHERE x.family_id=r.family_id AND x.id=r.id)"
            " ORDER BY r.seq ASC",
            (family_id,),
        )
    )


def get_reminder(connection: sqlite3.Connection, family_id: str, reminder_id: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT * FROM reminders WHERE family_id=? AND id=? ORDER BY version DESC LIMIT 1",
        (family_id, reminder_id),
    ).fetchone()


def list_devices(connection: sqlite3.Connection, family_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT id, family_id, label, created_at, last_seen_at, revoked_at FROM devices"
            " WHERE family_id=? ORDER BY created_at, id",
            (family_id,),
        )
    )


def get_pet_profile(connection: sqlite3.Connection, family_id: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT family_id, pet_name, birthday, breed, weight_grams, notes, updated_at, updated_by"
        " FROM pet_profile WHERE family_id=?",
        (family_id,),
    ).fetchone()


def active_records_between(
    connection: sqlite3.Connection, family_id: str, start: str, end: str
) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT * FROM records r WHERE r.family_id=? AND r.version=("
            " SELECT MAX(version) FROM records x WHERE x.family_id=r.family_id AND x.id=r.id)"
            " AND r.status=? AND r.occurred_at>=? AND r.occurred_at<?"
            " ORDER BY r.occurred_at DESC, r.id DESC",
            (family_id, model.STATUS_ACTIVE, start, end),
        )
    )


def max_seq(connection: sqlite3.Connection) -> int:
    row = connection.execute(
        "SELECT MAX(value) AS m FROM ("
        " SELECT COALESCE(MAX(seq),0) AS value FROM records"
        " UNION ALL SELECT COALESCE(MAX(seq),0) AS value FROM reminders)"
    ).fetchone()
    return int(row["m"] or 0)


# --------------------------------------------------------------------------
# Database writes (always called inside one transaction)
# --------------------------------------------------------------------------

def insert_record_revision(
    connection: sqlite3.Connection,
    *,
    seq: int,
    record_id: str,
    version: int,
    family_id: str,
    fields: Dict[str, Any],
    created_at: str,
    created_by: str,
    performed_by: str,
    status: str,
    client_request_id: Optional[str],
    supersedes_seq: Optional[int],
    revoke_reason: Optional[str] = None,
) -> int:
    cursor = connection.execute(
        "INSERT INTO records(seq, id, version, family_id, category, subitem, custom_name,"
        " occurred_at, occurred_tz, time_confidence, created_at, created_by, performed_by,"
        " amount_value, amount_unit, duration_minutes, note, status, source,"
        " client_request_id, supersedes_seq, revoke_reason)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            seq,
            record_id,
            version,
            family_id,
            fields["category"],
            fields["subitem"],
            fields["custom_name"],
            fields["occurred_at"],
            fields["occurred_tz"],
            fields["time_confidence"],
            created_at,
            created_by,
            performed_by,
            fields["amount_value"],
            fields["amount_unit"],
            fields["duration_minutes"],
            fields["note"],
            status,
            model.SOURCE_MANUAL,
            client_request_id,
            supersedes_seq,
            revoke_reason,
        ),
    )
    return int(cursor.lastrowid)


def find_idempotent_record(
    connection: sqlite3.Connection, family_id: str, client_request_id: str
) -> Optional[str]:
    row = connection.execute(
        "SELECT record_id FROM idempotency WHERE family_id=? AND client_request_id=?",
        (family_id, client_request_id),
    ).fetchone()
    return None if row is None else str(row["record_id"])


def remember_idempotent(
    connection: sqlite3.Connection,
    family_id: str,
    client_request_id: str,
    record_id: str,
    created_at: str,
) -> None:
    connection.execute(
        "INSERT INTO idempotency(family_id, client_request_id, record_id, created_at)"
        " VALUES(?,?,?,?)",
        (family_id, client_request_id, record_id, created_at),
    )


def ensure_member(connection: sqlite3.Connection, family_id: str, user_id: str) -> None:
    row = connection.execute(
        "SELECT id FROM users WHERE id=? AND family_id=? AND disabled_at IS NULL",
        (user_id, family_id),
    ).fetchone()
    if row is None:
        raise model.FieldError("performed_by", "foreign_user", "performed_by is not in this family")


def member_ids(connection: sqlite3.Connection, family_id: str) -> List[str]:
    return [str(row["id"]) for row in family_members(connection, family_id)]


# --------------------------------------------------------------------------
# Serialisation helpers
# --------------------------------------------------------------------------

def user_json(row: Any) -> Dict[str, Any]:
    return {
        "id": row["id"],
        "username": row["username"],
        "display_name": row["display_name"],
    }


def device_json(row: Any) -> Dict[str, Any]:
    return {
        "id": row["id"],
        "label": row["label"],
        "created_at": row["created_at"],
        "last_seen_at": row["last_seen_at"],
        "revoked_at": row["revoked_at"],
        "revoked": bool(row["revoked_at"]),
    }


def profile_json(row: Optional[Any], family_id: str) -> Dict[str, Any]:
    if row is None:
        return {
            "family_id": family_id,
            "pet_name": None,
            "birthday": None,
            "breed": None,
            "weight_grams": None,
            "notes": None,
            "updated_at": None,
            "updated_by": None,
        }
    return {
        "family_id": row["family_id"],
        "pet_name": row["pet_name"],
        "birthday": row["birthday"],
        "breed": row["breed"],
        "weight_grams": row["weight_grams"],
        "notes": row["notes"],
        "updated_at": row["updated_at"],
        "updated_by": row["updated_by"],
    }


def summary_payload(
    family_id: str,
    day: str,
    timezone_name: str,
    rows: Sequence[sqlite3.Row],
    zone: Any,
) -> Dict[str, Any]:
    counts = {category: 0 for category in model.CATEGORIES}
    totals: Dict[str, float] = {}
    amounts: Dict[str, Optional[float]] = {}
    latest: Dict[str, Optional[sqlite3.Row]] = {"meal": None, "water": None, "walk": None}
    for row in rows:
        counts[row["category"]] = counts.get(row["category"], 0) + 1
        if row["amount_value"] is not None and row["amount_unit"]:
            unit = row["amount_unit"]
            totals[unit] = totals.get(unit, 0.0) + float(row["amount_value"])
            if unit not in amounts:
                amounts[unit] = 0.0
        if row["category"] in latest and latest[row["category"]] is None:
            latest[row["category"]] = row

    return {
        "date": day,
        "timezone": timezone_name,
        "counts": counts,
        "amounts_by_unit": totals,
        "latest": {
            key: (None if row is None else model.record_to_compact(row))
            for key, row in latest.items()
        },
        "recent": [model.record_to_compact(row) for row in rows[:5]],
    }


# --------------------------------------------------------------------------
# Router
# --------------------------------------------------------------------------

class Api:
    """Dispatch one parsed request to a handler."""

    def __init__(self, config: Config) -> None:
        self.config = config

    # -- entry point -------------------------------------------------------
    def handle(self, request: Request, connection: sqlite3.Connection) -> Response:
        try:
            identity = auth.authenticate(connection, request.headers, self.config.session_days)
            return self._route(request, connection, identity)
        except auth.AuthError as error:
            return Response(error.status, json_bytes(error.as_error()))
        except ApiError as error:
            return error_response(error)
        except model.FieldError as error:
            return field_error(error)
        except sqlite3.IntegrityError as error:
            return Response(
                409,
                json_bytes({"error": {"code": "conflict", "message": "conflicting write: %s" % error}}),
            )

    def _route(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        path = request.path
        if not path.startswith("/api/v1"):
            raise ApiError(404, "not_found", "no such endpoint")
        relative = path[len("/api/v1") :] or "/"
        method = request.method

        if relative == "/auth/login" and method == "POST":
            return self._login(request, connection)
        if relative == "/auth/logout" and method == "POST":
            return self._logout(request, connection, identity)
        if relative == "/auth/change-password" and method == "POST":
            return self._change_password(request, connection, identity)

        if relative == "/me" and method == "GET":
            return self._me(connection, identity)
        if relative == "/status" and method == "GET":
            return self._status(connection, identity)

        if relative == "/records":
            if method == "POST":
                return self._create_record(request, connection, identity)
            if method == "GET":
                return self._list_records(request, connection, identity)
        if relative.startswith("/records/"):
            remainder = relative[len("/records/") :]
            if remainder.endswith("/revoke") and method == "POST":
                return self._revoke_record(
                    request, connection, identity, unquote(remainder[: -len("/revoke")])
                )
            if method == "GET":
                return self._record_detail(connection, identity, unquote(remainder))
            if method == "PATCH":
                return self._edit_record(request, connection, identity, unquote(remainder))

        if relative == "/summary/today" and method == "GET":
            return self._summary_today(connection, identity)

        if relative == "/reminders" and method == "GET":
            return self._list_reminders(connection, identity)
        if relative.startswith("/reminders/") and method == "PATCH":
            return self._patch_reminder(
                request, connection, identity, unquote(relative[len("/reminders/") :])
            )

        if relative == "/export/records.csv" and method == "GET":
            return self._export(connection, identity, "csv")
        if relative == "/export/records.json" and method == "GET":
            return self._export(connection, identity, "json")

        if relative == "/devices":
            if method == "POST":
                return self._create_device(request, connection, identity)
            if method == "GET":
                return self._list_devices(connection, identity)
        if relative.startswith("/devices/") and relative.endswith("/revoke") and method == "POST":
            return self._revoke_device(
                request, connection, identity, unquote(relative[len("/devices/") : -len("/revoke")])
            )

        if relative == "/profile":
            if method == "GET":
                return self._get_profile(connection, identity)
            if method == "PATCH":
                return self._patch_profile(request, connection, identity)

        if relative == "/sync/changes" and method == "GET":
            return self._sync_changes(request, connection, identity)
        if relative == "/sync/snapshot" and method == "GET":
            return self._sync_snapshot(request, connection, identity)
        if relative == "/sync/ack" and method == "POST":
            return self._sync_ack(request, connection, identity)

        raise ApiError(404, "not_found", "no such endpoint")

    # -- helpers -----------------------------------------------------------
    def _origin_allowed(self, request: Request) -> None:
        origin = request.headers.get("Origin")
        host = request.headers.get("Host")
        if not origin:
            raise ApiError(403, "origin_required", "state-changing requests need an Origin header")
        if not host:
            raise ApiError(403, "origin_invalid", "the Host header is required")
        parts = urlsplit(origin)
        if parts.scheme not in ("http", "https") or parts.netloc != host:
            raise ApiError(403, "origin_invalid", "the Origin header is not same-origin")

    def _family(self, connection: sqlite3.Connection, family_id: str) -> sqlite3.Row:
        row = get_family(connection, family_id)
        if row is None:
            raise ApiError(404, "family_not_found", "the family does not exist")
        return row

    def _storage_mode(self) -> str:
        return "production" if self.config.production else "development"

    # -- auth --------------------------------------------------------------
    def _login(self, request: Request, connection: sqlite3.Connection) -> Response:
        payload = parse_json_object(request.body)
        username = payload.get("username")
        password = payload.get("password")
        if not isinstance(username, str) or not username.strip():
            raise ApiError(400, "invalid_body", "username is required", "username")
        if not isinstance(password, str) or not password:
            raise ApiError(400, "invalid_body", "password is required", "password")

        key = auth.throttle_key(request.source_address, username)
        remaining = auth.throttled_for(connection, key)
        if remaining is not None:
            raise ApiError(
                429,
                "throttled",
                "too many failed attempts; retry in %d seconds" % remaining,
            )

        row = get_user_by_username(connection, username.strip())
        if row is None or row["disabled_at"] or not auth.verify_password(row["password_hash"], password):
            lock = auth.record_login_failure(connection, key)
            if lock is not None:
                raise ApiError(
                    429, "throttled", "too many failed attempts; retry in %d seconds" % lock
                )
            raise ApiError(401, "invalid_credentials", "username or password is incorrect")

        auth.clear_login_failures(connection, key)
        token, csrf = auth.create_session(connection, row["id"], self.config.session_days)
        family = self._family(connection, row["family_id"])
        cookie = self._session_cookie(token)
        body = {
            "user": user_json(row),
            "family": {
                "id": family["id"],
                "name": family["name"],
                "timezone": family["timezone"],
            },
            "csrf_token": csrf,
            "members": [user_json(member) for member in family_members(connection, row["family_id"])],
            "server_time": model.format_utc(model.now_utc()),
            "storage_mode": self._storage_mode(),
        }
        return ok(body, headers=[("Set-Cookie", cookie)])

    def _session_cookie(self, token: str) -> str:
        parts = [
            "%s=%s" % (auth.SESSION_COOKIE_NAME, token),
            "Path=/",
            "HttpOnly",
            "SameSite=Lax",
        ]
        if self.config.secure_cookies:
            parts.append("Secure")
        return "; ".join(parts)

    def _clear_cookie(self) -> str:
        parts = [
            "%s=" % auth.SESSION_COOKIE_NAME,
            "Path=/",
            "HttpOnly",
            "SameSite=Lax",
            "Max-Age=0",
        ]
        if self.config.secure_cookies:
            parts.append("Secure")
        return "; ".join(parts)

    def _logout(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        if identity.session_hash:
            auth.delete_session(connection, identity.session_hash)
        return ok({"ok": True}, headers=[("Set-Cookie", self._clear_cookie())])

    def _change_password(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body)
        current = payload.get("current_password")
        new_password = payload.get("new_password")
        row = connection.execute(
            "SELECT password_hash FROM users WHERE id=?", (identity.user_id,)
        ).fetchone()
        if row is None:
            raise ApiError(404, "user_not_found", "the caregiver does not exist")
        if not isinstance(current, str) or not auth.verify_password(row["password_hash"], current):
            raise ApiError(401, "invalid_credentials", "the current password is incorrect")
        if not isinstance(new_password, str):
            raise ApiError(400, "invalid_body", "new_password is required", "new_password")
        auth.validate_password_strength(new_password)
        connection.execute(
            "UPDATE users SET password_hash=? WHERE id=?",
            (auth.hash_password(new_password, self.config.pbkdf2_iterations), identity.user_id),
        )
        if identity.session_hash:
            connection.execute(
                "DELETE FROM sessions WHERE user_id=? AND token_hash<>?",
                (identity.user_id, identity.session_hash),
            )
        return ok({"ok": True})

    def _me(self, connection: sqlite3.Connection, identity: Optional[auth.Identity]) -> Response:
        if identity is None:
            raise ApiError(401, "unauthenticated", "authentication is required")
        family = self._family(connection, identity.family_id)
        zone = model.load_timezone(family["timezone"])
        members = [user_json(row) for row in family_members(connection, identity.family_id)]
        caregiver = None
        if identity.user_id:
            for member in members:
                if member["id"] == identity.user_id:
                    caregiver = member
        return ok(
            {
                "user": caregiver,
                "role": identity.kind,
                "family": {
                    "id": family["id"],
                    "name": family["name"],
                    "timezone": family["timezone"],
                },
                "members": members,
                "server_time": model.format_utc(model.now_utc()),
                "local_date": model.today_in_zone(zone),
                "utc_offset_minutes": model.offset_minutes(zone, model.now_utc()),
                "storage_mode": self._storage_mode(),
            }
        )

    def _status(self, connection: sqlite3.Connection, identity: Optional[auth.Identity]) -> Response:
        identity = auth.require_any(identity)
        family = self._family(connection, identity.family_id)
        zone = model.load_timezone(family["timezone"])
        now = model.now_utc()
        record_count = 0
        if identity.is_caregiver:
            row = connection.execute(
                "SELECT COUNT(*) AS n FROM records WHERE family_id=? AND version=("
                " SELECT MAX(version) FROM records x WHERE x.family_id=records.family_id AND x.id=records.id)"
                " AND status=?",
                (identity.family_id, model.STATUS_ACTIVE),
            ).fetchone()
            record_count = int(row["n"])
        devices = list_devices(connection, identity.family_id)
        last_sync = None
        for device in devices:
            if device["last_seen_at"] and (last_sync is None or device["last_seen_at"] > last_sync):
                last_sync = device["last_seen_at"]
        return ok(
            {
                "server_time": model.format_utc(now),
                "timezone": family["timezone"],
                "utc_offset_minutes": model.offset_minutes(zone, now),
                "storage_mode": self._storage_mode(),
                "schema_version": connection.execute("PRAGMA user_version").fetchone()[0],
                "max_seq": max_seq(connection),
                "active_records": record_count,
                "device_count": len(devices),
                "last_device_sync_at": last_sync,
            }
        )

    # -- records -----------------------------------------------------------
    def _prepare_record_payload(
        self,
        connection: sqlite3.Connection,
        identity: auth.Identity,
        payload: Dict[str, Any],
        family_timezone: str,
    ) -> Dict[str, Any]:
        fields = model.build_record_payload(payload, family_timezone)
        performed_by = payload.get("performed_by")
        if performed_by is None or performed_by == "":
            performed_by_id = identity.user_id
        elif isinstance(performed_by, str):
            performed_by_id = performed_by
        else:
            raise model.FieldError("performed_by", "invalid", "performed_by must be a user id")
        ensure_member(connection, identity.family_id, performed_by_id)
        fields["performed_by"] = performed_by_id
        return fields

    def _create_record(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body)
        family = self._family(connection, identity.family_id)

        client_request_id = payload.get("client_request_id")
        if client_request_id is not None and not isinstance(client_request_id, str):
            raise model.FieldError("client_request_id", "invalid", "client_request_id must be a string")
        client_request_id = (client_request_id or "").strip() or None
        if client_request_id is not None and len(client_request_id) > 64:
            raise model.FieldError(
                "client_request_id", "too_long", "client_request_id must be at most 64 characters"
            )

        if client_request_id is not None:
            existing_id = find_idempotent_record(connection, identity.family_id, client_request_id)
            if existing_id is not None:
                row = current_revision(connection, identity.family_id, existing_id)
                if row is not None:
                    original = connection.execute(
                        "SELECT * FROM records WHERE family_id=? AND id=? AND version=1",
                        (identity.family_id, existing_id),
                    ).fetchone()
                    candidate = dict(payload)
                    # An omitted time is assigned on the first request; a retry
                    # must compare against that original assignment.
                    if not candidate.get("occurred_at"):
                        candidate["occurred_at"] = original["occurred_at"]
                    fields = self._prepare_record_payload(connection, identity, candidate, family["timezone"])
                    if original["created_by"] != identity.user_id or any(
                        original[key] != value for key, value in fields.items()
                    ):
                        return ok({"error": {"code": "idempotency_conflict",
                                             "message": "This request id already saved different content."},
                                   "record": model.row_to_record(row)}, status=409)
                    return ok(
                        {"record": model.row_to_record(row), "idempotent_replay": True},
                        status=200,
                    )

        fields = self._prepare_record_payload(connection, identity, payload, family["timezone"])
        performed_by = fields.pop("performed_by")
        record_id = auth.new_id()
        created_at = model.format_utc(model.now_utc())
        seq = insert_record_revision(
            connection,
            seq=db_module.allocate_seq(connection, created_at),
            record_id=record_id,
            version=1,
            family_id=identity.family_id,
            fields=fields,
            created_at=created_at,
            created_by=identity.user_id,
            performed_by=performed_by,
            status=model.STATUS_ACTIVE,
            client_request_id=client_request_id,
            supersedes_seq=None,
        )
        if client_request_id is not None:
            remember_idempotent(
                connection, identity.family_id, client_request_id, record_id, created_at
            )
        row = current_revision(connection, identity.family_id, record_id)
        assert row is not None
        return ok(
            {"record": model.row_to_record(row), "idempotent_replay": False, "seq": seq},
            status=200,
        )

    def _list_records(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        limit = model.validate_limit(request.query_one("limit"), default=50, maximum=MAX_LIST_LIMIT)
        family_timezone = self._family(connection, identity.family_id)["timezone"]
        category = request.query_one("category")
        if category is not None and category not in model.CATEGORIES:
            raise ApiError(422, "invalid_category", "unknown category: %s" % category, "category")
        occurred_from = self._date_boundary(
            request.query_one("from"), "from", end_of_day=False, family_timezone=family_timezone
        )
        occurred_to = self._date_boundary(
            request.query_one("to"), "to", end_of_day=True, family_timezone=family_timezone
        )
        raw_cursor = request.query_one("cursor")
        cursor = None
        if raw_cursor:
            cursor = model.decode_page_cursor(raw_cursor)
            if cursor is None:
                raise ApiError(422, "cursor_invalid", "the page cursor is malformed", "cursor")
        rows, next_cursor = list_current_records(
            connection,
            identity.family_id,
            occurred_from=occurred_from,
            occurred_to=occurred_to,
            category=category,
            cursor=cursor,
            limit=limit,
        )
        return ok(
            {
                "records": [model.row_to_record(row) for row in rows],
                "next_cursor": (
                    None if next_cursor is None else model.encode_page_cursor(next_cursor[0], next_cursor[1])
                ),
                "limit": limit,
            }
        )

    def _date_boundary(
        self, raw: Optional[str], field: str, end_of_day: bool, family_timezone: str
    ) -> Optional[str]:
        """Accept a full RFC 3339 timestamp or a bare local date.

        A bare date bounds the family-local day, which is what the mobile page
        sends; a full timestamp is used as given.
        """
        if raw is None or raw == "":
            return None
        moment = model.try_parse_timestamp(raw)
        if moment is not None and ("T" in raw or " " in raw):
            return model.format_utc(moment)
        if model.valid_local_date(raw):
            start, end = model.local_date_bounds(raw, model.load_timezone(family_timezone))
            boundary = (end - _ONE_SECOND) if end_of_day else start
            return model.format_utc(boundary)
        raise ApiError(422, "invalid_time", "%s must be an RFC 3339 timestamp or a date" % field, field)

    def _record_detail(
        self, connection: sqlite3.Connection, identity: Optional[auth.Identity], record_id: str
    ) -> Response:
        identity = auth.require_caregiver(identity)
        revisions = record_revisions(connection, identity.family_id, record_id)
        if not revisions:
            raise ApiError(404, "record_not_found", "no such record")
        return ok(
            {
                "record": model.row_to_record(revisions[-1]),
                "revisions": [model.row_to_record(row) for row in reversed(revisions)],
            }
        )

    def _edit_record(
        self,
        request: Request,
        connection: sqlite3.Connection,
        identity: Optional[auth.Identity],
        record_id: str,
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body)
        expected = model.expected_version(payload)
        family = self._family(connection, identity.family_id)
        current = current_revision(connection, identity.family_id, record_id)
        if current is None:
            raise ApiError(404, "record_not_found", "no such record")
        if int(current["version"]) != expected:
            return ok(
                {
                    "error": {
                        "code": "version_conflict",
                        "message": "the record was changed by someone else",
                        "field": "expected_version",
                    },
                    "record": model.row_to_record(current),
                },
                status=409,
            )
        if current["status"] == model.STATUS_REVOKED:
            raise ApiError(409, "record_revoked", "a revoked record cannot be edited")

        fields = self._prepare_record_payload(connection, identity, payload, family["timezone"])
        performed_by = fields.pop("performed_by")
        # Creator and performer are history: an edit may repeat the stored
        # performer but never silently rewrite it.
        if performed_by != current["performed_by"]:
            raise model.FieldError(
                "performed_by",
                "immutable",
                "the performer cannot be changed by an edit; record a new entry instead",
            )
        created_at = model.format_utc(model.now_utc())
        insert_record_revision(
            connection,
            seq=db_module.allocate_seq(connection, created_at),
            record_id=record_id,
            version=expected + 1,
            family_id=identity.family_id,
            fields=fields,
            created_at=created_at,
            created_by=identity.user_id,
            performed_by=performed_by,
            status=model.STATUS_ACTIVE,
            client_request_id=None,
            supersedes_seq=int(current["seq"]),
        )
        row = current_revision(connection, identity.family_id, record_id)
        assert row is not None
        return ok({"record": model.row_to_record(row), "idempotent_replay": False})

    def _revoke_record(
        self,
        request: Request,
        connection: sqlite3.Connection,
        identity: Optional[auth.Identity],
        record_id: str,
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body) if request.body else {}
        expected = model.expected_version(payload)
        reason = model.optional_str(payload, "reason", 100)
        current = current_revision(connection, identity.family_id, record_id)
        if current is None:
            raise ApiError(404, "record_not_found", "no such record")
        if int(current["version"]) != expected:
            return ok(
                {
                    "error": {
                        "code": "version_conflict",
                        "message": "the record was changed by someone else",
                        "field": "expected_version",
                    },
                    "record": model.row_to_record(current),
                },
                status=409,
            )
        if current["status"] == model.STATUS_REVOKED:
            return ok({"record": model.row_to_record(current), "idempotent_replay": True})

        created_at = model.format_utc(model.now_utc())
        connection.execute(
            "INSERT INTO records(seq, id, version, family_id, category, subitem, custom_name,"
            " occurred_at, occurred_tz, time_confidence, created_at, created_by, performed_by,"
            " amount_value, amount_unit, duration_minutes, note, status, source,"
            " client_request_id, supersedes_seq, revoke_reason)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                db_module.allocate_seq(connection, created_at),
                current["id"],
                expected + 1,
                identity.family_id,
                current["category"],
                current["subitem"],
                current["custom_name"],
                current["occurred_at"],
                current["occurred_tz"],
                current["time_confidence"],
                created_at,
                identity.user_id,
                current["performed_by"],
                current["amount_value"],
                current["amount_unit"],
                current["duration_minutes"],
                current["note"],
                model.STATUS_REVOKED,
                model.SOURCE_MANUAL,
                None,
                int(current["seq"]),
                reason,
            ),
        )
        row = current_revision(connection, identity.family_id, record_id)
        assert row is not None
        return ok({"record": model.row_to_record(row), "idempotent_replay": False})

    def _summary_today(
        self, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        family = self._family(connection, identity.family_id)
        zone = model.load_timezone(family["timezone"])
        return self._summary_for(connection, identity.family_id, family["timezone"], zone, None)

    def _summary_for(
        self,
        connection: sqlite3.Connection,
        family_id: str,
        timezone_name: str,
        zone: Any,
        day: Optional[str],
    ) -> Response:
        target_day = day or model.today_in_zone(zone)
        start, end = model.local_date_bounds(target_day, zone)
        rows = active_records_between(
            connection, family_id, model.format_utc(start), model.format_utc(end)
        )
        payload = summary_payload(family_id, target_day, timezone_name, rows, zone)
        payload["server_time"] = model.format_utc(model.now_utc())
        payload["utc_offset_minutes"] = model.offset_minutes(zone, model.now_utc())
        return ok(payload)

    # -- reminders ---------------------------------------------------------
    def _list_reminders(
        self, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_any(identity)
        rows = current_reminders(connection, identity.family_id)
        family = self._family(connection, identity.family_id)
        return ok(
            {
                "reminders": [
                    {
                        "id": row["id"],
                        "category": row["category"],
                        "subitem": row["subitem"],
                        "custom_name": row["custom_name"],
                        "enabled": bool(row["enabled"]),
                        "schedule_type": row["schedule_type"],
                        "time_local": row["time_local"],
                        "version": row["version"],
                        "updated_at": row["updated_at"],
                        "updated_by": row["updated_by"],
                    }
                    for row in rows
                ],
                "timezone": family["timezone"],
            }
        )

    def _patch_reminder(
        self,
        request: Request,
        connection: sqlite3.Connection,
        identity: Optional[auth.Identity],
        reminder_id: str,
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body)
        family = self._family(connection, identity.family_id)
        current = get_reminder(connection, identity.family_id, reminder_id)
        if current is None:
            raise ApiError(404, "reminder_not_found", "no such reminder")

        raw_enabled = payload.get("enabled")
        if raw_enabled is None:
            enabled = bool(current["enabled"])
        elif isinstance(raw_enabled, bool):
            enabled = raw_enabled
        else:
            raise model.FieldError("enabled", "invalid", "enabled must be a boolean")

        if "time_local" in payload:
            time_local = model.validate_time_local(payload.get("time_local"))
        else:
            time_local = current["time_local"]

        schedule_type = model.validate_schedule(payload) if "schedule_type" in payload else current["schedule_type"]
        if enabled and not time_local:
            raise model.FieldError(
                "time_local", "required_when_enabled", "a reminder needs a time before it can be enabled"
            )

        if bool(current["enabled"]) == enabled and current["time_local"] == time_local:
            return ok({"reminder": self._reminder_json(current), "idempotent_replay": True})

        now = model.format_utc(model.now_utc())
        connection.execute(
            "INSERT INTO reminders(seq, id, version, family_id, category, subitem, custom_name,"
            " enabled, schedule_type, time_local, created_at, updated_at, updated_by)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                db_module.allocate_seq(connection, now),
                current["id"],
                int(current["version"]) + 1,
                identity.family_id,
                current["category"],
                current["subitem"],
                current["custom_name"],
                1 if enabled else 0,
                schedule_type,
                time_local,
                current["created_at"],
                now,
                identity.user_id,
            ),
        )
        row = get_reminder(connection, identity.family_id, reminder_id)
        assert row is not None
        return ok({"reminder": self._reminder_json(row)})

    def _reminder_json(self, row: Any) -> Dict[str, Any]:
        return {
            "id": row["id"],
            "category": row["category"],
            "subitem": row["subitem"],
            "custom_name": row["custom_name"],
            "enabled": bool(row["enabled"]),
            "schedule_type": row["schedule_type"],
            "time_local": row["time_local"],
            "version": row["version"],
            "updated_at": row["updated_at"],
            "updated_by": row["updated_by"],
        }

    # -- devices -----------------------------------------------------------
    def _create_device(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body) if request.body else {}
        label = model.optional_str(payload, "label", 64) or "passport"
        device_id, token = auth.create_device(connection, identity.family_id, label)
        row = connection.execute(
            "SELECT id, family_id, label, created_at, last_seen_at, revoked_at FROM devices WHERE id=?",
            (device_id,),
        ).fetchone()
        assert row is not None
        return ok({"device": device_json(row), "token": token}, status=201)

    def _list_devices(
        self, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        rows = list_devices(connection, identity.family_id)
        return ok({"devices": [device_json(row) for row in rows]})

    def _revoke_device(
        self,
        request: Request,
        connection: sqlite3.Connection,
        identity: Optional[auth.Identity],
        device_id: str,
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        row = connection.execute(
            "SELECT id, family_id, label, created_at, last_seen_at, revoked_at FROM devices"
            " WHERE id=? AND family_id=?",
            (device_id, identity.family_id),
        ).fetchone()
        if row is None:
            raise ApiError(404, "device_not_found", "no such device")
        if not row["revoked_at"]:
            auth.revoke_device(connection, device_id, model.format_utc(model.now_utc()))
        updated = connection.execute(
            "SELECT id, family_id, label, created_at, last_seen_at, revoked_at FROM devices WHERE id=?",
            (device_id,),
        ).fetchone()
        assert updated is not None
        return ok({"device": device_json(updated)})

    # -- profile -----------------------------------------------------------
    def _get_profile(
        self, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_any(identity)
        return ok({"profile": profile_json(get_pet_profile(connection, identity.family_id), identity.family_id)})

    def _patch_profile(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_caregiver(identity)
        self._origin_allowed(request)
        auth.require_csrf(identity, request.headers)
        payload = parse_json_object(request.body)

        pet_name = model.optional_str(payload, "pet_name", 24)
        breed = model.optional_str(payload, "breed", 24)
        notes = model.optional_str(payload, "notes", 200)
        birthday = model.optional_str(payload, "birthday", 10)
        if birthday is not None and not model.valid_local_date(birthday):
            raise model.FieldError("birthday", "invalid_date", "birthday must look like YYYY-MM-DD")

        raw_weight = payload.get("weight_grams")
        if raw_weight is None or raw_weight == "":
            weight: Optional[int] = None
        elif isinstance(raw_weight, bool) or not isinstance(raw_weight, int):
            raise model.FieldError("weight_grams", "invalid", "weight_grams must be a whole number")
        elif raw_weight <= 0 or raw_weight > 200000:
            raise model.FieldError(
                "weight_grams", "out_of_range", "weight_grams must be between 1 and 200000"
            )
        else:
            weight = raw_weight

        now = model.format_utc(model.now_utc())
        connection.execute(
            "INSERT INTO pet_profile(family_id, pet_name, birthday, breed, weight_grams, notes,"
            " updated_at, updated_by) VALUES(?,?,?,?,?,?,?,?)"
            " ON CONFLICT(family_id) DO UPDATE SET pet_name=excluded.pet_name,"
            " birthday=excluded.birthday, breed=excluded.breed, weight_grams=excluded.weight_grams,"
            " notes=excluded.notes, updated_at=excluded.updated_at, updated_by=excluded.updated_by",
            (
                identity.family_id,
                pet_name,
                birthday,
                breed,
                weight,
                notes,
                now,
                identity.user_id,
            ),
        )
        return ok({"profile": profile_json(get_pet_profile(connection, identity.family_id), identity.family_id)})

    # -- sync --------------------------------------------------------------
    def _changes_payload(
        self,
        connection: sqlite3.Connection,
        family_id: str,
        timezone_name: str,
        cursor: int,
        limit: int,
    ) -> Dict[str, Any]:
        record_rows = list(
            connection.execute(
                "SELECT * FROM records WHERE family_id=? AND seq>? ORDER BY seq ASC LIMIT ?",
                (family_id, cursor, limit + 1),
            )
        )
        reminder_rows = list(
            connection.execute(
                "SELECT * FROM reminders WHERE family_id=? AND seq>? ORDER BY seq ASC LIMIT ?",
                (family_id, cursor, limit + 1),
            )
        )
        merged: List[Tuple[int, sqlite3.Row, str]] = [
            (int(row["seq"]), row, "record") for row in record_rows
        ] + [(int(row["seq"]), row, "reminder") for row in reminder_rows]
        merged.sort(key=lambda item: item[0])
        batch = merged[:limit]
        # Both tables are read up to limit+1, so the merged length alone decides
        # whether anything is left after this batch.
        has_more = len(merged) > limit

        new_cursor = cursor
        if batch:
            new_cursor = max(item[0] for item in batch)

        records = []
        reminders = []
        revoked: List[str] = []
        for seq, row, kind in batch:
            if kind == "record":
                records.append(model.record_to_compact(row))
                if row["status"] == model.STATUS_REVOKED:
                    revoked.append(row["id"])
            else:
                reminders.append(model.reminder_to_compact(row))

        zone = model.load_timezone(timezone_name)
        now = model.now_utc()
        return {
            "server_time": model.format_utc(now),
            "timezone": timezone_name,
            "utc_offset_minutes": model.offset_minutes(zone, now),
            "cursor": new_cursor,
            "has_more": has_more,
            "records": records,
            "reminders": reminders,
            "revoked": revoked,
            "members": sync_members(connection, family_id),
        }

    def _sync_changes(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_device(identity)
        cursor = model.validate_cursor(request.query_one("cursor"))
        limit = model.validate_limit(request.query_one("limit"), default=50, maximum=MAX_SYNC_LIMIT)
        family = self._family(connection, identity.family_id)

        newest = max_seq(connection)
        if cursor > newest:
            raise ApiError(
                409,
                "cursor_invalid",
                "the cursor is newer than the log; resynchronize with /sync/snapshot",
            )

        payload = self._changes_payload(
            connection, identity.family_id, family["timezone"], cursor, limit
        )
        payload["device_id"] = identity.device_id
        return ok(payload)

    def _sync_snapshot(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_device(identity)
        offset = model.validate_offset(request.query_one("offset"))
        limit = model.validate_limit(request.query_one("limit"), default=50, maximum=MAX_SYNC_LIMIT)
        family = self._family(connection, identity.family_id)

        rows = list(
            connection.execute(
                "SELECT * FROM records r WHERE r.family_id=? AND r.version=("
                " SELECT MAX(version) FROM records x WHERE x.family_id=r.family_id AND x.id=r.id)"
                " AND r.status=? ORDER BY r.occurred_at ASC, r.id ASC",
                (identity.family_id, model.STATUS_ACTIVE),
            )
        )
        latest = request.query_one("latest") == "1"
        if latest:
            limit = min(limit, 40)
            offset = max(0, len(rows) - limit)
        page = rows[offset : offset + limit]
        zone = model.load_timezone(family["timezone"])
        now = model.now_utc()
        payload = {
            "server_time": model.format_utc(now),
            "timezone": family["timezone"],
            "utc_offset_minutes": model.offset_minutes(zone, now),
            "offset": offset, "limit": limit, "total": len(rows),
            "has_more": False if latest else offset + limit < len(rows),
            "records": [model.record_to_compact(row) for row in page],
            "reminders": [model.reminder_to_compact(row) for row in
                          current_reminders(connection, identity.family_id)] if latest or offset == 0 else [],
            "members": sync_members(connection, identity.family_id),
            "cursor": max_seq(connection),
        }
        if latest:
            for record in payload["records"]:
                # The cache keeps 48 UTF-8 bytes. Send one extra complete code
                # point so its existing parser marks a longer note as truncated.
                note = record.get("note") or ""
                prefix = note.encode("utf-8")[:52].decode("utf-8", errors="ignore")
                record["note"] = prefix
                # These optional fields do not affect the device cache.
                record.pop("tz", None)
                for key in list(record):
                    if record[key] is None:
                        del record[key]
            # An unusually large reminder/member envelope may reduce the recent
            # window. Full history and full notes remain on the server.
            payload["window_count"] = len(payload["records"])
            while payload["records"] and len(json_bytes(payload)) > 15 * 1024:
                payload["records"].pop(0)
                payload["offset"] += 1
                payload["window_count"] = len(payload["records"])
        return ok(payload)

    def _sync_ack(
        self, request: Request, connection: sqlite3.Connection, identity: Optional[auth.Identity]
    ) -> Response:
        identity = auth.require_device(identity)
        payload = parse_json_object(request.body)
        cursor = model.validate_cursor(payload.get("cursor"))
        applied_at = payload.get("applied_at")
        applied = model.try_parse_timestamp(applied_at) if isinstance(applied_at, str) else None
        if applied_at is not None and applied is None:
            raise model.FieldError("applied_at", "invalid_time", "applied_at must be an RFC 3339 timestamp")
        now = model.format_utc(model.now_utc())
        connection.execute(
            "INSERT INTO device_acks(device_id, cursor, synced_at) VALUES(?,?,?)"
            " ON CONFLICT(device_id) DO UPDATE SET cursor=excluded.cursor, synced_at=excluded.synced_at",
            (identity.device_id, cursor, now),
        )
        auth.touch_device(connection, identity.device_id, now)
        zone = model.load_timezone(self._family(connection, identity.family_id)["timezone"])
        skew_seconds = None
        if applied is not None:
            skew_seconds = int((applied - model.parse_timestamp(now)).total_seconds())
        return ok(
            {
                "ok": True,
                "server_time": now,
                "utc_offset_minutes": model.offset_minutes(zone, model.now_utc()),
                "max_seq": max_seq(connection),
                "clock_skew_seconds": skew_seconds,
            }
        )

    # -- export ------------------------------------------------------------
    def _export(
        self, connection: sqlite3.Connection, identity: Optional[auth.Identity], fmt: str
    ) -> Response:
        identity = auth.require_caregiver(identity)
        if fmt == "csv":
            body = export_module.export_csv(connection, identity.family_id).encode("utf-8")
            return Response(
                200,
                body,
                content_type="text/csv; charset=utf-8",
                headers=[("Content-Disposition", 'attachment; filename="lanlan-records.csv"')],
            )
        payload = export_module.export_json(connection, identity.family_id)
        return Response(
            200,
            export_module.dumps_json(payload).encode("utf-8"),
            headers=[("Content-Disposition", 'attachment; filename="lanlan-records.json"')],
        )
