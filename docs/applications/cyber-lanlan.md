<p align="right"><a href="cyber-lanlan.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan Passport Application (M0 design)

This document fixes the first-generation design of the Cyber Lanlan application on the
FoloToy AI Passport (ESP32-C3, 8 MB flash, no PSRAM, 240x320 SPI display, three buttons,
ES8311 audio, 2.4 GHz Wi-Fi). The cloud service, record model, authentication and sync
protocol are specified in
[cyber-lanlan-service.md](cyber-lanlan-service.md); verification and acceptance mapping are
in [cyber-lanlan-testing.md](cyber-lanlan-testing.md).

The application is a derived application and therefore designs its own screens, navigation
and visual language. It does not reuse the baseline hardware-test menu, the `demo_*.c`
pages, or the `ui_pixel` test shell. It reuses BSP APIs, ordinary LVGL widgets and the
lifecycle and concurrency patterns already present in the repository.

## 1. Pages and controls

| Page | UP / DOWN click | OK click | OK long press | Notes |
| --- | --- | --- | --- | --- |
| Home | Move the selection between the four entries | Enter the selected entry | Nothing | Shows today's summary when the clock is trusted, otherwise the most recent records; shows sync state and battery |
| Recent records | Previous / next record | Open the record detail | Back to home | Empty state when the cache has no record yet |
| Record detail | Previous / next record | Switch between the compact and the full view | Back to the list | Category, local time, performer, quantity or duration, note, revision marker |
| Companion | Switch between idle, blink and happy frames | Pet the character once (happy plus a short sound) | Back to home | A pet action never creates a care record |
| Settings | Move between rows | Toggle or open the row | Back to home | Refresh now, global mute, reminder sound, reminder list, timezone offset, sync information, storage state |
| Reminder list | Previous / next reminder | Nothing | Back to settings | Shows cached reminders, their configured time and their due state |
| Status / error | Nothing | Retry the sync | Back to home | Shown when the service is unreachable, the credential is rejected, or the cache was rebuilt |

Additional rules:

- The first gesture after the screen turns off only wakes the display. The long press that
  wakes the screen must not also deliver a short press when it is released.
- Network, storage and audio work never runs in the button callback; the callback only
  enqueues a `(key, event)` pair.
- A single application worker task owns the model, cache writes, PCM feeding and LVGL
  updates; every LVGL access from that task is wrapped in `bsp_lvgl_lock()` /
  `bsp_lvgl_unlock()`.
- The top bar shows the sync state and the battery percentage; when `bsp_battery_soc()`
  returns `-1` the battery reading is hidden instead of drawn as a number.

## 2. Module layout and testability

| File | Responsibility | Host-testable |
| --- | --- | --- |
| `main/main.c` | Initialization, task creation, key queue, backlight and idle handling | No (ESP-IDF) |
| `main/lanlan_model.c/.h` | Page and selection state machine, key handling, view strings selection | Yes |
| `main/lanlan_record.c/.h` | Record structure, category and sub-item tables, unit rules, compact formatting | Yes |
| `main/lanlan_cache.c/.h` | Bounded cache: encode, decode, merge a sync batch, tombstones, eviction, cursor commit | Yes |
| `main/lanlan_time.c/.h` | UTC seconds to local date and time using a UTC offset, clock-trust evaluation | Yes |
| `main/lanlan_reminder.c/.h` | Due evaluation and fired-instance suppression with an injected clock | Yes |
| `main/lanlan_sync.c/.h` | Wi-Fi STA, SNTP, HTTPS client, JSON parsing into the cache, retry and backoff | No (ESP-IDF) |
| `main/lanlan_ui.c/.h` | LVGL screens, fonts, sprite drawing, backlight-independent layout | No (LVGL) |
| `main/lanlan_assets.c/.h` | Generated sprite descriptors and PCM clip table | No (data only) |

Every module marked host-testable contains no ESP-IDF or LVGL include and is covered by a
C test under `tests/` that is registered in `tools/validate.sh`.

## 3. On-device cache and NVS layout

NVS namespace `lanlan` (the partition stays at the tracked 24 KB; no repartitioning):

| Key | Content | Bound |
| --- | --- | --- |
| `cfg_v1` | Schema version, UTC offset in minutes, global mute, reminder sound, dim and screen-off timeouts, service base URL | 128 B |
| `cache_v1` | Compact record cache: header, fixed-size entries and a CRC32 trailer | at most 8 KB |
| `tomb_v1` | Revocation tombstones (16-byte ids plus version) | about 512 B |
| `rem_v1` | Cached reminders plus the last rung instance per reminder | about 1 KB |
| `cred_v1` | Device credential (token plus device id) and the Wi-Fi SSID and password | about 256 B |

Cache rules:

- The cache holds the 40 newest records by `occurred_at`; older records are evicted from the
  device only. Service history is never deleted.
- A record is a fixed-size binary entry; `amount` is stored with an "unknown" flag so an
  unset quantity can never be displayed as `0`.
- The device entry keeps a bounded 48-byte UTF-8 preview of the note (the largest value
  that still satisfies the entry and blob budgets below) and a 24-byte custom name. The
  full note stays on the service and the phone, and the device renders a shortened note
  with an explicit truncation marker instead of pretending it is complete.
- Enforced size budgets, asserted by host tests: one entry is at most 160 bytes, the whole
  cache blob at most 8 KB, and the live cache plus its staging buffer at most 16 KB of
  static RAM. The blob is one canonical fixed-offset serialization covered by a CRC32
  trailer, written with a single NVS commit together with the new cursor. The 24 KB NVS
  partition is sufficient and is not repartitioned.
- A sync batch is applied into a temporary buffer, then the whole cache, the tombstones, the
  reminders and the new cursor are written in **one** NVS commit. The cursor advances only
  after that commit returns `ESP_OK`.
- An unreadable or version-mismatched cache is discarded and rebuilt from the service; the
  device shows a "cache rebuilt" notice and never erases unrelated NVS namespaces or any
  server data.
- Revoked records are removed from the cache and their ids are kept in the tombstone ring,
  so a later partial sync cannot resurrect them.

## 4. Sync behaviour

- The worker connects to Wi-Fi, synchronizes SNTP, then calls
  `GET /sync/changes?cursor=<n>` with the stored device token.
- Sync runs on application start, on manual refresh from Settings, and on a periodic timer;
  failures use exponential backoff and never block the UI or the audio path.
- `server_time` and `utc_offset_minutes` from the response set the local clock offset and
  the "clock trusted" flag. Without a successful SNTP or service response the clock is
  untrusted: the home page shows the most recent records instead of a today summary, and no
  reminder rings.
- The Settings page shows the last successful sync time, the number of cached records and
  the data age. Offline, the pages continue to show cached records, cached reminders and
  their timestamps, and the companion and sound features keep working.
- An HTTP `401` marks the credential as rejected and shows a status page with the
  provisioning instructions; the device does not retry with a broken credential.
- The passport never creates, edits or revokes records. There is no record input on the
  device in the first generation.

## 5. Reminders

- Reminders come only from the service; every reminder starts disabled and no interval is
  preset. The device renders the list and the due state, and cannot configure schedules.
- An instance is `(reminder_id, local_date)`. The device persists the last rung instance per
  reminder, so a refresh, a restart or a clock correction cannot ring the same instance
  twice.
- Reminder audio is off by default, can be enabled for all reminders in Settings, is always
  suppressed by the global mute, and never rings while the clock is untrusted.
- Dismissing or leaving the reminder page does not mark anything complete and does not
  create a record; the reminder page states this explicitly.

## 6. Companion animation and sound

- States: idle (looping two-frame breathing), blink (occasional one-frame overlay),
  happy (short three-frame reaction to OK on the Companion page) and bark (a frame plus a
  short clip).
- Sprites are stored as 16-bit frames in flash and copied frame by frame into a single
  canvas buffer; the whole animation set is never loaded at once.
- The first-generation art is explicitly marked as placeholder art in the repository and in
  the delivery report. The owner has not approved the final appearance; only the placeholder
  is used to validate layout, animation timing and rendering.
- Sound clips are short 16 kHz mono PCM assets derived from licensed or self-produced
  material with a recorded source and licence; no asset from the Korean learning prototype
  is reused.

## 7. Fonts, glyphs and assets

- The UI uses generated, uncompressed 2-bpp subsets of Noto Sans CJK SC at 16 px and 24 px,
  produced with a pinned `lv_font_conv` and an explicit code-point inventory derived from the
  application's fixed strings and category names.
- The English default document and the repository record the exact generation command, the
  source font and the OFL licence. Coverage is checked with `lv_font_get_glyph_dsc()` plus
  the placeholder flag, and a test fails when a new code point appears without regenerating
  the fonts.
- Free caregiver notes come from the service and can contain characters outside the fixed
  inventory; they are length-limited (200 characters) and rendered with the fallback
  placeholder strategy rather than promising full Unicode coverage.
- Assets stay under `assets/` (`assets/fonts/`, `assets/images/`, `assets/music/`) with their
  source, licence and generation command recorded in `assets/README.md`; generated `.c`
  files are wired through `main/CMakeLists.txt`.

## 8. Provisioning

The device ships with no Wi-Fi credentials. First-time setup uses the USB serial console
(USB-Serial-JTAG, the same port used for logs):

```text
lanlan cfg ssid <name>
lanlan cfg pass <password>
lanlan cfg url <https://host>
lanlan cfg token <device token>
lanlan cfg tz +08:00
lanlan cfg show
lanlan sync now
```

The web settings page creates the device credential and shows the token once for the owner
to paste into the console; the token is stored in NVS and never printed again. Serial setup
avoids allocating a Bluetooth stack and keeps RAM for TLS, UI and audio. Bluetooth
provisioning stays a documented alternative if this proves impractical.

## 9. Configuration and build

- `sdkconfig.defaults` on this branch enables Wi-Fi and keeps Bluetooth disabled, keeps
  `CONFIG_LV_TXT_ENC_UTF8` and `CONFIG_LV_USE_FONT_PLACEHOLDER`, and adds the certificates
  needed for the HTTPS client.
- `main/CMakeLists.txt` requires `bsp`, `nvs_flash`, `esp_timer`, `esp_wifi`, `esp_netif`,
  `esp_http_client`, `esp-tls`, `mbedtls`, `json` and `lwip`, and links the generated fonts,
  sprites and PCM pack.
- Partitions stay as tracked: NVS, PHY data and one factory application. The Korean
  application sources and Korean-only assets are removed from this branch rather than left
  referenced; the preserved `feature/korean-learning` branch keeps that application.

## 10. Resource budget and limits

- No PSRAM: the animation canvas uses one buffer (a 96x96 RGB565 canvas is about 18 KB), the
  JSON parsing buffer is bounded (about 8 KB), and TLS plus Wi-Fi are the largest consumers.
- The application logs free heap and the largest free internal block at start-up and after
  each sync, so the combined Wi-Fi, TLS, UI and audio peak can be measured on hardware.
- The idle behaviour dims the backlight after 30 seconds and turns the display off after 90
  seconds; both are configurable. This is not deep sleep, and no battery-life claim is made
  without measurement.

## 11. Hardware checks pending

Real rendering, sound quality, Wi-Fi and TLS stability, the combined RAM peak, reminder
timing and the 60-second sync goal are device checks. They are recorded as `Device tests:
NOT RUN` with an explicit list until hardware is available.
