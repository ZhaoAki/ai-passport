"""Authentication, tokens, CSRF and login throttling.

Password and token material is only ever handled as digests outside this
module, and nothing in it writes secrets to a log. The module works on plain
``sqlite3.Connection`` objects handed in by the caller so that each request
keeps using its own connection.
"""

from __future__ import annotations

import hashlib
import hmac
import secrets
import sqlite3
import uuid
from datetime import timedelta
from typing import Any, Dict, Optional, Tuple

from . import model

SESSION_COOKIE_NAME = "lanlan_session"
PBKDF2_PREFIX = "pbkdf2_sha256"
PBKDF2_SALT_BYTES = 16
TOKEN_BYTES = 32

# Login throttling: exponential backoff after this many failures, capped.
THROTTLE_FREE_FAILURES = 3
THROTTLE_BASE_SECONDS = 5
THROTTLE_MAX_SECONDS = 300
THROTTLE_WINDOW_SECONDS = 900


class AuthError(Exception):
    """Authentication or authorization failure with an HTTP status code."""

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


class Identity:
    """The authenticated party for one request."""

    def __init__(
        self,
        kind: str,
        family_id: str,
        user_id: Optional[str] = None,
        csrf: Optional[str] = None,
        session_hash: Optional[str] = None,
        device_id: Optional[str] = None,
        username: Optional[str] = None,
        display_name: Optional[str] = None,
    ) -> None:
        self.kind = kind
        self.family_id = family_id
        self.user_id = user_id
        self.csrf = csrf
        self.session_hash = session_hash
        self.device_id = device_id
        self.username = username
        self.display_name = display_name

    @property
    def is_caregiver(self) -> bool:
        return self.kind == "user"

    @property
    def is_device(self) -> bool:
        return self.kind == "device"


def new_id() -> str:
    return str(uuid.uuid4())


# --------------------------------------------------------------------------
# Password hashing
# --------------------------------------------------------------------------

def hash_password(password: str, iterations: int) -> str:
    salt = secrets.token_bytes(PBKDF2_SALT_BYTES)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
    return "%s$%d$%s$%s" % (PBKDF2_PREFIX, iterations, salt.hex(), digest.hex())


def verify_password(stored: str, password: str) -> bool:
    try:
        prefix, iterations_text, salt_hex, digest_hex = stored.split("$")
        if prefix != PBKDF2_PREFIX:
            return False
        iterations = int(iterations_text)
        salt = bytes.fromhex(salt_hex)
        expected = bytes.fromhex(digest_hex)
    except (ValueError, AttributeError):
        return False
    computed = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
    return hmac.compare_digest(computed, expected)


def validate_password_strength(password: str) -> None:
    if not isinstance(password, str) or len(password) < 8:
        raise model.FieldError("password", "too_short", "password must be at least 8 characters")
    if len(password) > 256:
        raise model.FieldError("password", "too_long", "password is too long")


# --------------------------------------------------------------------------
# Tokens and sessions
# --------------------------------------------------------------------------

def new_token() -> str:
    return secrets.token_urlsafe(TOKEN_BYTES)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def create_session(
    connection: sqlite3.Connection,
    user_id: str,
    session_days: int,
    now: Optional[str] = None,
) -> Tuple[str, str]:
    """Insert a session row and return ``(plaintext token, csrf token)``."""
    created_at = now or model.format_utc(model.now_utc())
    expires_at = model.format_utc(
        model.parse_timestamp(created_at) + timedelta(days=session_days)
    )
    token = new_token()
    csrf = new_token()
    connection.execute(
        "INSERT INTO sessions(token_hash, user_id, csrf, created_at, expires_at, last_seen_at)"
        " VALUES(?,?,?,?,?,?)",
        (token_hash(token), user_id, csrf, created_at, expires_at, created_at),
    )
    return token, csrf


def delete_session(connection: sqlite3.Connection, session_hash: str) -> None:
    connection.execute("DELETE FROM sessions WHERE token_hash=?", (session_hash,))


def touch_session(connection: sqlite3.Connection, session_hash: str, expires_at: str, now: str) -> None:
    connection.execute(
        "UPDATE sessions SET last_seen_at=?, expires_at=? WHERE token_hash=?",
        (now, expires_at, session_hash),
    )


def find_session(connection: sqlite3.Connection, token: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT token_hash, user_id, csrf, created_at, expires_at, last_seen_at"
        " FROM sessions WHERE token_hash=?",
        (token_hash(token),),
    ).fetchone()


def session_is_expired(row: sqlite3.Row, now: Optional[str] = None) -> bool:
    expires = model.try_parse_timestamp(row["expires_at"])
    if expires is None:
        return True
    return (model.parse_timestamp(now) if now else model.now_utc()) >= expires


def create_device(
    connection: sqlite3.Connection,
    family_id: str,
    label: str,
    now: Optional[str] = None,
) -> Tuple[str, str]:
    """Insert a device row and return ``(device id, plaintext token)``."""
    device_id = new_id()
    token = new_token()
    created_at = now or model.format_utc(model.now_utc())
    connection.execute(
        "INSERT INTO devices(id, family_id, label, token_hash, created_at) VALUES(?,?,?,?,?)",
        (device_id, family_id, label, token_hash(token), created_at),
    )
    return device_id, token


def find_device(connection: sqlite3.Connection, token: str) -> Optional[sqlite3.Row]:
    return connection.execute(
        "SELECT id, family_id, label, token_hash, created_at, last_seen_at, revoked_at"
        " FROM devices WHERE token_hash=?",
        (token_hash(token),),
    ).fetchone()


def device_is_usable(row: sqlite3.Row) -> bool:
    return not row["revoked_at"]


def touch_device(connection: sqlite3.Connection, device_id: str, now: str) -> None:
    connection.execute(
        "UPDATE devices SET last_seen_at=? WHERE id=?", (now, device_id)
    )


def revoke_device(connection: sqlite3.Connection, device_id: str, now: str) -> None:
    connection.execute(
        "UPDATE devices SET revoked_at=? WHERE id=?", (now, device_id)
    )


# --------------------------------------------------------------------------
# Login throttling
# --------------------------------------------------------------------------

def throttle_key(source_address: str, username: str) -> str:
    raw = "%s|%s" % (source_address or "-", (username or "").strip().lower())
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def throttled_for(connection: sqlite3.Connection, key: str, now: Optional[str] = None) -> Optional[int]:
    """Return remaining lockout seconds, or ``None`` when the caller may try."""
    row = connection.execute(
        "SELECT failure_count, last_failure_at, locked_until FROM login_attempts WHERE throttle_key=?",
        (key,),
    ).fetchone()
    if row is None or not row["locked_until"]:
        return None
    reference = model.parse_timestamp(now) if now else model.now_utc()
    locked_until = model.try_parse_timestamp(row["locked_until"])
    if locked_until is None or reference >= locked_until:
        return None
    return max(1, int((locked_until - reference).total_seconds()))


def record_login_failure(
    connection: sqlite3.Connection, key: str, now: Optional[str] = None
) -> Optional[int]:
    """Count one failure and return the new lockout in seconds, if any."""
    reference = model.parse_timestamp(now) if now else model.now_utc()
    stamp = model.format_utc(reference)
    row = connection.execute(
        "SELECT failure_count, first_failure_at, last_failure_at, locked_until"
        " FROM login_attempts WHERE throttle_key=?",
        (key,),
    ).fetchone()

    if row is None:
        count = 1
        first_failure_at = stamp
    else:
        first = model.try_parse_timestamp(row["first_failure_at"])
        stale = first is None or (reference - first).total_seconds() > THROTTLE_WINDOW_SECONDS
        count = 1 if stale else int(row["failure_count"]) + 1
        first_failure_at = stamp if stale else row["first_failure_at"]

    locked_until = None
    if count > THROTTLE_FREE_FAILURES:
        backoff = THROTTLE_BASE_SECONDS * (2 ** (count - THROTTLE_FREE_FAILURES - 1))
        backoff = min(backoff, THROTTLE_MAX_SECONDS)
        locked_until = model.format_utc(reference + timedelta(seconds=backoff))

    connection.execute(
        "INSERT INTO login_attempts(throttle_key, failure_count, first_failure_at,"
        " last_failure_at, locked_until) VALUES(?,?,?,?,?)"
        " ON CONFLICT(throttle_key) DO UPDATE SET failure_count=excluded.failure_count,"
        " first_failure_at=excluded.first_failure_at, last_failure_at=excluded.last_failure_at,"
        " locked_until=excluded.locked_until",
        (key, count, first_failure_at, stamp, locked_until),
    )
    if locked_until is None:
        return None
    return max(1, int((model.parse_timestamp(locked_until) - reference).total_seconds()))


def clear_login_failures(connection: sqlite3.Connection, key: str) -> None:
    connection.execute("DELETE FROM login_attempts WHERE throttle_key=?", (key,))


# --------------------------------------------------------------------------
# Request authentication
# --------------------------------------------------------------------------

def authenticate(connection: sqlite3.Connection, headers: Any, session_days: int) -> Optional[Identity]:
    """Resolve the caller from the session cookie or the bearer token.

    Returns ``None`` when no usable credential is present; callers turn that
    into ``401`` for protected endpoints.
    """
    bearer = bearer_token(headers)
    if bearer:
        row = find_device(connection, bearer)
        if row is None or not device_is_usable(row):
            raise AuthError(401, "invalid_token", "device token is not valid")
        now = model.format_utc(model.now_utc())
        touch_device(connection, row["id"], now)
        return Identity(
            kind="device",
            family_id=row["family_id"],
            device_id=row["id"],
        )

    cookie = session_cookie(headers)
    if not cookie:
        return None
    row = find_session(connection, cookie)
    if row is None:
        return None
    if session_is_expired(row):
        delete_session(connection, row["token_hash"])
        return None
    user = connection.execute(
        "SELECT id, family_id, username, display_name, disabled_at FROM users WHERE id=?",
        (row["user_id"],),
    ).fetchone()
    if user is None or user["disabled_at"]:
        delete_session(connection, row["token_hash"])
        return None
    now_dt = model.now_utc()
    # Renew the 30-day sliding window, but do not turn every read into a write:
    # one update per hour per session is enough to keep it alive.
    last_seen = model.try_parse_timestamp(row["last_seen_at"])
    if last_seen is None or (now_dt - last_seen).total_seconds() > 3600:
        touch_session(
            connection,
            row["token_hash"],
            model.format_utc(now_dt + timedelta(days=session_days)),
            model.format_utc(now_dt),
        )
    return Identity(
        kind="user",
        family_id=user["family_id"],
        user_id=user["id"],
        csrf=row["csrf"],
        session_hash=row["token_hash"],
        username=user["username"],
        display_name=user["display_name"],
    )


def session_cookie(headers: Any) -> Optional[str]:
    raw = headers.get("Cookie") if headers is not None else None
    if not raw:
        return None
    for part in raw.split(";"):
        name, _, value = part.strip().partition("=")
        if name == SESSION_COOKIE_NAME and value:
            return value
    return None


def bearer_token(headers: Any) -> Optional[str]:
    raw = headers.get("Authorization") if headers is not None else None
    if not raw:
        return None
    scheme, _, token = raw.partition(" ")
    if scheme.lower() != "bearer" or not token.strip():
        return None
    return token.strip()


def require_caregiver(identity: Optional[Identity]) -> Identity:
    if identity is None:
        raise AuthError(401, "unauthenticated", "authentication is required")
    if not identity.is_caregiver:
        raise AuthError(403, "caregiver_required", "a caregiver session is required")
    return identity


def require_device(identity: Optional[Identity]) -> Identity:
    if identity is None:
        raise AuthError(401, "unauthenticated", "authentication is required")
    if not identity.is_device:
        raise AuthError(403, "device_required", "a device credential is required")
    return identity


def require_any(identity: Optional[Identity]) -> Identity:
    if identity is None:
        raise AuthError(401, "unauthenticated", "authentication is required")
    return identity


def require_csrf(identity: Identity, headers: Any) -> None:
    """State-changing caregiver requests need the header and a same-origin Origin."""
    if not identity.is_caregiver:
        raise AuthError(403, "caregiver_required", "a caregiver session is required")
    origin = headers.get("Origin")
    if not origin:
        raise AuthError(403, "origin_required", "state-changing requests need an Origin header")
    supplied = headers.get("X-Lanlan-CSRF")
    if not supplied or not identity.csrf or not hmac.compare_digest(supplied, identity.csrf):
        raise AuthError(403, "csrf_invalid", "the CSRF token is missing or invalid")
