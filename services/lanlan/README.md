<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan service

The persistent care-recording service behind the Cyber Lanlan phone page and the
passport device. It stores feeding, water, care, cleaning, walking and other
records for one household, keeps every revision, serves the mobile web page and
answers the device sync protocol described in
[cyber-lanlan-service.md](../../docs/applications/cyber-lanlan-service.md).

## Zero third-party dependencies

The service imports nothing outside the Python standard library
(`http.server`, `sqlite3`, `hashlib`, `hmac`, `secrets`, `json`, `csv`,
`zoneinfo`, `argparse`). There is no requirements file, no virtual environment
and no lock file to maintain. It runs on CPython 3.9 through 3.13 from a clean
interpreter; the test suite needs nothing either.

The mobile page is plain HTML, CSS and JavaScript served as static files. It has
no build step, no bundler, no framework and no CDN reference.

Runtime dependencies in the deployment sense are therefore only CPython and a
writable directory for the SQLite database.

## Layout

| Path | Purpose |
| --- | --- |
| `services/lanlan/__main__.py` | `python3 -m lanlan <subcommand>` entry point |
| `services/lanlan/config.py` | environment configuration and production checks |
| `services/lanlan/db.py` | SQLite schema, pragmas and transaction helpers |
| `services/lanlan/model.py` | validation, time handling and row shaping |
| `services/lanlan/auth.py` | password hashing, sessions, device tokens, CSRF, throttling |
| `services/lanlan/api.py` | JSON API routing and queries |
| `services/lanlan/server.py` | threaded HTTP server and static-file host |
| `services/lanlan/export.py` | CSV and JSON writers for every revision |
| `services/lanlan/admin.py` | init, devices, backup, restore and export helpers |
| `services/lanlan/tests/` | unittest suites driven over real HTTP |
| `services/lanlan/run_tests.sh` | test runner used by the repository gate |
| `web/lanlan/` | the mobile page (`index.html`, `styles.css`, `app.js`) |
| `deploy/` | `docker compose` deployment with a Caddy HTTPS proxy |

## Local start

Run from the `services` directory so the package imports without installation:

```bash
cd services
LANLAN_DB=./lanlan.sqlite3 PYTHONDONTWRITEBYTECODE=1 python3 -m lanlan serve
```

The same thing through the environment variable alone:

```bash
cd services
export LANLAN_DB=./lanlan.sqlite3
python3 -m lanlan serve            # http://127.0.0.1:8787/
```

`python3 -m lanlan --help` lists every subcommand:

| Subcommand | Purpose |
| --- | --- |
| `serve` | start the HTTP server (default when the environment is set) |
| `init` | create the family, the two caregivers and the seven disabled reminders |
| `device-create` | create a device credential and print the token once |
| `device-list` | list devices with creation, last-seen and revocation state |
| `device-revoke` | revoke one device credential |
| `backup` | online backup with an integrity check and a SHA-256 sidecar |
| `restore` | restore a backup into the configured database path |
| `export` | write a CSV or JSON export of every revision |
| `check-config` | print the resolved configuration and validate it |

## Environment variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `LANLAN_DB` | `./lanlan.sqlite3` | SQLite database path. Required explicitly in production |
| `LANLAN_HOST` | `127.0.0.1` | bind address (`0.0.0.0` behind a container proxy) |
| `LANLAN_PORT` | `8787` | TCP port |
| `LANLAN_ENV` | `development` | `development` or `production` |
| `LANLAN_SECURE_COOKIES` | `1` in production, else `0` | add `Secure` to the session cookie |
| `LANLAN_WEB_DIR` | `web/lanlan` next to the checkout | static page directory |
| `LANLAN_PBKDF2_ITERATIONS` | `210000` | password hashing cost; lower it only in tests |
| `LANLAN_SESSION_DAYS` | `30` | sliding session lifetime in days |
| `LANLAN_TIMEZONE` | `Asia/Shanghai` | family timezone used at initialization |
| `LANLAN_FAMILY_NAME` | `Lanlan family` | family display name used at initialization |
| `LANLAN_ALLOW_INSECURE` | unset | set to `1` to start production without an explicit database path or secure cookies |

In `LANLAN_ENV=production` the service refuses to start unless `LANLAN_DB` is
set explicitly and `LANLAN_SECURE_COOKIES=1`, unless `LANLAN_ALLOW_INSECURE=1`
is set deliberately. Check the resolved values with:

```bash
cd services
python3 -m lanlan check-config
```

`check-config` prints the settings and ends with `check-config: OK` or a
`FAIL:` line naming the problem; it never prints a password or token.

## First-run initialization

```bash
cd services
export LANLAN_DB=./lanlan.sqlite3
python3 -m lanlan init
```

`init` creates one family, the two caregivers (display names for the two
household members) and the seven reminders, all disabled and without a time. It
prints the generated passwords exactly once:

```text
initialized database: /absolute/path/lanlan.sqlite3
family: Lanlan family (<uuid>, Asia/Shanghai)
reminders created: 7 (all disabled)
caregiver passwords are shown once; store them in a password manager now:
  hehe  <generated>
  yangyang  <generated>
```

Choose the passwords yourself when it is more convenient:

```bash
python3 -m lanlan init --password hehe='a-long-example-password' \
                       --password yangyang='another-example-password' \
                       --timezone Asia/Shanghai --family-name 'Example family'
```

`init` refuses to touch a database that already holds a family unless
`--force` is passed. Nothing in the repository or the database dump contains a
real password: only the PBKDF2 hash is stored.

## Device credentials

A device credential can read records, reminders and family settings and can
never create, edit or revoke records, and never manages accounts.

```bash
cd services
python3 -m lanlan device-create --label passport-a
# device id: <uuid>
# label: passport-a
# token (shown once, store it on the passport now): <token>

python3 -m lanlan device-list
python3 -m lanlan device-revoke <device-id>
```

The plaintext token exists only in that one output; the database stores its
SHA-256 digest. Revocation takes effect on the device's next request. The same
operations are available in the mobile page under "account and export".

## Backup, restore and export

Both commands work on a live database. `backup` uses the SQLite online backup
API, runs `PRAGMA integrity_check` on the copy and writes a `.sha256` file next
to it:

```bash
cd services
python3 -m lanlan backup --out ./backups
# backup written: ./backups/lanlan-<stamp>.sqlite3
# sha256: <digest>
# checksum file: ./backups/lanlan-<stamp>.sqlite3.sha256

python3 -m lanlan restore --from ./backups/lanlan-<stamp>.sqlite3 --force
```

`restore` refuses to overwrite a non-empty database without `--force`, and it
verifies the restored copy before returning. Stop the service first, or expect
in-flight requests to fail while the file is replaced.

Exports always contain every revision, including revoked tombstones; the JSON
form is the lossless one:

```bash
python3 -m lanlan export --format json --out ./lanlan-export.json
python3 -m lanlan export --format csv  --out ./lanlan-export.csv
python3 -m lanlan export --format csv  --out -            # standard output
```

Signed-in caregivers can download the same files from the page at
`/api/v1/export/records.json` and `/api/v1/export/records.csv`.

## Tests

From the repository root:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s services/lanlan/tests -t .
```

Or through the wrapper, which fixes the working directory and lowers the PBKDF2
cost automatically:

```bash
./services/lanlan/run_tests.sh
```

The suite starts a real server on `127.0.0.1` with an ephemeral port and a
temporary database, drives it over HTTP and removes everything afterwards. It
sets `LANLAN_PBKDF2_ITERATIONS=1000` so hashing stays cheap; production keeps
the default 210000. The repository gate runs the same suite; the single line to
add to `run_static_checks` in `tools/validate.sh` is:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s services/lanlan/tests -t .
```

## Deployment

`deploy/docker-compose.yml` builds this service and runs it behind a Caddy
reverse proxy that terminates HTTPS. The full procedure, including DNS,
certificate, backup and restore steps, is in
[deploy/README.md](../../deploy/README.md). In short:

```bash
cd deploy
cp lanlan.env.example lanlan.env      # edit: set LANLAN_DOMAIN
docker compose up -d --build
docker compose exec lanlan python3 -m lanlan init
docker compose exec lanlan python3 -m lanlan device-create --label passport-a
```

## HTTPS requirements

- Serving the page over plain HTTP is a development-only arrangement. In
  production the reverse proxy provides HTTPS and the session cookie is marked
  `Secure`, which is why the service refuses to start in `LANLAN_ENV=production`
  without `LANLAN_SECURE_COOKIES=1`.
- Keep the page and the API on the same origin: the CSRF check compares the
  `Origin` header with the `Host` the browser used.
- Do not log in over an untrusted network without HTTPS; the session cookie is
  the only bearer of authority for a caregiver.

## Editing rules

A record keeps its creator (`created_by`, fixed when the first revision is
written) and its performer (`performed_by`, defaulting to the creator). An edit
may repeat the stored performer but cannot change it: a different value is
rejected with `422` and the field name `performed_by`, because the merge rule
would otherwise rewrite history. `PATCH` and `revoke` require
`expected_version`; a stale value returns `409` together with the current record
so the page can show the conflict instead of overwriting silently. A revoked
record is a tombstone: it stays in the log and in exports, disappears from lists
and summaries, and cannot be edited; a correction is a new record.

## Schema and the sync cursor

`records.seq` and `reminders.seq` are both allocated from one table
(`change_seq`), inside the same transaction as the revision INSERT, so the two
append-only logs share a single strictly increasing change order. That is what
makes `GET /sync/changes` lossless: the cursor is the highest `seq` in the
batch, so no later write can land below a cursor the device already stored. The
specification's table definitions are unchanged; the allocator table is
additive, and every revision still inserts an explicit `seq`.

`PRAGMA user_version` records the layout, currently `2`. A database written by
layout 1, where the two tables had independent AUTOINCREMENT counters and could
reuse the same `seq` value, is migrated in one transaction by the first command
that opens it: every revision from both tables is renumbered into a single 1..N
order that preserves each table's internal order (records first on a tie) and
`supersedes_seq` revision links are remapped with it, so no row and no link is
lost. After such an upgrade a device must resynchronize once with
`/sync/snapshot`, because cursors from the old numbering are meaningless;
`/sync/changes` answers `409 cursor_invalid` when a cursor is newer than the
log.

## Device sync responses

`GET /api/v1/sync/changes` and `GET /api/v1/sync/snapshot` return the same
top-level shape:

| Field | Meaning |
| --- | --- |
| `server_time` | RFC 3339 UTC server clock, so the device can detect skew |
| `timezone` / `utc_offset_minutes` | Family zone and its offset at `server_time` |
| `cursor` / `has_more` | Highest `seq` in the batch, and whether more remain |
| `records` / `reminders` | Compact changed rows in `seq` order |
| `revoked` | Stable ids the device must delete from its cache |
| `members` | The family's enabled caregivers: `id`, `display_name`, `username` |

`members` is identical on every page and every response, so the device reads it
once and caches the `id` to display-name mapping; it never has to invent a
mapping from first-seen order, which would be able to swap the two caregivers.
The order is creation time, then username, then id, which keeps the list stable
even though the initialization writes both caregivers within the same second.
It is the only account view a device credential receives: no password material,
session data, mail address or any other account column is included. A device
that ignores the field keeps working unchanged.

## Security notes

- Passwords: PBKDF2-HMAC-SHA256, 16-byte random salt per user, 210000
  iterations, stored as `pbkdf2_sha256$<iterations>$<salt>$<hash>`.
- Sessions: 32 random bytes, only the SHA-256 digest is stored, 30-day sliding
  expiry, cookie `HttpOnly` + `SameSite=Lax` + `Path=/` and `Secure` in
  production.
- Device tokens: 32 random bytes, stored as a digest, revocable, read-only.
- CSRF: a session-bound token must be sent in `X-Lanlan-CSRF` and the request
  must carry a same-origin `Origin` header.
- Login throttling: per source address plus username, exponential backoff with
  a short lockout window.
- Family scoping: every query filters on the family resolved from the session or
  the device credential, so another household sees nothing and gets `403`/`404`.
- Logging: method, query-free path, status and duration only. Record content,
  request bodies, passwords, tokens and query strings are never logged.
- Database: SQLite with WAL, `synchronous=FULL`, `busy_timeout=5000` and foreign
  keys enabled; records and reminders are append-only revision logs, so nothing
  is silently overwritten or deleted.
- Never commit a real database, a family record, a device token or a photograph.
