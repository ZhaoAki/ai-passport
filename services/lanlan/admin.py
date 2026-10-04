"""Administrative operations: initialize, devices, backup, restore and export.

These commands are what an operator runs on the host. They never print record
content, passwords or tokens beyond the single moment a new credential is
generated, which is the only time the plaintext exists.
"""

from __future__ import annotations

import hashlib
import os
import secrets
import shutil
import sqlite3
import sys
from typing import Any, Dict, List, Optional, Tuple

from . import auth, export as export_module, model
from .config import Config, DEFAULT_TIMEZONE
from . import db as db_module

# Display names come from the frozen specification: Hehe is 赫赫 and Yangyang is
# 羊羊. The service is the source of truth for these labels; the device receives
# them through the sync `members` field instead of hardcoding a mapping.
DEFAULT_CAREGIVERS: Tuple[Tuple[str, str], ...] = (
    ("hehe", "赫赫"),
    ("yangyang", "羊羊"),
)

BACKUP_SUFFIX = ".sha256"


class AdminError(Exception):
    """A recoverable administrative failure."""


def generate_password() -> str:
    """A readable one-time password; printed once and never stored in clear."""
    return secrets.token_urlsafe(12)


def database_is_empty(connection: sqlite3.Connection) -> bool:
    row = connection.execute("SELECT COUNT(*) AS n FROM families").fetchone()
    if int(row["n"]) > 0:
        return False
    row = connection.execute("SELECT COUNT(*) AS n FROM users").fetchone()
    return int(row["n"]) == 0


def create_family(
    connection: sqlite3.Connection,
    name: str,
    timezone_name: str = DEFAULT_TIMEZONE,
) -> str:
    family_id = auth.new_id()
    connection.execute(
        "INSERT INTO families(id, name, timezone, created_at) VALUES(?,?,?,?)",
        (family_id, name, timezone_name, model.format_utc(model.now_utc())),
    )
    return family_id


def create_caregiver(
    connection: sqlite3.Connection,
    family_id: str,
    username: str,
    display_name: str,
    password: str,
    iterations: int,
) -> str:
    user_id = auth.new_id()
    connection.execute(
        "INSERT INTO users(id, family_id, username, display_name, password_hash, created_at)"
        " VALUES(?,?,?,?,?,?)",
        (
            user_id,
            family_id,
            username,
            display_name,
            auth.hash_password(password, iterations),
            model.format_utc(model.now_utc()),
        ),
    )
    return user_id


def create_disabled_reminders(connection: sqlite3.Connection, family_id: str, updated_by: str) -> List[str]:
    """Create the seven reminders, all disabled and without a time."""
    now = model.format_utc(model.now_utc())
    created: List[str] = []
    for category, subitem in model.reminder_specs():
        reminder_id = auth.new_id()
        connection.execute(
            "INSERT INTO reminders(seq, id, version, family_id, category, subitem, custom_name,"
            " enabled, schedule_type, time_local, created_at, updated_at, updated_by)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                db_module.allocate_seq(connection, now),
                reminder_id,
                1,
                family_id,
                category,
                subitem,
                None,
                0,
                model.SCHEDULE_DAILY,
                None,
                now,
                now,
                updated_by,
            ),
        )
        created.append(reminder_id)
    return created


def initialize_database(
    connection: sqlite3.Connection,
    config: Config,
    family_name: Optional[str] = None,
    timezone_name: Optional[str] = None,
    passwords: Optional[Dict[str, str]] = None,
    caregivers: Optional[List[Tuple[str, str]]] = None,
    force: bool = False,
) -> Dict[str, Any]:
    """Create the family, the two caregivers and the seven reminders."""
    passwords = passwords or {}
    caregivers = caregivers or list(DEFAULT_CAREGIVERS)
    timezone_name = timezone_name or config.timezone
    family_name = family_name or config.family_name

    if not database_is_empty(connection) and not force:
        raise AdminError(
            "this database already holds a family; pass --force to add another one deliberately"
        )

    generated: Dict[str, str] = {}
    with db_module.transaction(connection):
        family_id = create_family(connection, family_name, timezone_name)
        first_user_id = ""
        for username, display_name in caregivers:
            password = passwords.get(username) or generate_password()
            if len(password) < 8:
                raise AdminError("password for %s must be at least 8 characters" % username)
            user_id = create_caregiver(
                connection,
                family_id,
                username,
                display_name,
                password,
                config.pbkdf2_iterations,
            )
            generated[username] = password
            if not first_user_id:
                first_user_id = user_id
        if not first_user_id:
            raise AdminError("at least one caregiver is required")
        reminder_ids = create_disabled_reminders(connection, family_id, first_user_id)

    return {
        "family_id": family_id,
        "family_name": family_name,
        "timezone": timezone_name,
        "caregivers": [username for username, _ in caregivers],
        "passwords": generated,
        "reminder_ids": reminder_ids,
    }


def ensure_family(connection: sqlite3.Connection, family_id: Optional[str] = None) -> str:
    """Resolve the family id, failing loudly for an empty or unknown database."""
    rows = list(connection.execute("SELECT id FROM families ORDER BY created_at"))
    if not rows:
        raise SystemExit("no family in this database; run 'python3 -m lanlan init' first")
    if family_id is not None:
        for row in rows:
            if row["id"] == family_id:
                return str(family_id)
        raise SystemExit("no such family: %s" % family_id)
    return str(rows[0]["id"])


def list_devices(connection: sqlite3.Connection, family_id: str) -> List[sqlite3.Row]:
    return list(
        connection.execute(
            "SELECT id, label, created_at, last_seen_at, revoked_at FROM devices"
            " WHERE family_id=? ORDER BY created_at, id",
            (family_id,),
        )
    )


def create_device(
    connection: sqlite3.Connection, family_id: str, label: str
) -> Tuple[str, str]:
    with db_module.transaction(connection):
        return auth.create_device(connection, family_id, label)


def revoke_device(connection: sqlite3.Connection, family_id: str, device_id: str) -> bool:
    row = connection.execute(
        "SELECT id, revoked_at FROM devices WHERE id=? AND family_id=?", (device_id, family_id)
    ).fetchone()
    if row is None:
        return False
    if row["revoked_at"]:
        return True
    with db_module.transaction(connection):
        auth.revoke_device(connection, device_id, model.format_utc(model.now_utc()))
    return True


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_checksum(path: str) -> str:
    digest = sha256_file(path)
    with open(path + BACKUP_SUFFIX, "w", encoding="utf-8") as handle:
        handle.write("%s  %s\n" % (digest, os.path.basename(path)))
    return digest


def verify_checksum(path: str) -> Optional[bool]:
    checksum_path = path + BACKUP_SUFFIX
    if not os.path.isfile(checksum_path):
        return None
    with open(checksum_path, "r", encoding="utf-8") as handle:
        first = handle.readline().strip().split()
    if not first:
        return False
    return first[0] == sha256_file(path)


def backup_database(source_path: str, out_dir: str, label: Optional[str] = None) -> str:
    """Online backup plus an integrity check and a SHA-256 sidecar."""
    if not os.path.isfile(source_path):
        raise AdminError("no database at %s" % source_path)
    os.makedirs(out_dir, exist_ok=True)
    stamp = model.format_utc(model.now_utc()).replace(":", "").replace("-", "")
    name = label or ("lanlan-%s.sqlite3" % stamp)
    target_path = os.path.join(out_dir, name)
    if os.path.exists(target_path):
        raise AdminError("refusing to overwrite %s" % target_path)

    source = db_module.connect(source_path)
    target = sqlite3.connect(target_path)
    try:
        db_module.vacuum_into(source, target)
        target.commit()
    finally:
        target.close()
        source.close()

    check = sqlite3.connect(target_path)
    try:
        result = check.execute("PRAGMA integrity_check").fetchone()[0]
    finally:
        check.close()
    if result != "ok":
        raise AdminError("backup integrity check failed: %s" % result)
    # Opening the copy can leave WAL sidecar files behind; a backup is one file.
    for suffix in ("-wal", "-shm"):
        sidecar = target_path + suffix
        if os.path.exists(sidecar):
            os.remove(sidecar)
    write_checksum(target_path)
    return target_path


def restore_database(source_path: str, target_path: str, force: bool = False) -> str:
    """Copy a backup over the live database, refusing non-empty targets."""
    if not os.path.isfile(source_path):
        raise AdminError("no backup at %s" % source_path)
    if os.path.exists(target_path) and os.path.getsize(target_path) > 0 and not force:
        raise AdminError(
            "refusing to overwrite the non-empty database %s; pass --force to replace it"
            % target_path
        )
    os.makedirs(os.path.dirname(os.path.abspath(target_path)) or ".", exist_ok=True)
    shutil.copyfile(source_path, target_path)
    connection = sqlite3.connect(target_path)
    try:
        result = connection.execute("PRAGMA integrity_check").fetchone()[0]
        if result != "ok":
            raise AdminError("restored database failed its integrity check: %s" % result)
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=FULL")
    finally:
        connection.close()
    return target_path


def export_to_file(
    connection: sqlite3.Connection, family_id: str, fmt: str, out_path: Optional[str]
) -> str:
    if fmt == "csv":
        text = export_module.export_csv(connection, family_id)
    elif fmt == "json":
        text = export_module.dumps_json(export_module.export_json(connection, family_id))
    else:
        raise AdminError("unknown export format: %s" % fmt)
    if out_path is None or out_path == "-":
        sys.stdout.write(text)
        sys.stdout.flush()
        return "-"
    with open(out_path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    return out_path
