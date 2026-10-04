"""Domain model: validation, time handling and row shaping.

Validation lives here so every write path (web page, retry, future import)
shares one implementation. The module has no knowledge of HTTP and no
third-party dependencies.
"""

from __future__ import annotations

import json
import re
from datetime import datetime, timedelta, timezone, tzinfo
from typing import Any, Dict, Mapping, Optional, Sequence, Tuple
from urllib.parse import quote

try:  # pragma: no cover - exercised on interpreters without tzdata
    from zoneinfo import ZoneInfo, ZoneInfoNotFoundError
except ImportError:  # pragma: no cover - CPython 3.9+ always has zoneinfo
    ZoneInfo = None  # type: ignore[assignment]

    class ZoneInfoNotFoundError(Exception):
        pass


UTC = timezone.utc
RFC3339_UTC_FORMAT = "%Y-%m-%dT%H:%M:%SZ"

DEFAULT_TIMEZONE = "Asia/Shanghai"

CATEGORIES: Tuple[str, ...] = ("meal", "water", "care", "cleaning", "walk", "other")

MEAL_UNITS: Tuple[str, ...] = ("g", "ml", "scoop", "cup", "piece", "bag")
WATER_UNITS: Tuple[str, ...] = ("ml", "bowl")
CATEGORY_UNITS: Dict[str, Tuple[str, ...]] = {
    "meal": MEAL_UNITS,
    "water": WATER_UNITS,
}

CARE_SUBITEMS: Tuple[str, ...] = ("bath", "grooming", "teeth", "comb", "other")
CLEANING_SUBITEMS: Tuple[str, ...] = ("ear", "paw", "pad", "litter", "other")
CATEGORY_SUBITEMS: Dict[str, Tuple[str, ...]] = {
    "care": CARE_SUBITEMS,
    "cleaning": CLEANING_SUBITEMS,
}

TIME_CONFIDENCE_VALUES: Tuple[str, ...] = ("trusted", "estimated")
STATUS_ACTIVE = "active"
STATUS_REVOKED = "revoked"
SOURCE_MANUAL = "manual"
SCHEDULE_DAILY = "daily"

MAX_AMOUNT = 9999.0
MAX_NOTE_LENGTH = 200
MAX_CUSTOM_NAME_LENGTH = 12
MIN_DURATION_MINUTES = 1
MAX_DURATION_MINUTES = 1440
MAX_PAST_DAYS = 400
MAX_FUTURE_DAYS = 1

# The seven reminders the initialization command creates, all disabled.
DEFAULT_REMINDERS: Tuple[Tuple[str, Optional[str]], ...] = (
    ("meal", None),
    ("water", None),
    ("care", "bath"),
    ("care", "grooming"),
    ("care", "teeth"),
    ("walk", None),
    ("care", "comb"),
)

_TIME_RE = re.compile(r"^([01][0-9]|2[0-3]):([0-5][0-9])$")


class FieldError(Exception):
    """A machine-readable validation failure that becomes HTTP 422."""

    def __init__(self, field: str, code: str, message: str) -> None:
        super().__init__(message)
        self.field = field
        self.code = code
        self.message = message

    def as_error(self) -> Dict[str, Any]:
        return {"error": {"code": self.code, "message": self.message, "field": self.field}}


# --------------------------------------------------------------------------
# Time helpers
# --------------------------------------------------------------------------

# Fixed-offset fallback for the small set of zones a household realistically
# configures, used only when the interpreter has no IANA database.
_FALLBACK_ZONES: Dict[str, int] = {
    "UTC": 0,
    "Etc/UTC": 0,
    "Asia/Shanghai": 480,
    "Asia/Chongqing": 480,
    "Asia/Harbin": 480,
    "Asia/Urumqi": 360,
    "Asia/Hong_Kong": 480,
    "Asia/Taipei": 480,
    "Asia/Tokyo": 540,
    "Asia/Seoul": 540,
    "Asia/Singapore": 480,
    "Europe/London": 0,
    "Europe/Berlin": 60,
    "Europe/Paris": 60,
    "America/New_York": -300,
    "America/Los_Angeles": -480,
    "Australia/Sydney": 600,
}

_FIXED_ZONE_RE = re.compile(r"^(?:UTC|GMT)?([+-])(\d{1,2})(?::?(\d{2}))?$")


class FixedOffsetZone(tzinfo):
    """A fixed-offset ``tzinfo`` used when no IANA database is available."""

    def __init__(self, minutes: int, key: str = "UTC") -> None:
        self._offset = timedelta(minutes=minutes)
        self.key = key

    def utcoffset(self, dt: Optional[datetime]) -> timedelta:
        return self._offset

    def dst(self, dt: Optional[datetime]) -> timedelta:
        return timedelta(0)

    def tzname(self, dt: Optional[datetime]) -> str:
        return self.key

    def fromutc(self, dt: datetime) -> datetime:
        return (dt + self._offset).replace(tzinfo=self)

    def __repr__(self) -> str:
        return "FixedOffsetZone(%r)" % self.key


def _fixed_zone(minutes: int, key: str) -> FixedOffsetZone:
    return FixedOffsetZone(minutes, key)


# Cache of already-resolved zones, keyed by the configured IANA name.
_ZONE_OVERRIDES: Dict[str, Any] = {}


def load_timezone(name: str) -> "Any":
    """Return a tzinfo for an IANA name, degrading gracefully.

    When the interpreter cannot load IANA data (no tzdata on a slim image)
    the function falls back to a fixed offset taken from a small table, to a
    parsed ``UTC+08:00`` style name, or finally to UTC. The returned object
    always understands ``utcoffset``/``dst`` through the datetime module.
    """
    key = (name or "").strip() or DEFAULT_TIMEZONE
    override = _ZONE_OVERRIDES.get(key)
    if override is not None:
        return override
    if ZoneInfo is not None:
        try:
            zone = ZoneInfo(key)
        except (ZoneInfoNotFoundError, ValueError, KeyError, OSError):
            zone = None
        if zone is not None:
            _ZONE_OVERRIDES[key] = zone
            return zone
    minutes = _FALLBACK_ZONES.get(key)
    if minutes is None:
        minutes = _parse_offset_name(key)
    zone = _fixed_zone(minutes, key)
    _ZONE_OVERRIDES[key] = zone
    return zone


def _parse_offset_name(key: str) -> int:
    match = _FIXED_ZONE_RE.match(key.replace(" ", ""))
    if not match:
        return 0
    sign = -1 if match.group(1) == "-" else 1
    hours = int(match.group(2))
    minutes = int(match.group(3) or 0)
    return sign * (hours * 60 + minutes)


def now_utc() -> datetime:
    return datetime.now(UTC).replace(microsecond=0)


def format_utc(moment: datetime) -> str:
    """RFC 3339 UTC with second precision, e.g. ``2026-10-04T12:00:00Z``."""
    return moment.astimezone(UTC).replace(microsecond=0).strftime(RFC3339_UTC_FORMAT)


def parse_timestamp(value: str) -> datetime:
    """Parse an RFC 3339 timestamp (``Z`` or numeric offset) into UTC."""
    if not isinstance(value, str) or not value.strip():
        raise ValueError("empty timestamp")
    text = value.strip()
    if text.endswith("Z") or text.endswith("z"):
        text = text[:-1] + "+00:00"
    try:
        moment = datetime.fromisoformat(text)
    except ValueError:
        raise ValueError("invalid timestamp: %s" % value)
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=UTC)
    return moment.astimezone(UTC).replace(microsecond=0)


def try_parse_timestamp(value: Any) -> Optional[datetime]:
    try:
        return parse_timestamp(value)
    except (ValueError, TypeError):
        return None


def offset_minutes(zone: "Any", moment: datetime) -> int:
    delta = zone.utcoffset(moment.astimezone(zone))
    if delta is None:
        return 0
    return int(delta.total_seconds() // 60)


def local_date_bounds(day: str, zone: "Any") -> Tuple[datetime, datetime]:
    """Return the UTC instants delimiting one local calendar day."""
    year, month, day_of_month = (int(part) for part in day.split("-"))
    start_local = datetime(year, month, day_of_month, tzinfo=zone)
    end_local = start_local + timedelta(days=1)
    return start_local.astimezone(UTC), end_local.astimezone(UTC)


def local_date_of(moment: datetime, zone: "Any") -> str:
    return moment.astimezone(zone).strftime("%Y-%m-%d")


def today_in_zone(zone: "Any", moment: Optional[datetime] = None) -> str:
    return local_date_of(moment or now_utc(), zone)


def valid_local_date(day: str) -> bool:
    try:
        datetime.strptime(day, "%Y-%m-%d")
    except (ValueError, TypeError):
        return False
    return True


def seconds_until_lock(locked_until: str, reference: Optional[datetime] = None) -> int:
    """Retry-after helper for the login throttle."""
    target = try_parse_timestamp(locked_until)
    if target is None:
        return 0
    delta = target - (reference or now_utc())
    return max(0, int(delta.total_seconds()))


# --------------------------------------------------------------------------
# Row shaping
# --------------------------------------------------------------------------

def row_to_record(row: Mapping[str, Any]) -> Dict[str, Any]:
    """Current-revision shape used by the API and the web page."""
    return {
        "id": row["id"],
        "version": row["version"],
        "seq": row["seq"],
        "category": row["category"],
        "subitem": row["subitem"],
        "custom_name": row["custom_name"],
        "occurred_at": row["occurred_at"],
        "occurred_tz": row["occurred_tz"],
        "time_confidence": row["time_confidence"],
        "created_at": row["created_at"],
        "created_by": row["created_by"],
        "performed_by": row["performed_by"],
        "amount_value": row["amount_value"],
        "amount_unit": row["amount_unit"],
        "duration_minutes": row["duration_minutes"],
        "note": row["note"],
        "status": row["status"],
        "source": row["source"],
        "client_request_id": row["client_request_id"],
        "supersedes_seq": row["supersedes_seq"],
        "revoke_reason": row["revoke_reason"],
    }


def record_to_compact(row: Mapping[str, Any]) -> Dict[str, Any]:
    """Cache shape for the passport; ``amt`` is omitted when unknown."""
    compact: Dict[str, Any] = {
        "id": row["id"],
        "v": row["version"],
        "cat": row["category"],
        "sub": row["subitem"],
        "name": row["custom_name"],
        "at": row["occurred_at"],
        "tz": row["occurred_tz"],
        "tc": row["time_confidence"],
        "by": row["created_by"],
        "perf": row["performed_by"],
        "unit": row["amount_unit"],
        "dur": row["duration_minutes"],
        "note": row["note"],
        "st": row["status"],
        "seq": row["seq"],
    }
    if row["amount_value"] is not None:
        compact["amt"] = row["amount_value"]
    return compact


def reminder_to_json(row: Mapping[str, Any]) -> Dict[str, Any]:
    return {
        "id": row["id"],
        "version": row["version"],
        "seq": row["seq"],
        "category": row["category"],
        "subitem": row["subitem"],
        "custom_name": row["custom_name"],
        "enabled": bool(row["enabled"]),
        "schedule_type": row["schedule_type"],
        "time_local": row["time_local"],
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
        "updated_by": row["updated_by"],
    }


def reminder_to_compact(row: Mapping[str, Any]) -> Dict[str, Any]:
    return {
        "id": row["id"],
        "v": row["version"],
        "cat": row["category"],
        "sub": row["subitem"],
        "name": row["custom_name"],
        "en": 1 if row["enabled"] else 0,
        "sched": row["schedule_type"],
        "t": row["time_local"],
        "upd": row["updated_at"],
        "seq": row["seq"],
    }


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------

def require_str(payload: Mapping[str, Any], field: str, max_length: Optional[int] = None) -> str:
    value = payload.get(field)
    if not isinstance(value, str) or not value.strip():
        raise FieldError(field, "required", "%s is required" % field)
    text = value.strip()
    if max_length is not None and len(text) > max_length:
        raise FieldError(field, "too_long", "%s must be at most %d characters" % (field, max_length))
    return text


def optional_str(
    payload: Mapping[str, Any], field: str, max_length: Optional[int] = None
) -> Optional[str]:
    value = payload.get(field)
    if value is None:
        return None
    if not isinstance(value, str):
        raise FieldError(field, "invalid", "%s must be a string" % field)
    text = value.strip()
    if not text:
        return None
    if max_length is not None and len(text) > max_length:
        raise FieldError(field, "too_long", "%s must be at most %d characters" % (field, max_length))
    return text


def validate_category(payload: Mapping[str, Any]) -> Dict[str, Optional[str]]:
    """Validate the category, sub-item and custom-name combination."""
    category = require_str(payload, "category")
    if category not in CATEGORIES:
        raise FieldError("category", "invalid_category", "unknown category: %s" % category)

    raw_subitem = payload.get("subitem")
    subitem: Optional[str] = None
    if raw_subitem is not None and raw_subitem != "":
        if not isinstance(raw_subitem, str):
            raise FieldError("subitem", "invalid", "subitem must be a string")
        subitem = raw_subitem.strip() or None

    custom_name = optional_str(payload, "custom_name", MAX_CUSTOM_NAME_LENGTH)

    allowed = CATEGORY_SUBITEMS.get(category)
    if allowed is None:
        if subitem is not None:
            raise FieldError("subitem", "not_allowed", "%s does not take a sub-item" % category)
    else:
        if subitem is None:
            raise FieldError("subitem", "required", "subitem is required for %s" % category)
        if subitem not in allowed:
            raise FieldError("subitem", "invalid_subitem", "unknown subitem: %s" % subitem)
        if category == "care" and subitem == "other" and custom_name is None:
            raise FieldError("custom_name", "required", "custom_name is required for care/other")

    if category == "other" and custom_name is None:
        raise FieldError("custom_name", "required", "custom_name is required for other")
    if category not in ("cleaning", "other") and not (category == "care" and subitem == "other"):
        if custom_name is not None:
            raise FieldError("custom_name", "not_allowed", "%s does not take a custom name" % category)

    return {"category": category, "subitem": subitem, "custom_name": custom_name}


def validate_amount(payload: Mapping[str, Any], category: str) -> Dict[str, Any]:
    """Validate the optional quantity; ``None`` always means unknown."""
    raw_value = payload.get("amount_value")
    raw_unit = payload.get("amount_unit")
    if isinstance(raw_unit, str) and not raw_unit.strip():
        raw_unit = None

    if raw_value is None or raw_value == "":
        if raw_unit is not None:
            raise FieldError("amount_unit", "unit_without_amount", "amount_unit needs amount_value")
        return {"amount_value": None, "amount_unit": None}

    if isinstance(raw_value, bool) or not isinstance(raw_value, (int, float)):
        raise FieldError("amount_value", "invalid", "amount_value must be a number")
    value = float(raw_value)
    if value != value or value in (float("inf"), float("-inf")):
        raise FieldError("amount_value", "invalid", "amount_value must be a finite number")
    if value <= 0:
        raise FieldError("amount_value", "out_of_range", "amount_value must be greater than 0")
    if value > MAX_AMOUNT:
        raise FieldError("amount_value", "out_of_range", "amount_value must be at most %g" % MAX_AMOUNT)

    units = CATEGORY_UNITS.get(category)
    if units is None:
        raise FieldError("amount_unit", "not_allowed", "%s does not take an amount" % category)
    if not isinstance(raw_unit, str) or raw_unit not in units:
        raise FieldError(
            "amount_unit", "invalid_unit", "amount_unit must be one of %s" % ", ".join(units)
        )
    return {"amount_value": value, "amount_unit": raw_unit}


def validate_duration(payload: Mapping[str, Any], category: str) -> Optional[int]:
    raw_duration = payload.get("duration_minutes")
    if raw_duration is None or raw_duration == "":
        return None
    if isinstance(raw_duration, bool) or not isinstance(raw_duration, int):
        raise FieldError("duration_minutes", "invalid", "duration_minutes must be a whole number")
    if category != "walk":
        raise FieldError("duration_minutes", "not_allowed", "%s does not take a duration" % category)
    if raw_duration < MIN_DURATION_MINUTES or raw_duration > MAX_DURATION_MINUTES:
        raise FieldError(
            "duration_minutes",
            "out_of_range",
            "duration_minutes must be between %d and %d"
            % (MIN_DURATION_MINUTES, MAX_DURATION_MINUTES),
        )
    return raw_duration


def validate_note(payload: Mapping[str, Any]) -> Optional[str]:
    return optional_str(payload, "note", MAX_NOTE_LENGTH)


def validate_occurred_at(payload: Mapping[str, Any], reference: Optional[datetime] = None) -> datetime:
    reference = reference or now_utc()
    raw = payload.get("occurred_at")
    if raw is None or raw == "":
        return reference
    moment = try_parse_timestamp(raw)
    if moment is None:
        raise FieldError("occurred_at", "invalid_time", "occurred_at must be an RFC 3339 timestamp")
    earliest = reference - timedelta(days=MAX_PAST_DAYS)
    latest = reference + timedelta(days=MAX_FUTURE_DAYS)
    if moment < earliest or moment > latest:
        raise FieldError(
            "occurred_at",
            "out_of_range",
            "occurred_at must be between %d days in the past and %d day in the future"
            % (MAX_PAST_DAYS, MAX_FUTURE_DAYS),
        )
    return moment


def validate_time_confidence(payload: Mapping[str, Any]) -> str:
    value = payload.get("time_confidence")
    if value is None or value == "":
        return "trusted"
    if not isinstance(value, str) or value not in TIME_CONFIDENCE_VALUES:
        raise FieldError(
            "time_confidence",
            "invalid",
            "time_confidence must be one of %s" % ", ".join(TIME_CONFIDENCE_VALUES),
        )
    return value


def validate_occurred_tz(payload: Mapping[str, Any], family_timezone: str) -> str:
    value = payload.get("occurred_tz")
    if value is None or value == "":
        return family_timezone
    if not isinstance(value, str):
        raise FieldError("occurred_tz", "invalid", "occurred_tz must be a string")
    text = value.strip()
    if len(text) > 64:
        raise FieldError("occurred_tz", "invalid", "occurred_tz is too long")
    zone = load_timezone(text)
    if zone is None:
        raise FieldError("occurred_tz", "invalid_zone", "unknown timezone: %s" % text)
    return text


def validate_schedule(payload: Mapping[str, Any]) -> str:
    value = payload.get("schedule_type")
    if value is None or value == "":
        return SCHEDULE_DAILY
    if value != SCHEDULE_DAILY:
        raise FieldError("schedule_type", "invalid", "only the daily schedule is supported")
    return value


def validate_time_local(value: Any) -> Optional[str]:
    if value is None or value == "":
        return None
    if not isinstance(value, str):
        raise FieldError("time_local", "invalid", "time_local must be a string")
    text = value.strip()
    if not _TIME_RE.match(text):
        raise FieldError("time_local", "invalid_time", "time_local must look like HH:MM")
    return text


def build_record_payload(
    payload: Mapping[str, Any],
    family_timezone: str,
    reference: Optional[datetime] = None,
) -> Dict[str, Any]:
    """Validate a create/edit body and return the storable column values."""
    if not isinstance(payload, Mapping):
        raise FieldError("body", "invalid_body", "request body must be a JSON object")
    category_fields = validate_category(payload)
    category = category_fields["category"]
    amount = validate_amount(payload, category)
    duration = validate_duration(payload, category)
    note = validate_note(payload)
    occurred_at = validate_occurred_at(payload, reference)
    occurred_tz = validate_occurred_tz(payload, family_timezone)
    confidence = validate_time_confidence(payload)
    return {
        "category": category,
        "subitem": category_fields["subitem"],
        "custom_name": category_fields["custom_name"],
        "occurred_at": format_utc(occurred_at),
        "occurred_tz": occurred_tz,
        "time_confidence": confidence,
        "amount_value": amount["amount_value"],
        "amount_unit": amount["amount_unit"],
        "duration_minutes": duration,
        "note": note,
    }


def expected_version(payload: Mapping[str, Any]) -> int:
    raw = payload.get("expected_version")
    if isinstance(raw, bool) or not isinstance(raw, int):
        raise FieldError("expected_version", "required", "expected_version is required")
    if raw < 1:
        raise FieldError("expected_version", "out_of_range", "expected_version must be at least 1")
    return raw


def encode_page_cursor(occurred_at: str, record_id: str) -> str:
    raw = json.dumps({"at": occurred_at, "id": record_id}, separators=(",", ":"))
    return quote(raw, safe="")


def decode_page_cursor(cursor: str) -> Optional[Tuple[str, str]]:
    from urllib.parse import unquote

    try:
        decoded = json.loads(unquote(cursor))
    except (ValueError, TypeError):
        return None
    if not isinstance(decoded, dict):
        return None
    occurred_at = decoded.get("at")
    record_id = decoded.get("id")
    if not isinstance(occurred_at, str) or not isinstance(record_id, str):
        return None
    return occurred_at, record_id


def validate_limit(raw: Any, default: int = 50, maximum: int = 100) -> int:
    if raw is None or raw == "":
        return default
    try:
        value = int(raw)
    except (TypeError, ValueError):
        raise FieldError("limit", "invalid", "limit must be an integer")
    if value < 1 or value > maximum:
        raise FieldError("limit", "out_of_range", "limit must be between 1 and %d" % maximum)
    return value


def validate_offset(raw: Any) -> int:
    if raw is None or raw == "":
        return 0
    try:
        value = int(raw)
    except (TypeError, ValueError):
        raise FieldError("offset", "invalid", "offset must be an integer")
    if value < 0:
        raise FieldError("offset", "out_of_range", "offset must not be negative")
    return value


def validate_cursor(raw: Any) -> int:
    if raw is None or raw == "":
        return 0
    try:
        value = int(raw)
    except (TypeError, ValueError):
        raise FieldError("cursor", "invalid", "cursor must be an integer")
    if value < 0:
        raise FieldError("cursor", "out_of_range", "cursor must not be negative")
    return value


def reminder_key(category: str, subitem: Optional[str]) -> Tuple[str, Optional[str]]:
    return (category, subitem)


def reminder_specs() -> Sequence[Tuple[str, Optional[str]]]:
    return DEFAULT_REMINDERS
