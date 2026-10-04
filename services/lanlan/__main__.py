"""Command line entry point: ``python3 -m lanlan <subcommand>``.

Run from the ``services`` directory (or with ``PYTHONPATH=services``) so that
the ``lanlan`` package is importable without installation.
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional, Sequence

from . import __version__, admin, config as config_module, db as db_module
from .config import Config, ConfigError


PROGRAM = "python3 -m lanlan"

EPILOG = """\
Examples:
  python3 -m lanlan init --db ./lanlan.sqlite3
  python3 -m lanlan serve
  python3 -m lanlan device-create --label passport-a
  python3 -m lanlan backup --out ./backups
  python3 -m lanlan export --format json --out ./lanlan-export.json

All settings come from the environment; see services/lanlan/README.md.
Configuration is read from LANLAN_DB, LANLAN_HOST, LANLAN_PORT, LANLAN_ENV,
LANLAN_SECURE_COOKIES, LANLAN_WEB_DIR, LANLAN_PBKDF2_ITERATIONS,
LANLAN_SESSION_DAYS, LANLAN_TIMEZONE and LANLAN_FAMILY_NAME.
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROGRAM,
        description="Cyber Lanlan care-recording service (standard library only).",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--version", action="version", version="lanlan %s" % __version__)
    subparsers = parser.add_subparsers(dest="command", metavar="<subcommand>")

    serve = subparsers.add_parser("serve", help="start the HTTP server")
    serve.add_argument("--db", help="override LANLAN_DB for this run")
    serve.add_argument("--host", help="override LANLAN_HOST for this run")
    serve.add_argument("--port", type=int, help="override LANLAN_PORT for this run")

    init = subparsers.add_parser(
        "init", help="create the family, the two caregivers and the seven disabled reminders"
    )
    init.add_argument("--db", help="override LANLAN_DB for this run")
    init.add_argument("--family-name", help="family display name")
    init.add_argument("--timezone", help="IANA timezone, default Asia/Shanghai")
    init.add_argument(
        "--password",
        action="append",
        default=[],
        metavar="USERNAME=PASSWORD",
        help="set a caregiver password explicitly; may be repeated",
    )
    init.add_argument(
        "--caregiver",
        action="append",
        default=[],
        metavar="USERNAME=DISPLAY_NAME",
        help="override the default caregiver list; may be repeated",
    )
    init.add_argument("--force", action="store_true", help="add a family to a non-empty database")

    device_create = subparsers.add_parser(
        "device-create", help="create a device credential and print its token once"
    )
    device_create.add_argument("--db", help="override LANLAN_DB for this run")
    device_create.add_argument("--label", default="passport", help="device label")
    device_create.add_argument("--family-id", help="family to attach the device to")

    device_list = subparsers.add_parser("device-list", help="list device credentials")
    device_list.add_argument("--db", help="override LANLAN_DB for this run")
    device_list.add_argument("--family-id", help="family to list")

    device_revoke = subparsers.add_parser("device-revoke", help="revoke a device credential")
    device_revoke.add_argument("device_id", help="device id from device-list")
    device_revoke.add_argument("--db", help="override LANLAN_DB for this run")
    device_revoke.add_argument("--family-id", help="family that owns the device")

    backup = subparsers.add_parser("backup", help="online backup with an integrity check and SHA-256")
    backup.add_argument("--db", help="database to back up")
    backup.add_argument("--out", default="./backups", help="output directory")
    backup.add_argument("--name", help="backup file name")

    restore = subparsers.add_parser("restore", help="restore a backup into the database path")
    restore.add_argument("--db", help="destination database path")
    restore.add_argument("--from", dest="source", required=True, help="backup file to restore")
    restore.add_argument("--force", action="store_true", help="replace a non-empty database")

    export = subparsers.add_parser("export", help="write a CSV or JSON export of every revision")
    export.add_argument("--db", help="database to export")
    export.add_argument("--format", choices=("csv", "json"), default="json", help="export format")
    export.add_argument("--out", help="output file, or - for standard output")
    export.add_argument("--family-id", help="family to export")

    check = subparsers.add_parser("check-config", help="validate the environment configuration")
    check.add_argument("--db", help="treat this path as LANLAN_DB when checking")

    return parser


def _parse_pairs(values: Sequence[str], label: str) -> "dict":
    parsed = {}
    for value in values:
        name, separator, rest = value.partition("=")
        if not separator or not name.strip() or not rest:
            raise SystemExit("%s must look like NAME=VALUE, got %r" % (label, value))
        parsed[name.strip()] = rest
    return parsed


def _open_db(path: str, create: bool = True):
    """Open a database, creating or migrating the schema as needed.

    Every administrative subcommand goes through here, so an older database is
    upgraded once by whichever command touches it first instead of staying in
    the old layout until `serve` runs.
    """
    connection = db_module.connect(path, create=create)
    db_module.initialize(connection)
    return connection


def _print_passwords(result: dict) -> None:
    """Print generated credentials once, on stdout, marked as one-time."""
    print("family: %s (%s, %s)" % (result["family_name"], result["family_id"], result["timezone"]))
    print("reminders created: %d (all disabled)" % len(result["reminder_ids"]))
    print("caregiver passwords are shown once; store them in a password manager now:")
    for username, password in result["passwords"].items():
        print("  %s  %s" % (username, password))


def command_init(args: argparse.Namespace, config: Config) -> int:
    if args.timezone:
        config.timezone = args.timezone
    if args.family_name:
        config.family_name = args.family_name
    passwords = _parse_pairs(args.password, "--password")
    if args.caregiver:
        caregivers = [
            (name, display)
            for name, display in (
                pair.split("=", 1) if "=" in pair else (pair, pair) for pair in args.caregiver
            )
        ]
    else:
        caregivers = None
    path = db_module.resolve_db_path(args.db or config.db_path)
    connection = _open_db(path)
    try:
        result = admin.initialize_database(
            connection,
            config,
            family_name=config.family_name,
            timezone_name=config.timezone,
            passwords=passwords,
            caregivers=caregivers,
            force=args.force,
        )
    except admin.AdminError as error:
        raise SystemExit("init failed: %s" % error)
    finally:
        connection.close()
    print("initialized database: %s" % path)
    _print_passwords(result)
    return 0


def command_device_create(args: argparse.Namespace, config: Config) -> int:
    path = db_module.resolve_db_path(args.db or config.db_path)
    connection = _open_db(path, create=False)
    try:
        family_id = admin.ensure_family(connection, args.family_id)
        device_id, token = admin.create_device(connection, family_id, args.label)
    finally:
        connection.close()
    print("device id: %s" % device_id)
    print("label: %s" % args.label)
    print("token (shown once, store it on the passport now): %s" % token)
    return 0


def command_device_list(args: argparse.Namespace, config: Config) -> int:
    path = db_module.resolve_db_path(args.db or config.db_path)
    connection = _open_db(path, create=False)
    try:
        family_id = admin.ensure_family(connection, args.family_id)
        rows = admin.list_devices(connection, family_id)
    finally:
        connection.close()
    if not rows:
        print("no devices")
        return 0
    print("id                                   label            created_at            last_seen_at          revoked_at")
    for row in rows:
        print(
            "%-36s %-16s %-21s %-21s %s"
            % (
                row["id"],
                row["label"][:16],
                row["created_at"],
                row["last_seen_at"] or "-",
                row["revoked_at"] or "-",
            )
        )
    return 0


def command_device_revoke(args: argparse.Namespace, config: Config) -> int:
    path = db_module.resolve_db_path(args.db or config.db_path)
    connection = _open_db(path, create=False)
    try:
        family_id = admin.ensure_family(connection, args.family_id)
        found = admin.revoke_device(connection, family_id, args.device_id)
    finally:
        connection.close()
    if not found:
        raise SystemExit("no such device in this family: %s" % args.device_id)
    print("revoked device: %s" % args.device_id)
    return 0


def command_backup(args: argparse.Namespace, config: Config) -> int:
    path = db_module.resolve_db_path(args.db or config.db_path)
    try:
        target = admin.backup_database(path, args.out, args.name)
    except admin.AdminError as error:
        raise SystemExit("backup failed: %s" % error)
    digest = admin.sha256_file(target)
    print("backup written: %s" % target)
    print("sha256: %s" % digest)
    print("checksum file: %s%s" % (target, admin.BACKUP_SUFFIX))
    return 0


def command_restore(args: argparse.Namespace, config: Config) -> int:
    path = db_module.resolve_db_path(args.db or config.db_path)
    try:
        admin.restore_database(args.source, path, force=args.force)
    except admin.AdminError as error:
        raise SystemExit("restore failed: %s" % error)
    print("restored %s -> %s" % (args.source, path))
    return 0


def command_export(args: argparse.Namespace, config: Config) -> int:
    path = db_module.resolve_db_path(args.db or config.db_path)
    connection = _open_db(path, create=False)
    try:
        family_id = admin.ensure_family(connection, args.family_id)
        target = admin.export_to_file(connection, family_id, args.format, args.out)
    finally:
        connection.close()
    if target != "-":
        print("export written: %s" % target)
    return 0


def command_check_config(args: argparse.Namespace, config: Config) -> int:
    environment = dict(os.environ)
    if args.db:
        environment["LANLAN_DB"] = args.db
    try:
        _, report = config_module.check_config(environment)
    except ConfigError as error:
        print("check-config: FAIL")
        print("FAIL: %s" % error)
        return 1
    print(report)
    return 0 if report.rstrip().endswith("OK") else 1


def command_serve(args: argparse.Namespace, config: Config) -> int:
    from .server import serve_forever

    if args.host:
        config.host = args.host
    if args.port:
        config.port = args.port
    return serve_forever(config, args.db)


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 2

    try:
        config = Config.from_env()
    except ConfigError as error:
        raise SystemExit("configuration error: %s" % error)

    handlers = {
        "serve": command_serve,
        "init": command_init,
        "device-create": command_device_create,
        "device-list": command_device_list,
        "device-revoke": command_device_revoke,
        "backup": command_backup,
        "restore": command_restore,
        "export": command_export,
        "check-config": command_check_config,
    }
    handler = handlers.get(args.command)
    if handler is None:
        parser.print_help()
        return 2
    try:
        return handler(args, config)
    except db_module.DatabaseError as error:
        raise SystemExit("database error: %s" % error)


if __name__ == "__main__":
    sys.exit(main())
