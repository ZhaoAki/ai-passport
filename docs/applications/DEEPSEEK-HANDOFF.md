<p align="right"><a href="DEEPSEEK-HANDOFF.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cyber Lanlan continuation handoff

Dated 2026-10-05. Current release: v0.3. Code commit: `8e1717c`, on
`feature/cyber-lanlan`; target: `https://github.com/ZhaoAki/ai-passport`.
The following documentation commit does not change the compiled firmware.
GitHub DNS is unavailable in the current environment. Local commits are complete;
remote publication is not confirmed. Check the remote before claiming it is pushed.

## User contract

Lanlan is a real Maltipoo born in 2026. Hehe (the owner) and Yangyang are separate
caregivers. The phone web app records meals, water, bathing, grooming, brushing,
teeth cleaning, walks and household cleaning. The server is authoritative; the
Passport reads a bounded synchronized cache. Virtual interaction never creates a
real care record, and unknown quantities are not zero.

Keep this application independent of the Korean prototype. No Korean content or
AI conversation. Xiaomi purifier, visual feeder 2, sterilizing water fountain 2
and C700 integrations are deferred. Reminders start disabled and are configured
by the owner later. There is no always-on home computer. Local serving is only a
test arrangement; no cloud host, budget, domain or paid deployment is settled.

## Resume safely

Read `AGENTS.md` and `docs/development/ai-guide.md`, inspect the branch and dirty
files, and preserve all concurrent work. Ensure commit `8e1717c` is present; do
not resume from the old `989f746` delivery alone. If publication is unavailable,
use the supplied incremental Git bundle or the complete v0.3 source package.
The bundle requires the existing `989f746` history:

```bash
git status --short --branch
git bundle verify /path/to/cyber-lanlan-v0.3-handoff.bundle
git fetch /path/to/cyber-lanlan-v0.3-handoff.bundle feature/cyber-lanlan:refs/heads/import/cyber-lanlan-v03
```

Only with a clean compatible working branch, fast-forward using
`git merge --ff-only import/cyber-lanlan-v03`. Review and normally merge divergent
work instead of resetting or force pushing. Finish v0.3 acceptance before adding
unrequested integrations. Each new firmware flash needs version-specific owner
authorization; prefer verified compatible application-only updates to preserve NVS.

## Completed implementation

v0.1 fixes browser-local to UTC conversion, stale unknown quantities, and silent
loss of modified idempotent retries. Conflicting content now returns
`409 idempotency_conflict` with the saved record; the web form keeps edits and lets
the caregiver explicitly save a revision. Snapshot recovery uses one consistent
`latest=1&limit=40` read of records, reminders and cursor. UTF-8 previews and a
15 KB response limit fit the 16 KB device buffer; an extreme payload may reduce
the recent window without deleting server history. Reminder rollback uses a
persisted date high-water mark and saves before sounding. Healthy idle sync is
30 seconds, with the 60-second visibility target still unmeasured. Both firmware
and host UI use a 32 KB LVGL pool instead of mismatched 24/48 KB budgets.

v0.2 replaces geometric placeholders with imagegen art based on the prior Lanlan
character and owner GIF motion references, on the Passport and web overview/profile.
Six 96 × 96 RGB565 frames occupy 110,592 Flash bytes and one 18,432-byte RAM canvas.
The unchanged generated atlas, prompt and conversion hashes live in
`assets/images/lanlan-v2/`. Original family photos and reference GIFs are excluded.
Do not regenerate the old geometric placeholder over this artwork.

v0.3 adds an allocation-free four-reaction shuffle bag: blink, tilt, raised paws
and open-mouth bark. Every round covers all four and round boundaries avoid an
adjacent repeat. Each physical OK press reacts immediately; its subsequent
single/double classification is consumed. A long hold returns after its initial
reaction; the first gesture after screen-off only wakes. Three original synthetic
puppy sounds replace the interaction chirp, not recordings of a real dog.
`chirp` remains the historical name for two arfs, alongside `bark` and `bark_soft`;
the reminder chime is unchanged. The PCM pack is 47,678 bytes. Global mute still
works; rapid presses replace the current clip rather than queuing it.

## Entry points

- `main/lanlan_reaction.c/.h`: pure reaction selection; `main/main.c`: button/audio
  dispatch, tasks and storage; `main/lanlan_ui.c`: frames, pages and timers.
- `main/lanlan_sync.c`: device Wi-Fi/SNTP/sync; `services/lanlan/api.py`: API,
  snapshots and idempotency; `web/lanlan/`: build-free mobile UI.
- `tools/import_lanlan_sprites.py`: mechanical atlas conversion, Pillow required.
  `tools/generate_lanlan_assets.py`: fixed assets and original audio synthesis.
- `tools/start-lanlan-local.command`: Mac serving; application documents
  `cyber-lanlan-interaction`, `cyber-lanlan-character`, and `cyber-lanlan-delivery`
  under `docs/applications/` cover v0.3, v0.2 and historical v0.1 respectively.

## Firmware identity and evidence

Merged image: 1,864,688 bytes at `0x0`. Application: 1,799,152 bytes at `0x10000`.
Merged flashing may reset NVS. Only a verified compatible application-only update
preserves that region; no routine whole-chip erase.

- Full SHA-256: `dc9dd26980edd79fa52c08057aeb8af4f039bb317ff46379a621616e75d276bf`.
- ELF SHA-256: `d90a48aa117740fbe19bff081602c45c41394de994feea1d44f75c3c285ebfdf`.
- Embedded version: `989f746-dirty`, built before committing the source now in
  `8e1717c`; hashes identify the exact image.
- Archive: `build/firmware/<full-image-hash>/`; the release also retains its
  matching `debug/` bundle and `release.json`. Never substitute a different ELF.

Build: PASS, ESP-IDF 5.5.3 with isolated tracked defaults, merged/layout/archive
verification. Host tests: PARTIAL, firmware logic, assets, JS regressions,
socket-free SQLite/API checks and remaining repository checks pass. The service
suite reports 174 tests: 149 fail at local HTTP server setup with
`Operation not permitted`; 25 pass. This is not a green full gate.

The reaction selector passes one million selections including constant entropy.
UI stress passes 1,103 switches, 2,160 keys and 2,880 renders; LVGL free memory
returns to 13,432 bytes with a 10,328-byte minimum. Device tests: NOT RUN for v0.3.
No result from any owner-operated upgrade has been received. Unverified: actual
sound/key feel, screen, Wi-Fi/TLS memory, NVS persistence, 60-second visibility,
full HTTP tests and cloud deployment.

The current environment also denies opening USB serial, despite detecting the
board, and cannot resolve GitHub. Do not alter system protections or bypass these
restrictions; use an owner-authorized normal terminal with access. Recheck:

```bash
source /path/to/esp-idf-v5.5.3/export.sh
./tools/validate.sh
python3 tools/preview_lanlan.py --mode stress
python3 tools/archive_firmware.py verify /absolute/path/to/archive
```

The gate needs Node.js for web regression tests. Pillow is only required for atlas
import. The service uses Python 3.9+ standard library. Build success is not board
acceptance.

## Data and next actions

The Mac launcher uses `~/Library/Application Support/CyberLanlan/lanlan.sqlite3`.
Stop the old server and start the new launcher with the same database. If the
owner used another path manually, verify it rather than initializing a new DB.
Phone/device URLs use the computer LAN address at port 8787, not the phone's
127.0.0.1. Passport Wi-Fi is 2.4 GHz; configure credentials, URL and device token
locally over serial. Never request or commit real secrets, family databases or
private raw logs.

First verify/push the current branch: the owner authorizes normal publication of
v0.3 to the feature branch, not main, force pushes, a GitHub Release or debug-file
publication. Then run the full gate and perform an authorized board upgrade.
Check at least 12 consecutive OK presses, rapid presses, mute, long-return,
wake-only, sound, restart and synchronization. Record observations and decode any
crash with the matching ELF. Synthetic sound cuteness is awaiting owner feedback;
do not call it Lanlan's real voice. Cloud hosting and Xiaomi work need a later
owner decision.

## Usage-limit handoff preference

The owner asks for a handoff before approaching the five-hour allowance. When a
usage tool is available, check at the start of long tasks, major milestones and
before delivery. Use 80% consumed as this project's early documentation threshold,
not as an automatic platform trigger. Update this paired handoff at every release
and before a likely pause: branch/commit, dirty paths, done/pending work, actual
test evidence, running commands, blockers and the next executable step. Give the
owner a self-contained Chinese copy. Do not defer the note until after exhaustion,
claim incomplete work complete, or create an unrequested background scheduler.
