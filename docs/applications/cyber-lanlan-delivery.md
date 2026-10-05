<p align="right"><a href="cyber-lanlan-delivery.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan initial device-test release

> This page records v0.1. For the current visual update, see [character v0.2](cyber-lanlan-character.md).

This 2026-10-05 repair release supersedes the previous DeepSeek handoff. It is
based on `989f746e6ff03fd660839b94e7e81a8d99db18a4` with local, uncommitted fixes.
No board has been flashed or accepted. The source, service and firmware must be
used together. Xiaomi integration remains deferred and reminders start disabled.

## Repairs and scope

- Browser-local dates round-trip through UTC; an unknown amount no longer inherits
  a previously edited record's quantity.
- A lost-response retry with changed content produces an explicit conflict, keeps
  the entered fields and lets the caregiver save a revision of the existing record.
- Snapshot recovery reads the recent window, reminders and cursor together. The
  window holds up to 40 records with bounded note previews and a 15 KB response
  budget; full records remain on the server. See the [protocol](cyber-lanlan-service.md).
- Healthy idle sync runs every 30 seconds. Failure backoff remains enabled.
- Reminder dates use a persisted high-water mark. Clock rollback cannot re-arm an
  older date; sound is played only after persistence succeeds. A clock mistakenly
  advanced far into the future suppresses reminders until that date is passed.
- Firmware and UI stress tests both use a 32 KB LVGL pool. This adds 8 KB to the
  prior firmware pool, while replacing the test's previous 48 KB assumption.

Phone recording, separate Hehe/Yangyang accounts, cached device reading, companion
interaction, sound and mute are included. Character sprites are still placeholder
art, pending owner approval. No cloud host has been deployed.

## Validation and exact firmware

| Check | Result |
| --- | --- |
| Build | PASS: ESP-IDF 5.5.3, isolated tracked defaults, merged layout and matching debug archive verified |
| Host tests | PARTIAL: firmware logic, assets, JavaScript time/form/retry regressions and socket-free SQLite/API regressions pass; the complete gate is blocked by denied local HTTP socket binding |
| Device tests | NOT RUN |
| Unverified | Real Wi-Fi/TLS peak memory, screen, keys, audio, NVS persistence, battery and phone-to-device timing; real HTTP suite and production deployment |

The full gate was attempted. HTTP tests fail during server setup with
`PermissionError: [Errno 1] Operation not permitted`; this is not a passing gate.
Run it in a normal terminal with ESP-IDF 5.5.3 and Node.js available:

```bash
./tools/validate.sh
python3 tools/preview_lanlan.py --mode stress
```

Host UI stress passed 1,103 page switches, 2,160 keys and 2,880 renders. Free LVGL
memory returned to 13,432 bytes; minimum observed free memory was 10,328 bytes.
This host measurement does not establish ESP32 Wi-Fi/TLS heap headroom.

- Merged image: 1,831,568 bytes, flash at `0x0`.
- Full SHA-256: `2fc9a71d82f6ca43961d0627d70d66a4c4d09ec3f643c0a5632100ad3b4df90e`.
- Matching ELF SHA-256: `14a07985330bf04deb99fdbd36e4264fb44b93619d2ecc1bf388055c3b874fdf`.
- Embedded version: `989f746-dirty`; use the hashes to identify this build.
- Debug archive: `build/firmware/2fc9a71d82f6ca43961d0627d70d66a4c4d09ec3f643c0a5632100ad3b4df90e/`.

Merged flashing can reset NVS settings and cached data. It replaces the installed
application. Do not use an application-only image at `0x0` or perform a routine
full-chip erase. Flashing needs separate owner authorization.

## Local phone and device testing

Use a temporary computer-hosted server for the test session. The phone, computer
and Passport must share a reachable network; the Passport needs 2.4 GHz Wi-Fi.
Run from the repository root with Python 3.9 or newer:

```bash
export PYTHONPATH="$PWD/services"
export LANLAN_DB="$PWD/.local-data/lanlan.sqlite3"
mkdir -p .local-data
python3 -m lanlan init
python3 -m lanlan serve --host 0.0.0.0 --port 8787
```

Initialize only a new database. Save the generated caregiver passwords shown once
by `init`. Open `http://<computer-LAN-IP>:8787/` on the phone. `127.0.0.1` on the
phone points to the phone itself. Allow local-network access if the OS firewall
asks. Local HTTP is for a trusted test network only. The terminal must stay open;
when the computer stops serving, the device keeps its cache but receives no updates.
For daily independent use, deploy the service to an always-available host later.

Create a device token on the web account page, then enter these commands in the
Passport USB serial console at 115200 baud, substituting real values without
angle brackets. Do not put credentials into screenshots or Git:

```text
lanlan cfg ssid <Wi-Fi-name>
lanlan cfg pass <Wi-Fi-password>
lanlan cfg url http://<computer-LAN-IP>:8787
lanlan cfg token <device-token>
lanlan cfg tz +08:00
lanlan cfg show
lanlan sync now
```

Use Up/Down to select, OK to enter, and long OK to return. The first gesture after
screen-off only wakes the display. Check a phone-created record on the Passport,
edit its note/time, switch caregiver, test an unknown quantity, disconnect/reconnect
Wi-Fi, restart, then test companion sound and mute. Leave care reminders disabled
unless deliberately testing a schedule.

## Optional later hosting

Configure a real domain and an always-on host before running:

```bash
cp deploy/lanlan.env.example deploy/lanlan.env
# Edit the domain, then:
docker compose --env-file deploy/lanlan.env -f deploy/docker-compose.yml up -d --build
```

Follow [deployment instructions](../../deploy/README.md) for database initialization,
backup and HTTPS. No paid resource or external publication was created by this repair.
