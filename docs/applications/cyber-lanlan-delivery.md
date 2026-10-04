<p align="right"><a href="cyber-lanlan-delivery.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan First-Generation Delivery

This document is the handover record for the first generation of the Cyber Lanlan
pet-care recorder: what was delivered, how to run it, what was verified, and what is
still open. The frozen design is in [cyber-lanlan.md](cyber-lanlan.md) and
[cyber-lanlan-service.md](cyber-lanlan-service.md); the acceptance mapping is in
[cyber-lanlan-testing.md](cyber-lanlan-testing.md).

## 1. Delivered state

| Item | Value |
| --- | --- |
| Repository | https://github.com/ZhaoAki/ai-passport |
| Branch | `feature/cyber-lanlan` |
| Baseline | `99058004449a76c313375e238436b4642e36886c` (`feature/korean-learning`) |
| Preserved branch | `feature/korean-learning` is untouched; this branch retires the Korean application from its own tree only |
| Firmware | merged image `build/FoloToy-AI-Passport-full.bin`, application image and matching ELF/MAP retained in the content-addressed bundle under `build/firmware/<sha256>/` |
| Service | `services/lanlan/`, CPython standard library only, SQLite storage |
| Mobile web | `web/lanlan/`, no build step, no third-party assets |
| Deployment | `deploy/` (`docker-compose.yml`, `Caddyfile`, environment example) |

Stage coverage: M0 design documents, M1 service and mobile web, M2 passport sync with a
bounded cache, M3 companion interaction, sounds and owner-configured reminders (all
disabled by default), M4 build, artifacts and this handover.

## 2. What the first generation does

- Two caregivers record feeding, water, care (bath, grooming, teeth, combing), cleaning,
  walking and other items from a mobile web page, with an explicit occurrence time, a
  performer that can be the other caregiver, an optional amount with a unit, an optional
  walk duration and an optional note. An unset amount stays unknown and is never stored or
  displayed as zero.
- The service is the authority: append-only revisions with a creator, a performer, a
  version, a revocation tombstone, idempotent submission, visible edit conflicts, CSV and
  JSON export, online backup and restore.
- The passport keeps a bounded recent cache (40 records, 32 tombstones, 16 reminders in one
  CRC32-protected blob), syncs incrementally with a single global cursor, shows the sync
  state, the battery, the last successful sync time and the data age, works offline from the
  cache, and offers a second, redesigned UI with a pixel companion, short sounds, a global
  mute and a reminder list.
- Reminders start disabled with no interval preset; the owner enables a daily time in the
  web page. An instance can ring at most once, and dismissing a prompt is not completion.
- The passport cannot create, edit or revoke records, and virtual interaction never
  produces a care record.

Out of scope and not built: MI Home or appliance integration, camera notifications or
video, remote feeding, phone lock-screen push, Korean learning content, AI chat, and any
hunger, death or absence penalty.

## 3. How to run it

### 3.1 Service and mobile web (development)

```bash
cd services
python3 -m lanlan init --password hehe=<password> --password yangyang=<password>   # creates the family and 7 disabled reminders
python3 -m lanlan serve --host 127.0.0.1 --port 8787
```

Open `http://127.0.0.1:8787/` on the phone (same network) and sign in as `hehe` or
`yangyang`. Configuration is environment-based: `LANLAN_DB`, `LANLAN_HOST`, `LANLAN_PORT`,
`LANLAN_ENV`, `LANLAN_SECURE_COOKIES`, `LANLAN_TIMEZONE`, `LANLAN_PBKDF2_ITERATIONS`,
`LANLAN_SESSION_DAYS`. Details are in [services/lanlan/README.md](../../services/lanlan/README.md).

> The development server is not an always-on deployment: the phone can only reach it while
> the machine running it is on. A hosted deployment needs the budget decision that is still
> open.

### 3.2 Deployment

```bash
cp deploy/lanlan.env.example deploy/lanlan.env     # set the public domain
docker compose -f deploy/docker-compose.yml up -d  # service + Caddy with automatic HTTPS
```

See [deploy/README.md](../../deploy/README.md). HTTPS is required in production; the
service refuses a production configuration without an explicit database path and secure
cookies.

### 3.3 Passport firmware

```bash
source <path-to-esp-idf-5.5.3>/export.sh
./tools/validate.sh --firmware          # builds and verifies the merged 0x0 image
idf.py -p <port> flash monitor          # optional incremental development flashing
```

Without hardware, the merged image is the deliverable. Flashing is a separate action that
needs explicit approval; on a blank device flash `build/FoloToy-AI-Passport-full.bin` at
offset `0x0`.

### 3.4 First-time passport setup

The device ships without Wi-Fi credentials and without a device credential. Create the
credential in the web page (Account and export, "device credential") and paste these into
the passport over its USB serial console (the same port as the logs):

```text
lanlan cfg ssid <wifi name>
lanlan cfg pass <wifi password>
lanlan cfg url <https://host>
lanlan cfg token <device token>
lanlan cfg tz +08:00
lanlan cfg show
lanlan sync now
```

`url` must be `http(s)://` with no trailing slash; plain `http://` is accepted for a LAN
development service and is marked as insecure on the status page. `cfg show` never prints
the password or the token in full. The owner-facing control map is in
[cyber-lanlan.md](cyber-lanlan.md#1-pages-and-controls).

## 4. How to verify

```bash
./tools/validate.sh --static     # repository checks, firmware host tests, the service test suite
./tools/validate.sh --firmware   # ESP-IDF build, merged image, layout check, debug archive
./tools/validate.sh              # complete gate
python3 tools/preview_lanlan.py  # render every screen at 240x320 with host LVGL plus a glyph audit
python3 tools/archive_firmware.py verify build/firmware/<sha256>
```

The service suite and the firmware host tests are registered in `tools/validate.sh`, so the
same checks run locally and in CI.

## 5. Verification results for this delivery

```text
Build:        PASS
Host tests:   PASS
Device tests: NOT RUN
Unverified:   on-glass rendering, Wi-Fi association and TLS behaviour, real key feel and
              backlight timing, audio playback, reminder ringing across a real day boundary,
              the 60-second save-to-passport goal, the measured RAM peak with Wi-Fi and TLS
              active, cache rebuild on a real NVS partition, battery behaviour, and the
              owner's decision on the final character art
```

Exact commands, test counts, image sizes and hashes are recorded in the commit messages and
in the sections above; the acceptance-by-acceptance view is in
[cyber-lanlan-testing.md](cyber-lanlan-testing.md).

## 6. Known limitations and deviations

| Item | State |
| --- | --- |
| Character art | The sprite set is placeholder art, marked as such in the generated source and in [assets/README.md](../../assets/README.md). The pixel grain and the final appearance still need the owner's approval before they are frozen. |
| Note preview | The passport keeps a 48-byte UTF-8 preview of a note and renders an explicit truncation marker; the full note stays on the service and the phone. |
| Caregiver labels | The passport learns the names from the service's sync payload; if a name cannot be resolved it shows the neutral fallback label from the string table instead of a raw id or a slot number. |
| Key hints | The bottom hint line uses the ASCII form `UP/DN OK HOLD=BACK`; the frozen string table has no entry for a generic hint. |
| Display timeouts | Dimming (30 s) and screen-off (90 s) are configurable through the configuration blob and the console, not from a settings row. |
| Reminder schedule | Daily repeat at one local time is the only supported schedule in this generation. |
| Photos | The original family photographs are neither uploaded nor committed; the profile page has text fields only. |
| Hosting | Local service plus a ready container configuration; no paid resource was created and no hosting decision was made. |
| Kernel of truth on latency | The "saved on the phone, visible on the passport within 60 seconds" goal is a measurement target, not a guarantee. |

## 7. Open decisions for the owner

1. Hosting: whether to run the service on a small VPS or container host, and the domain name.
2. Character art: approve or replace the placeholder sprite set, then freeze the assets and
   regenerate the fonts/sounds if the palette changes.
3. Device testing: connect the passport over USB so the device checklist can be executed and
   `Device tests` can move from `NOT RUN` to a real result.
4. Reminder policy: which items to enable, at which local times, and whether reminder sound
   should be on.
5. Whether the upstream repository should receive a pull request from this branch, or the
   fork stays the delivery target.
