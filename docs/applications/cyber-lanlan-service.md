<p align="right"><a href="cyber-lanlan-service.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan Service and Mobile Web (M0 design)

This document fixes the first-generation service design for the Cyber Lanlan pet-care
recorder: the persistent cloud service, the mobile web input surface, the record model,
authentication, the device sync protocol, capacity limits, and the reminder rules. The
device application itself is described in [cyber-lanlan.md](cyber-lanlan.md); verification
steps and acceptance mapping are in [cyber-lanlan-testing.md](cyber-lanlan-testing.md).

Nothing in this document enables the deferred scope: no MI Home or camera integration, no
remote feeding control, no video, no phone lock-screen push, no Korean learning content,
no AI chat, and no hunger/death/leaving penalties. Virtual pet interaction never creates a
real care record.

## 1. Scope of this increment

| In scope | Out of scope |
| --- | --- |
| Two caregivers (Hehe, Yangyang) recording feeding, water, care, cleaning, walking and other items from a phone web page | Lock-screen push notifications |
| Server-side persistence as the authoritative record store | Automatic event capture from pet appliances |
| Revision, revocation and audit trail for every record | Silent overwrite of another caregiver's edit |
| Device sync with stable IDs, versions and an incremental cursor | Remote device control |
| Bounded on-device cache of recent records and reminders | Unbounded history on the device |
| Care reminders that start disabled and are configured later by the owner | Preset care intervals of any kind |
| CSV/JSON export, database backup and restore | Paid hosting resources (deferred until a budget decision) |

## 2. Deployment candidates

The household has no always-on computer, so a phone-visible service must eventually run on
a hosted machine. Until the budget decision, the deliverable is a reproducible local
service plus a deployable configuration.

| Candidate | Shape | Notes |
| --- | --- | --- |
| A. Local development (default now) | `python3 -m lanlan` on the development machine, SQLite file on disk | Closed loop for web plus device on the same LAN. Not an always-on deployment; the phone can only reach it while the machine is on. |
| B. Single small VPS | `docker compose` with the service container plus a Caddy reverse proxy terminating HTTPS | Recommended first hosted target. One domain, one volume, automatic certificates. Requires a budget decision. |
| C. Managed container platform | Same image pushed to a container host with a persistent volume | Acceptable if the platform provides a persistent disk. Object-storage-only platforms do not fit SQLite. |

All candidates use the same image, the same environment variables and the same SQLite file
layout, so promoting A to B is a deployment change only. HTTPS is mandatory in production;
credentials come from environment variables or deployment secrets and are never committed.
The service refuses to start in `LANLAN_ENV=production` without an explicit database path
and a secure-cookie setting.

Runtime dependencies are deliberately zero third-party packages: CPython 3.11 or newer with
the standard library only (`http.server`, `sqlite3`, `hashlib`, `hmac`, `secrets`, `json`,
`csv`, `zoneinfo`). The mobile web page is plain HTML/CSS/JavaScript served as static files;
it has no build step and no bundler. This keeps the lock file empty by design and makes the
service reproducible on any machine with Python.

## 3. Accounts, roles and authorization

- One **family** (household) per deployment. It carries the display name and the family
  timezone (initial value `Asia/Shanghai`, changeable in settings).
- Family members are **caregivers**. The first generation ships Hehe and Yangyang, created
  by the initialization command. Caregivers have identical permissions: both can view,
  create, edit and revoke shared records.
- A record stores **creator** and **performer** separately, so Hehe can record a walk that
  Yangyang performed. Editing never rewrites either field silently; the revision history
  keeps who changed what and when.
- A **device credential** is a separate identity for the passport. It can read records,
  reminders and family settings, and can never create, edit or revoke records, and never
  manages accounts. It is revocable at any time; revocation takes effect on the next
  request.
- Every record endpoint requires an authenticated caregiver session or a device token.
  Unauthenticated requests receive `401` and no record content. A caregiver from another
  family receives `403` or `404`; family scoping is applied inside every database query,
  not in the page layer.

### 3.1 Password and token handling

| Item | Decision |
| --- | --- |
| Password storage | PBKDF2-HMAC-SHA256, 210000 iterations, 16-byte random salt per user, stored as `pbkdf2_sha256$<iterations>$<salt>$<hash>` |
| Session token | 32 random bytes, URL-safe encoded; the database stores only its SHA-256 digest; 30-day sliding expiry |
| Session cookie | `HttpOnly`, `SameSite=Lax`, `Path=/`, `Secure` when the deployment is HTTPS |
| Device token | 32 random bytes; stored as SHA-256 digest with device id, label, creation time, last-seen time and revocation time |
| CSRF | Session-bound random token returned at login; every state-changing request must send it in `X-Lanlan-CSRF` and must carry a same-origin `Origin` header |
| Login throttling | Per source-address plus username failure counter with exponential backoff and a short lockout window |
| Secret material | Environment variables or deployment secrets only; never written into the repository, the database dump, the export files or log lines |

No sample configuration in the repository contains a real password, token, device secret,
care record or family photograph.

## 4. Record model

### 4.1 Tables

```sql
families(id TEXT PRIMARY KEY, name TEXT NOT NULL, timezone TEXT NOT NULL, created_at TEXT NOT NULL)
users(id TEXT PRIMARY KEY, family_id TEXT NOT NULL REFERENCES families(id),
      username TEXT NOT NULL UNIQUE, display_name TEXT NOT NULL,
      password_hash TEXT NOT NULL, created_at TEXT NOT NULL, disabled_at TEXT)
sessions(token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id),
         csrf TEXT NOT NULL, created_at TEXT NOT NULL, expires_at TEXT NOT NULL, last_seen_at TEXT NOT NULL)
devices(id TEXT PRIMARY KEY, family_id TEXT NOT NULL REFERENCES families(id), label TEXT NOT NULL,
        token_hash TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL, last_seen_at TEXT, revoked_at TEXT)
records(seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL, version INTEGER NOT NULL,
        family_id TEXT NOT NULL REFERENCES families(id),
        category TEXT NOT NULL, subitem TEXT, custom_name TEXT,
        occurred_at TEXT NOT NULL, occurred_tz TEXT NOT NULL, time_confidence TEXT NOT NULL,
        created_at TEXT NOT NULL, created_by TEXT NOT NULL, performed_by TEXT NOT NULL,
        amount_value REAL, amount_unit TEXT, duration_minutes INTEGER, note TEXT,
        status TEXT NOT NULL, source TEXT NOT NULL, client_request_id TEXT,
        supersedes_seq INTEGER, revoke_reason TEXT,
        UNIQUE(id, version))
reminders(seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL, version INTEGER NOT NULL,
        family_id TEXT NOT NULL REFERENCES families(id),
        category TEXT NOT NULL, subitem TEXT, custom_name TEXT,
        enabled INTEGER NOT NULL DEFAULT 0, schedule_type TEXT NOT NULL DEFAULT 'daily',
        time_local TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, updated_by TEXT NOT NULL,
        UNIQUE(id, version))
device_acks(device_id TEXT PRIMARY KEY REFERENCES devices(id), cursor INTEGER NOT NULL,
        synced_at TEXT NOT NULL)
idempotency(family_id TEXT NOT NULL, client_request_id TEXT NOT NULL, record_id TEXT NOT NULL,
        created_at TEXT NOT NULL, PRIMARY KEY(family_id, client_request_id))
```

`records` and `reminders` are append-only revision logs: every accepted change inserts a new
row with `version + 1` and the same `id`. `seq` is a **globally unique, monotonically
increasing change number** drawn from one sequence shared by both tables and allocated in
the same transaction as the revision insert (the service keeps a dedicated allocator table
for it). A single cursor over both tables therefore can never skip a change, which two
independent per-table row-id sequences would not guarantee. The current state of a record is
its highest-version row. There is no `UPDATE` and no `DELETE` on either table, which is what
makes the incremental cursor, the audit trail and the tombstone rule consistent with each
other. Statistics and lists read only rows where `status` is `active` and the version is
current.

### 4.2 Field semantics

| Field | Meaning |
| --- | --- |
| `id` | Stable UUIDv4 string that never changes across revisions; the device cache key |
| `version` | Integer starting at 1, incremented on every accepted edit or revocation |
| `category` | One of `meal`, `water`, `care`, `cleaning`, `walk`, `other` |
| `subitem` | Fixed sub-item for `care` (`bath`, `grooming`, `teeth`, `comb`, `other`) or a preset key for `cleaning`; `NULL` otherwise |
| `custom_name` | Short owner-defined name for `cleaning` and for `care/other` and `other`, 1 to 12 characters |
| `occurred_at` | Event time, stored as RFC 3339 UTC with second precision |
| `occurred_tz` | IANA zone the caregiver used when entering the event; used to reproduce the local wall-clock time |
| `time_confidence` | `trusted` when the caregiver picked or confirmed the time, `estimated` for an explicitly approximated backfill |
| `created_at` / `created_by` | Server time and the caregiver who submitted the record |
| `performed_by` | The caregiver who actually did the work; defaults to the creator, may be the other caregiver |
| `amount_value` / `amount_unit` | Optional quantity; **`NULL` means unknown and must never be rendered or stored as 0** |
| `duration_minutes` | Optional whole minutes, currently used by `walk` |
| `note` | Optional free text, at most 200 characters |
| `status` | `active` or `revoked` |
| `source` | Always `manual` in the first generation; the field exists so a future integration can be distinguished without a migration |
| `client_request_id` | Retry identifier supplied by the web page; unique per family |
| `supersedes_seq` | The `seq` of the revision this row replaces, `NULL` for a first version |
| `revoke_reason` | Optional short reason recorded when a caregiver revokes a record |

A revoked record is a tombstone: it stays in the log, disappears from lists and summaries,
and is delivered to the device so the cache cannot resurrect it. Revoking is reversible in
the sense that the history remains inspectable, but there is no "unrevoke" operation in the
first generation; a correction is a new record.

### 4.3 Category and unit validation

Validation lives in the model layer (`services/lanlan/model.py`) and is applied to every
write path, including device-authenticated ones, so a validator bug in the web page cannot
produce an invalid row.

| Category | Allowed sub-items | Amount | Duration | Custom name |
| --- | --- | --- | --- | --- |
| `meal` | none | optional, unit in `g`, `ml`, `scoop`, `cup`, `piece`, `bag` | no | no |
| `water` | none | optional, unit in `ml`, `bowl` | no | no |
| `care` | `bath`, `grooming`, `teeth`, `comb`, `other` | no | no | required only for `other` |
| `cleaning` | preset keys (`ear`, `paw`, `pad`, `litter`, `other`) | no | no | required for `other`, optional label for presets |
| `walk` | none | no | optional, 1 to 1440 minutes | no |
| `other` | none | no | no | required, 1 to 12 characters |

Additional server-side rules: `amount_value` must be greater than 0 and at most 9999 when
present; `occurred_at` must be between 400 days in the past and 1 day in the future relative
to server time; `note` is limited to 200 characters; `performed_by` must be a member of the
same family. Violations return `422` with a machine-readable field error and the record is
not stored.

## 5. HTTP API

All endpoints live under `/api/v1` and exchange JSON unless stated otherwise. Errors use
`{"error": {"code": "...", "message": "...", "field": "..."}}`. Caregiver endpoints require
the session cookie plus the CSRF header on writes; device endpoints require
`Authorization: Bearer <device token>`.

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| `POST` | `/auth/login` | none | Start a caregiver session; returns the user, the family and the CSRF token |
| `POST` | `/auth/logout` | session | End the session |
| `GET` | `/me` | session | Current caregiver, family timezone and family members |
| `POST` | `/records` | session | Create a record; `client_request_id` makes retries idempotent |
| `GET` | `/records` | session | List records with `from`, `to`, `category`, `limit` and keyset cursor |
| `GET` | `/records/{id}` | session | Current revision plus the full revision history |
| `PATCH` | `/records/{id}` | session | Edit a record; body carries `expected_version` |
| `POST` | `/records/{id}/revoke` | session | Revoke a record; body carries `expected_version` and an optional reason |
| `GET` | `/summary/today` | session | Today's counts per category and the latest feeding/water/walk, in family timezone |
| `GET` | `/reminders` | session | Reminder list with enabled flag, schedule type and time |
| `PATCH` | `/reminders/{id}` | session | Enable, disable or set the time of one reminder |
| `GET` | `/export/records.csv`, `/export/records.json` | session | Full export, all revisions, including revoked tombstones |
| `POST` | `/devices` | session | Create a device credential; the plaintext token is returned once |
| `GET` | `/devices` | session | List devices with last-seen and revocation state |
| `POST` | `/devices/{id}/revoke` | session | Revoke a device credential |
| `GET` | `/sync/changes` | device | Incremental changes after a cursor |
| `GET` | `/sync/snapshot` | device | Paged full resynchronization |
| `POST` | `/sync/ack` | device | Report the applied cursor and the device clock for diagnostics |
| `GET`, `PATCH` | `/profile` | session | Pet profile fields shown on the profile page |
| `POST` | `/auth/change-password` | session | Change the signed-in caregiver's password |
| `GET` | `/status` | session | Service status shown on the account page |

Both sync responses also carry `members`, the family's caregivers as
`[{"id", "display_name", "username"}]`, so the passport can label the creator and the
performer without holding family-management privileges.

The implementation additionally keeps a `login_attempts` table for login throttling, a
`pet_profile` row for the profile page, and the `change_seq` allocator that provides the
global change sequence described in section 4.1. Those are service-internal; the record and
reminder tables above are created exactly as written.

`GET /` and `/assets/*` serve the mobile web page from `web/lanlan/`. Those files contain no
family data; record data is only reachable through the authenticated JSON API.

### 5.1 Write semantics

1. The web page generates a `client_request_id` (UUIDv4) when the form is opened and keeps
   it across retries of the same submission.
2. The service inserts the record and the idempotency row in one transaction. A repeated
   `client_request_id` returns the previously created record with HTTP `200` and
   `"idempotent_replay": true` instead of storing a second row.
3. The web page tells the caregiver "saved to the service" only after that response. A
   failure keeps the form contents and offers retry with the same identifier. Failure is
   never presented as success.
4. `PATCH` and `revoke` require `expected_version`. If the stored version is newer, the
   service returns `409` with the current record so the page can show the conflict and let
   the caregiver choose; it never overwrites silently.

## 6. Sync protocol

The service database is the authority; the passport keeps a bounded recent cache.

### 6.1 Incremental changes

```text
GET /api/v1/sync/changes?cursor=<integer>&limit=<1..100>
Authorization: Bearer <device token>

200 OK
{
  "server_time": "2026-10-04T12:00:00Z",
  "timezone": "Asia/Shanghai",
  "utc_offset_minutes": 480, // family timezone offset in effect at server_time
  "cursor": 412,            // cursor to persist with this batch
  "has_more": false,
  "records": [ ... ],       // changed records, ordered by seq, at most `limit`
  "reminders": [ ... ],     // changed reminders in the same seq order
  "revoked": [ ... ]        // stable ids of records that must be deleted from the cache
}
```

- `cursor` is the highest `seq` included in the batch. `cursor=0` means "send everything".
- A record is delivered in the compact form used by the cache:
  `{"id","v","cat","sub","name","at","tz","tc","by","perf","amt","unit","dur","note","st","seq"}`,
  where `amt` is omitted when unknown rather than sent as `0`.
- The batch is a consistent snapshot: the server reads the log inside one transaction, so
  the cursor never skips a concurrent write.
- `utc_offset_minutes` is the offset of the family timezone at `server_time`. The passport
  has no timezone database, so it uses this offset for local time and for reminder
  instances, and refreshes it on every sync; the service therefore recomputes it instead of
  caching a fixed value.
- If `cursor` is greater than the newest `seq` (restored backup, replaced database), the
  service returns `409 cursor_invalid`; the device then requests the bounded latest snapshot below.
- Retention is permanent in the first generation, so a cursor cannot expire from pruning.
  The expired-cursor path exists for recovery, not for routine operation.

### 6.2 Full resynchronization

`GET /api/v1/sync/snapshot?latest=1&limit=40` returns the newest bounded window,
all reminders, member labels and one cursor from the same database transaction.
The device replaces its cache only after storing this entire response. Notes are
UTF-8 previews with enough extra bytes to preserve the device's truncation marker;
full notes and history remain available on the phone. The response is capped at
15 KB by removing oldest window entries when needed, so the 16 KB device buffer
has headroom. `window_count` reports the actual count, at most 40. `has_more` is
false because this is a completed recent window, not a full-history export.

The legacy `offset` mode remains available for compatibility. Its pages are
independent read views and must not be combined into a device snapshot while
writes are occurring. Use the new service and firmware together.

### 6.3 Device-side rules

- The device persists the new cache batch and the new cursor in a single NVS commit; the
  cursor advances only after that commit succeeds.
- A failed or interrupted sync keeps the previous cache and cursor, and the page shows the
  last successful sync time and the data age.
- Offline the device shows the cached records, cached reminders and their timestamps, and
  keeps the interaction and sound features available.
- Conflicts are resolved in favor of the service: the device never edits records.
- The target "saved on the phone is visible on the passport within 60 seconds" is a
  measurement goal to be verified on hardware, not a guarantee of the first generation.

## 7. Capacity, retention, backup and export

| Item | Decision |
| --- | --- |
| Device cache size | 40 newest records plus up to 32 revocation tombstones plus 16 reminders, stored as one canonical CRC32-protected binary blob under NVS namespace `lanlan`: one entry is at most 160 bytes, the blob at most 8 KB, and the live cache plus its staging buffer at most 16 KB of static RAM. The device keeps only a 48-byte UTF-8 preview of a note; the full note stays on the service |
| NVS partition | Unchanged: the tracked `partitions.csv` keeps 24 KB NVS. We do not repartition unless a measured need appears, and any such change must state its effect on application capacity and upgrade data |
| Cache eviction | Oldest records are dropped from the device cache only; the service history is never deleted by eviction |
| Damaged cache | The device discards and rebuilds an unreadable cache from the service and keeps a visible unsaved/unsynced warning; server data is never touched |
| Service retention | All revisions are kept indefinitely; no silent overwrite or deletion |
| Database mode | SQLite with WAL, `synchronous=FULL`, `busy_timeout=5000`, foreign keys enabled |
| Backup | `lanlan-admin backup --out <dir>` uses the SQLite online backup API, then verifies integrity and records a SHA-256 next to the file |
| Restore | `lanlan-admin restore --from <file>` refuses to overwrite a non-empty database without an explicit flag |
| Export | CSV and JSON exports contain every revision with creator, performer, status and timestamps; the JSON export is the lossless form |

## 8. Reminders

- Every reminder starts disabled with no schedule. The initialization creates disabled
  entries for feeding, water, bath, grooming, teeth, walking and combing. No care interval
  is preset, and the first generation does not invent one.
- The only supported schedule in the first generation is `daily` with a local time
  `HH:MM` interpreted in the family timezone. Enabling a reminder without a time is
  rejected with `422`.
- A reminder instance is identified by `(reminder_id, local_date)`. Ringing is allowed only
  when the instance has not rung before; the device persists the last rung instance per
  reminder, so refreshes, restarts and clock corrections cannot ring the same instance
  twice.
- Dismissing a prompt or reading a banner is not completion. A reminder counts as satisfied
  for display when a matching record for that category exists on that local date. The
  reminder page links to the record form, and the record still has to be submitted
  explicitly.
- Changing a reminder's time on the same day never causes a second ring for that instance.
  If the new time is still ahead, the instance can ring once at the new time.
- Reminder audio is off by default, can be enabled per reminder, and is always suppressed by
  the global mute setting. Muting never hides the due state visually.
- The device only rings when its clock is trusted. With an unverified clock it shows cached
  reminders with the last data update time and does not ring.
- Reminder completion rules and rescheduling behavior are covered by host tests using an
  injected clock.

## 9. Mobile web pages

| Page | Content and flow |
| --- | --- |
| Login | Username and password, error text, no data before authentication |
| Overview | Today's summary when the date is trustworthy, otherwise the most recent records; the last device sync time; a shortcut to the record form |
| Record | Category and sub-item selection, occurrence time, performer (defaults to the signed-in caregiver, can be the other one), optional amount with unit, optional duration, optional note, save feedback with retry |
| Records | Reverse-chronological list with date and category filters, keyset paging |
| Detail | Full record, revision history with creator and time, edit and revoke actions, visible conflict result on `409` |
| Reminders | Per-item enable switch, time picker, reminder sound switch, explicit note that dismissing is not completing |
| Profile | Pet name, birthday, breed, weight and free notes; no photograph upload in the first generation |
| Account and export | Change password, caregiver list, device list with revocation, CSV and JSON export, service status |

The layout targets a phone browser at 360 to 430 CSS pixels wide, uses large touch targets,
keeps the save button reachable, and shows the unsaved state when a submission fails.

## 10. Time handling

- All stored timestamps are RFC 3339 UTC; display uses the family timezone from the family
  row, so a timezone change in settings cannot duplicate or lose reminders.
- The service sets `server_time` in every sync response so the device can detect clock skew
  and show an unverified-clock warning instead of inventing dates.
- The device records nothing on its own; a record's `occurred_at` always comes from an
  explicit caregiver action or an explicit backfill.
- A date is treated as trustworthy on the web page when the service answers; on the device
  only after a successful SNTP synchronization. Without it, the device shows "most recent
  records" rather than a "today" summary.

## 11. Open items tracked after M0

| Item | State |
| --- | --- |
| Hosting provider and budget | Unconfirmed; local first, container configuration ready |
| Device provisioning channel | Design: a device credential is generated in the web settings page and pasted into the passport over its USB serial console. Wi-Fi credentials are entered the same way. Bluetooth provisioning remains a documented alternative if the serial path proves impractical |
| Character appearance | Placeholder pixel art, explicitly marked as not final art, until the owner approves the frozen sprite set |
| 60-second sync goal | Measurement target for hardware verification |
| Phone lock-screen push | Deferred by the task statement |

## Initial device-test release corrections

A repeated create request with the same key and the same normalized content replays
its record. Different content returns `409 idempotency_conflict` with the saved
record. The phone preserves edited fields and requires an explicit save as a
revision of that record. It never silently rotates the key to create a duplicate.
Datetime inputs use the browser's local timezone and are converted to UTC on save.
The firmware's healthy idle sync interval is 30 seconds; network failures still
back off. The 60-second visibility target needs measurement on the real device.
