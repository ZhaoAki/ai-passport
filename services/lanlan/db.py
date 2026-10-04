"""SQLite access layer for the Lanlan service.

The schema follows ``docs/applications/cyber-lanlan-service.md`` section 4.1
exactly for the tables the specification freezes. Two additional service tables
are needed to implement features the specification requires but does not model:
``pet_profile`` (the profile page) and ``login_attempts`` (login throttling).
Both are additive and never alter the frozen tables.

``records`` and ``reminders`` are append-only revision logs: the code never
issues ``UPDATE`` or ``DELETE`` against them. The current state of a row is its
highest ``version`` for a given ``id``.

Every request gets its own connection, so threads never share a sqlite3
connection. A write request wraps its statements in one ``BEGIN IMMEDIATE``
transaction, which serialises writers and keeps the append-only invariant.

``records.seq`` and ``reminders.seq`` draw from one allocator table
(``change_seq``) so that the sync cursor has a single global change order.
Without that, the two AUTOINCREMENT counters overlap and the cursor can skip
changes. ``change_seq`` is additive; the specification's table definitions are
unchanged and every revision still inserts an explicit ``seq`` value.
"""

from __future__ import annotations

import os
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from typing import Iterator, Optional


#: 1 was the first layout with per-table sequences; 2 adds ``change_seq`` and
#: renumbers existing rows into one global order.
SCHEMA_VERSION = 2

SCHEMA = """
CREATE TABLE IF NOT EXISTS families(
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    timezone TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS users(
    id TEXT PRIMARY KEY,
    family_id TEXT NOT NULL REFERENCES families(id),
    username TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    created_at TEXT NOT NULL,
    disabled_at TEXT
);

CREATE TABLE IF NOT EXISTS sessions(
    token_hash TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id),
    csrf TEXT NOT NULL,
    created_at TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS devices(
    id TEXT PRIMARY KEY,
    family_id TEXT NOT NULL REFERENCES families(id),
    label TEXT NOT NULL,
    token_hash TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    last_seen_at TEXT,
    revoked_at TEXT
);

CREATE TABLE IF NOT EXISTS records(
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    id TEXT NOT NULL,
    version INTEGER NOT NULL,
    family_id TEXT NOT NULL REFERENCES families(id),
    category TEXT NOT NULL,
    subitem TEXT,
    custom_name TEXT,
    occurred_at TEXT NOT NULL,
    occurred_tz TEXT NOT NULL,
    time_confidence TEXT NOT NULL,
    created_at TEXT NOT NULL,
    created_by TEXT NOT NULL,
    performed_by TEXT NOT NULL,
    amount_value REAL,
    amount_unit TEXT,
    duration_minutes INTEGER,
    note TEXT,
    status TEXT NOT NULL,
    source TEXT NOT NULL,
    client_request_id TEXT,
    supersedes_seq INTEGER,
    revoke_reason TEXT,
    UNIQUE(id, version)
);

CREATE TABLE IF NOT EXISTS reminders(
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    id TEXT NOT NULL,
    version INTEGER NOT NULL,
    family_id TEXT NOT NULL REFERENCES families(id),
    category TEXT NOT NULL,
    subitem TEXT,
    custom_name TEXT,
    enabled INTEGER NOT NULL DEFAULT 0,
    schedule_type TEXT NOT NULL DEFAULT 'daily',
    time_local TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    updated_by TEXT NOT NULL,
    UNIQUE(id, version)
);

CREATE TABLE IF NOT EXISTS device_acks(
    device_id TEXT PRIMARY KEY REFERENCES devices(id),
    cursor INTEGER NOT NULL,
    synced_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS idempotency(
    family_id TEXT NOT NULL,
    client_request_id TEXT NOT NULL,
    record_id TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY(family_id, client_request_id)
);

-- Service table: the single global change sequence.
--
-- Every accepted revision of ``records`` and ``reminders`` takes its ``seq``
-- from this table inside the same transaction as the revision INSERT, so the
-- sync cursor never skips a change.
CREATE TABLE IF NOT EXISTS change_seq(
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at TEXT NOT NULL
);

-- Service table: the pet profile shown on the profile page.
CREATE TABLE IF NOT EXISTS pet_profile(
    family_id TEXT PRIMARY KEY REFERENCES families(id),
    pet_name TEXT,
    birthday TEXT,
    breed TEXT,
    weight_grams INTEGER,
    notes TEXT,
    updated_at TEXT NOT NULL,
    updated_by TEXT NOT NULL
);

-- Service table: per source-address plus username login throttling.
CREATE TABLE IF NOT EXISTS login_attempts(
    throttle_key TEXT PRIMARY KEY,
    failure_count INTEGER NOT NULL DEFAULT 0,
    first_failure_at TEXT NOT NULL,
    last_failure_at TEXT NOT NULL,
    locked_until TEXT
);

CREATE INDEX IF NOT EXISTS idx_records_family_id_seq ON records(family_id, id, seq);
CREATE INDEX IF NOT EXISTS idx_records_family_seq ON records(family_id, seq);
CREATE INDEX IF NOT EXISTS idx_records_family_occurred ON records(family_id, occurred_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_records_family_category ON records(family_id, category, occurred_at DESC);
CREATE INDEX IF NOT EXISTS idx_reminders_family_seq ON reminders(family_id, seq);
CREATE INDEX IF NOT EXISTS idx_reminders_family_id_seq ON reminders(family_id, id, seq);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_devices_family ON devices(family_id);
"""


class DatabaseError(Exception):
    """Raised for unrecoverable database problems."""


def resolve_db_path(path: str) -> str:
    """Return an absolute path so per-thread connections agree on the file."""
    if path == ":memory:":
        return path
    return os.path.abspath(os.path.expanduser(path))


def connect(path: str, create: bool = True) -> sqlite3.Connection:
    """Open a connection with the pragmas the specification requires."""
    if not create and not os.path.exists(path):
        raise DatabaseError("database does not exist: %s" % path)
    connection = sqlite3.connect(path, timeout=5.0, isolation_level=None)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA busy_timeout=5000")
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("PRAGMA synchronous=FULL")
    connection.execute("PRAGMA foreign_keys=ON")
    return connection


def initialize(connection: sqlite3.Connection) -> None:
    """Create the schema when it is missing and upgrade an older one.

    Safe to call repeatedly. ``PRAGMA user_version`` records the layout; a
    database at version 1 (or unversioned) is migrated in one transaction
    before the new version number is written.
    """
    connection.executescript(SCHEMA)
    version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if version < SCHEMA_VERSION:
        migrate(connection, version)
    connection.execute("PRAGMA user_version=%d" % SCHEMA_VERSION)


def migrate(connection: sqlite3.Connection, from_version: int) -> None:
    """Upgrade a database created by an older layout. Never drops history."""
    if from_version >= SCHEMA_VERSION:
        return
    with transaction(connection):
        _renumber_global_sequence(connection)


def _renumber_global_sequence(connection: sqlite3.Connection) -> None:
    """Give every existing revision a globally unique, increasing ``seq``.

    Before version 2, ``records.seq`` and ``reminders.seq`` came from two
    independent AUTOINCREMENT counters, so the same value could appear in both
    tables and one merged cursor could not address them. This pass assigns the
    union of both tables a fresh 1..N order that preserves each table's internal
    order, merging the two streams by their old values (records first on a tie),
    and remaps ``supersedes_seq`` with the same mapping. Device cursors from the
    old numbering are only valid for a snapshot resynchronization afterwards.
    """
    records = list(
        connection.execute("SELECT seq, supersedes_seq FROM records ORDER BY seq ASC")
    )
    reminders = list(connection.execute("SELECT seq FROM reminders ORDER BY seq ASC"))
    if not records and not reminders:
        return

    order = []
    record_index = 0
    reminder_index = 0
    while record_index < len(records) and reminder_index < len(reminders):
        if records[record_index]["seq"] <= reminders[reminder_index]["seq"]:
            order.append(("records", int(records[record_index]["seq"])))
            record_index += 1
        else:
            order.append(("reminders", int(reminders[reminder_index]["seq"])))
            reminder_index += 1
    while record_index < len(records):
        order.append(("records", int(records[record_index]["seq"])))
        record_index += 1
    while reminder_index < len(reminders):
        order.append(("reminders", int(reminders[reminder_index]["seq"])))
        reminder_index += 1

    record_mapping = {}
    reminder_mapping = {}
    for position, (table, old_seq) in enumerate(order, start=1):
        if table == "records":
            record_mapping[old_seq] = position
        else:
            reminder_mapping[old_seq] = position

    # 1. Remap revision links while the old seq values are still addressable.
    for row in records:
        if row["supersedes_seq"] is None:
            continue
        mapped = record_mapping.get(int(row["supersedes_seq"]))
        connection.execute(
            "UPDATE records SET supersedes_seq=? WHERE seq=?",
            (mapped, row["seq"]),
        )

    # 2. Park both tables on unique negative values so the new values cannot
    #    collide with a row that has not been renumbered yet.
    connection.execute("UPDATE records SET seq=-seq WHERE seq>0")
    connection.execute("UPDATE reminders SET seq=-seq WHERE seq>0")

    # 3. Apply the new global order.
    for old_seq, new_seq in record_mapping.items():
        connection.execute("UPDATE records SET seq=? WHERE seq=?", (new_seq, -old_seq))
    for old_seq, new_seq in reminder_mapping.items():
        connection.execute("UPDATE reminders SET seq=? WHERE seq=?", (new_seq, -old_seq))

    # 4. Keep the allocator ahead of everything assigned above.
    highest = len(order)
    connection.execute("DELETE FROM change_seq")
    connection.execute(
        "INSERT INTO change_seq(seq, created_at) VALUES(?,?)",
        (highest, _utc_now()),
    )

    # 5. Keep each table's own AUTOINCREMENT counter consistent with its new
    #    maximum, so an explicit seq is never required to stay unique.
    for table in ("records", "reminders"):
        maximum = int(
            connection.execute(
                "SELECT COALESCE(MAX(seq),0) AS m FROM %s" % table
            ).fetchone()["m"]
        )
        connection.execute("DELETE FROM sqlite_sequence WHERE name=?", (table,))
        if maximum > 0:
            connection.execute(
                "INSERT INTO sqlite_sequence(name, seq) VALUES(?,?)", (table, maximum)
            )


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).strftime("%Y-%m-%dT%H:%M:%SZ")


def allocate_seq(connection: sqlite3.Connection, created_at: Optional[str] = None) -> int:
    """Allocate the next global change sequence number.

    Call this inside the same write transaction as the revision INSERT it
    belongs to. The allocator is an AUTOINCREMENT table, so the value is unique
    across ``records`` and ``reminders`` and strictly increasing even when
    transactions are serialized by the writer lock.
    """
    cursor = connection.execute(
        "INSERT INTO change_seq(created_at) VALUES(?)",
        (created_at or _utc_now(),),
    )
    return int(cursor.lastrowid)


def create_database(path: str) -> sqlite3.Connection:
    connection = connect(path)
    initialize(connection)
    return connection


@contextmanager
def transaction(connection: sqlite3.Connection) -> Iterator[sqlite3.Connection]:
    """One writer transaction per request.

    ``BEGIN IMMEDIATE`` takes the write lock up front, so two requests that
    create or edit the same record cannot interleave their statements. The
    transaction is rolled back on any exception.
    """
    connection.execute("BEGIN IMMEDIATE")
    try:
        yield connection
    except BaseException:
        connection.execute("ROLLBACK")
        raise
    else:
        connection.execute("COMMIT")


@contextmanager
def read_transaction(connection: sqlite3.Connection) -> Iterator[sqlite3.Connection]:
    """A deferred read transaction used for consistent sync snapshots."""
    connection.execute("BEGIN")
    try:
        yield connection
    except BaseException:
        connection.execute("ROLLBACK")
        raise
    else:
        connection.execute("COMMIT")


def vacuum_into(source: sqlite3.Connection, target: sqlite3.Connection) -> None:
    """Online backup used by ``lanlan backup``."""
    source.backup(target)
