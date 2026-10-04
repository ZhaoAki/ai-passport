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
| A01 | A phone page can record every first-generation item, usable by both caregivers | `tests/test_web_flow.py` drives the record form for every category and unit over the HTTP API; the mobile layout is inspected at 390x844 in a browser | Automated part runs; visual phone check pending |
| A02 | Creator, performer and revision are distinguishable; unknown quantity never becomes zero | `tests/test_records_api.py` (creator versus performer, revision chain, `amount_value` stays `NULL`), `tests/test_lanlan_record.c` for the device rendering path | Automated |
| A03 | Conflicts are visible, retries do not duplicate, revocation does not reappear | `tests/test_concurrency.py` (409 with current version, `client_request_id` replay, tombstone delivery after revoke) plus `tests/test_lanlan_cache.c` for tombstone handling | Automated |
| A04 | Unauthenticated and foreign-family identities cannot read records; device credentials are limited | `tests/test_authz.py` (401 for anonymous, 403/404 for another family, device token rejected on every write endpoint and on account endpoints) | Automated |
| A05 | Saved data survives a restart; a failed write keeps the caregiver's input | `tests/test_persistence.py` restarts the service process against the same database file; the web test asserts the form keeps its values and the retry reuses the same request id | Automated |
| A06 | Sync meets the agreed latency; offline shows cache and data age | `tests/test_sync_protocol.py` for cursor ordering and batch consistency; latency and offline behaviour require hardware | Protocol automated; device check pending |
| A07 | A damaged cache can be rebuilt, paging is bounded, server records are untouched | `tests/test_lanlan_cache.c` (corrupt blob, oversized batch, eviction, cursor not advanced on failure) | Automated |
| A08 | Companion interaction creates no real record and no Korean entry remains | `tests/test_lanlan_model.c` (interaction never emits a record action); repository check confirms Korean application sources, tooling, tests and assets are removed from this branch | Automated |
| A09 | Chinese and dynamic text follow the agreed strategy without truncated key fields | `tests/test_lanlan_fonts.py` (glyph inventory matches the fixed strings, no placeholder for required glyphs), `tests/lanlan_ui/` host preview renders the fixed screens at 240x320 | Automated inventory; real-panel rendering pending |
| A10 | Reminders start disabled, are owner-configurable, and never ring twice across refresh, restart or clock change | `tests/test_lanlan_reminder.c` with an injected clock and persisted fired instances; `tests/test_reminders_api.py` for default-disabled state and enable/disable rules | Automated |
| A11 | Three-button short and long presses, mute and screen-off wake behave consistently with no false submission | `tests/test_lanlan_model.c` key sequences including wake-then-release, plus the existing BSP button tests | Automated logic; real key feel pending |
| A12 | Network, audio and animation run together without leaks | Host-side lifetime tests only; on hardware, page switching and event loops with heap and largest-block logging | Device measurement pending |
| A13 | Export matches effective records, backup restores, no silent history overwrite | `tests/test_export_backup.py` (CSV and JSON against the database, backup plus restore round trip, revision history preserved) | Automated |
| A14 | The full build passes and firmware, ELF, MAP and partition files correspond to one commit | `./tools/validate.sh` and `python3 tools/archive_firmware.py verify <bundle>` | Firmware gate to run in M4 |
| A15 | No credentials, family records or unauthorized photographs in the source; asset sources recorded | `tools/check_repo.py` secret scan, plus a manual review of `assets/README.md` and the commit contents | Automated scan plus review |

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
