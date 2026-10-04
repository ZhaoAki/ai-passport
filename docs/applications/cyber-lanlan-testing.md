<p align="right"><a href="cyber-lanlan-testing.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan Verification Plan (M0)

This document maps the first-generation acceptance criteria to concrete, reproducible checks
and states which parts can be executed in this environment. The design it verifies is in
[cyber-lanlan-service.md](cyber-lanlan-service.md) and [cyber-lanlan.md](cyber-lanlan.md).

Delivery reports always separate four fields:

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or user checks
```

No device was available during development, so every on-hardware item is reported as
`Device tests: NOT RUN` with the pending check named explicitly; a successful build is never
presented as hardware validation.

## 1. Acceptance mapping

| ID | Check | Evidence in this repository | Status now |
| --- | --- | --- | --- |
| A01 | A phone page can record every first-generation item, usable by both caregivers | `services/lanlan/tests/test_records_api.py` and `test_web_assets.py` drive every category, unit and web view over real HTTP; the mobile layout still needs a browser check at 390x844 | Automated over the API; visual phone check pending |
| A02 | Creator, performer and revision are distinguishable; unknown quantity never becomes zero | `services/lanlan/tests/test_records_api.py` (creator versus performer, revision chain, `amount_value` stays `NULL`), `tests/test_lanlan_record.c` and `tests/test_lanlan_caregiver.c` for the device rendering path | Automated |
| A03 | Conflicts are visible, retries do not duplicate, revocation does not reappear | `services/lanlan/tests/test_concurrency.py` and `test_change_sequence.py` (409 with the current version, `client_request_id` replay, tombstones delivered to the cursor) plus `tests/test_lanlan_cache.c` for tombstone handling | Automated |
| A04 | Unauthenticated and foreign-family identities cannot read records; device credentials are limited | `services/lanlan/tests/test_authz.py` and `test_config_auth_security.py` (401 for anonymous, 403/404 for another family, device token rejected on every write and account endpoint, CSRF and origin checks) | Automated |
| A05 | Saved data survives a restart; a failed write keeps the caregiver's input | `services/lanlan/tests/test_persistence.py` restarts the service against the same database file; `test_web_assets.py` asserts the form keeps its values and reuses the same request id on retry | Automated |
| A06 | Sync meets the agreed latency; offline shows cache and data age | `services/lanlan/tests/test_sync_protocol.py` and `test_change_sequence.py` for cursor ordering, paging completeness and batch consistency; the 60-second goal and offline behaviour require hardware | Protocol automated; device check pending |
| A07 | A damaged cache can be rebuilt, paging is bounded, server records are untouched | `tests/test_lanlan_cache.c` (corrupt, truncated and version-mismatched blobs, batch staging failure, eviction, cursor not advanced on failure) | Automated |
| A08 | Companion interaction creates no real record and no Korean entry remains | `tests/test_lanlan_model.c` (interaction never emits a record action); this branch removes the Korean application sources, tests, tooling and assets, and `main/CMakeLists.txt` links none | Automated |
| A09 | Chinese and dynamic text follow the agreed strategy without truncated key fields | `tests/test_lanlan_assets.py` (font inventory matches the fixed strings) and the `tests/lanlan_ui/` host render harness, which renders every 240x320 screen with the real `lanlan_ui.c` and audits glyph coverage | Automated host rendering; real-panel rendering pending |
| A10 | Reminders start disabled, are owner-configurable, and never ring twice across refresh, restart or clock change | `tests/test_lanlan_reminder.c` with an injected clock and persisted rung instances; `services/lanlan/tests/test_reminders_api.py` for the default-disabled state and enable/disable rules | Automated |
| A11 | Three-button short and long presses, mute and screen-off wake behave consistently with no false submission | `tests/test_lanlan_model.c` key sequences including wake-then-release, plus the existing BSP button tests | Automated logic; real key feel pending |
| A12 | Network, audio and animation run together without leaks | `tools/preview_lanlan.py --mode stress` drives over 500 page switches and 1000 key events through the real UI and model with LVGL, and asserts that the free pool, the largest free block and the live-object count return exactly to their pre-run baseline; the application also logs free heap and the largest internal block at boot and after every sync for the on-hardware half | Host half automated; network, audio and 2-hour device measurement pending |
| A13 | Export matches effective records, backup restores, no silent history overwrite | `services/lanlan/tests/test_export_backup.py` (CSV and JSON against the database, backup plus restore round trip, revision history preserved) | Automated |
| A14 | The full build passes and firmware, ELF, MAP and partition files correspond to one commit | `./tools/validate.sh` plus `python3 tools/archive_firmware.py verify <bundle>` on the content-addressed archive | Firmware gate passes; repeated in M4 |
| A15 | No credentials, family records or unauthorized photographs in the source; asset sources recorded | `tools/check_repo.py` secret scan, the `.gitignore` rules for databases and deployment secrets, and a manual review of `assets/README.md` and the commit contents | Automated scan plus review |

## 2. How to run the checks

Service, web and protocol tests (no third-party packages required):

```bash
python3 -m unittest discover -s services/lanlan/tests -t . -v
PYTHONPATH=services python3 -m lanlan check-config
```

Firmware host logic tests and repository checks:

```bash
./tools/validate.sh --static
```

Complete gate with an activated ESP-IDF 5.5.3 environment:

```bash
./tools/validate.sh
```

`tools/validate.sh` stays the single entry point: the new C and Python tests are registered
in it rather than in a second pipeline, so local and CI behaviour cannot drift apart.

## 3. Fault injection covered by automated tests

| Scenario | Injection |
| --- | --- |
| Duplicate submission | The same `client_request_id` is posted twice, including concurrently |
| Concurrent edit | Two caregivers patch the same record with the same `expected_version`; exactly one wins and the other receives the current record |
| Interrupted sync | The batch is applied but the cursor commit is simulated as failing; the cache must keep the previous cursor and content |
| Corrupt cache | A truncated and a version-mismatched blob are decoded; the cache must rebuild without touching other NVS keys |
| Clock change | The injected clock jumps forward and backward across a reminder instance; no instance rings twice |
| Database restart | The service process is stopped and restarted against the same file; every saved record is still readable |
| Revoked record | A revoked id is delivered again in a later batch; it must not reappear in lists or on the device |

## 4. Not covered by automation

- Real screen rendering, colour, readability at 240x320 and pixel-art scaling.
- Wi-Fi association, TLS handshake time, radio stability and the combined RAM peak with
  Wi-Fi, TLS, UI and audio active.
- Actual key feel, backlight behaviour and the wake-then-release edge on hardware.
- Reminder audio and the 60-second save-to-passport latency goal.
- Battery behaviour and any dimming or screen-off timing under real use.
- Photograph and artwork approval by the owner.
