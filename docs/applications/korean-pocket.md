<p align="right"><a href="korean-pocket.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Korean Pocket

An offline Korean learning prototype for the ESP32-C3 AI Passport. It starts in a new five-entry learning home, not the hardware-test menu. The course contains 24 basic letters (10 vowels and 14 consonants) and 36 everyday vocabulary/phrase cards. Compound vowels, tense consonants, pronunciation changes, grammar, speech scoring and cloud AI are outside this first increment.

## Controls

| Page | UP / DOWN click | OK click | Hold DOWN | Hold OK |
| --- | --- | --- | --- | --- |
| Home | Select one of five entries | Enter | No action | Home |
| Letter / word card | Previous / next card | Show / hide meaning | Play pronunciation | Home |
| Question | Select one of three meanings | Submit; confirm again to advance | Play / replay target audio | Home, abandon round |
| Result / empty / audio failure | No action | Home | No action | Home |

The first key after screen-off only wakes the screen. A question receives at most one answer; each round samples up to five distinct items. The reading and listening quizzes use the vocabulary deck. Review draws up to five records from the mistake list and retains each mistake until two consecutive correct answers. Alphabet review is supported by the model, though this release has no alphabet quiz entry. Card visits are recorded as **viewed**, not mastered.

## Persistence and power

`korean_model.c` has no ESP-IDF/LVGL dependencies. The application serializes 84 bytes to the NVS namespace `korean_course`, key `progress_v1`, after a new card visit or answer. This stores viewed/mistake bitmaps, cumulative answer/correct counts and per-item streaks. Record IDs and ordering are fixed by course version 1; a future course must migrate this format instead of reordering existing IDs. Length, bitmap bounds, count relationships and streak ranges are checked before loading. NVS provides the underlying integrity/commit semantics. Invalid data is retained; the app continues in RAM and shows an unsaved warning. It never erases other applications' NVS on errors.

There is no trustworthy offline calendar in this increment: counts are cumulative, not daily streaks. The backlight dims after 30 seconds and turns off after 90 seconds; the next key wakes it. The idle audio codec is suspended. CPU deep/light sleep and measured battery lifetime are not implemented/verified.

Button callbacks enqueue only. A single application worker owns the model, saving, PCM feed and UI updates, with LVGL locking around rendering. Audio streams in 512-byte aligned chunks from Flash; no whole-clip buffer is allocated. Codec writes/suspend and NVS commits are serialized. At most about one DMA queue remains audible on navigation; real latency/clicks require device testing. Wi-Fi and Bluetooth are not started.

## Fonts and speech

Generated Noto Sans CJK SC subsets cover the fixed UI and course at 16/28 px, 2 bpp, uncompressed. The source font and SIL OFL are under `assets/fonts/`. The converter is `lv_font_conv@1.5.3`; required code points are in `korean_symbols.txt`. The app selects these fonts on every label. Fixed resources do not support arbitrary server text.

The speech model is [Meta MMS Korean](https://huggingface.co/facebook/mms-tts-kor), revision `1b6491366d2ed6ea8e4e735607155d9f0110df29`, with Transformers 4.57.1 / PyTorch 2.9.1. Generation uses seeds 20261002 + item ID and `split_special_tokens=True` so the model's reserved literal `u` is processed as an ordinary romanized character. There are 60 synthetic clips in the embedded 16 kHz mono PCM pack; metadata and SHA-256 are alongside it. Alphabet clips speak vowel syllables or consonant names, not an isolated consonant sound. Romanization is an aid and does not replace listening.

**The MMS model is CC-BY-NC-4.0. This speech asset is for a personal, noncommercial prototype; replace it with commercially licensed recordings before commercial distribution.** The source firmware retains the repository's MIT license; the font uses SIL OFL. The model weights are not included in firmware. Audio quality and educational pronunciation have not been approved by a Korean teacher. Course notes were authored for this prototype; references for alphabet/romanization are the [National Institute of Korean Language](https://m.korean.go.kr/front_eng/roman/roman_01.do) and its [Hangeul introduction](https://www.korean.go.kr/common/download.do?c_file_name=89c36d8f-0c82-401b-b293-9394b66a058e_0.pdf&file_path=reportData&o_file_name=The+Korean+Alphabet_Hangeul.pdf).

Regenerate course/font/audio assets with `tools/generate_korean_assets.py`; use Python with `torch==2.9.1`, `transformers==4.57.1`, `uroman==1.3.1.1`, `fonttools==4.66.1` and NumPy. Install the pinned converter in a separate tools folder, then run from the repository root:

```bash
python tools/generate_korean_assets.py --audio \
  --font assets/fonts/NotoSansCJKsc-Regular.otf \
  --converter /path/to/node_modules/lv_font_conv/lv_font_conv.js --node /path/to/node
```

The initial model download requires network access; device operation does not. Existing generated assets are enough for a normal firmware build.

## Validation and device acceptance

Use ESP-IDF 5.5.3 and `./tools/validate.sh`. On this restricted macOS host, the workspace toolchain uses idf-component-manager 2.5.2 with a POSIX-only compatibility adjustment: its CMake PID helper returns `os.getppid()` instead of enumerating the host's process tree. This follows the helper's documented direct-parent behavior on POSIX and avoids reading unavailable process information. The patch is confined to the workspace toolchain; firmware dependencies are unmodified, registry-verified releases. Older manager versions incorrectly reclassified managed components as local during CMake retries, so they are not the delivery configuration. Scope `IDF_COMPONENT_CACHE_PATH` to a writable workspace cache when needed. The static gate uses Darwin's `-dead_strip` flag and Python SHA-256 verification for actionlint.

The Korean host tests cover navigation, round uniqueness, answer locking, review clearing, save/reload validation, saturated counters and 500 random seeds. The asset checker validates clip bounds/non-silence and font inventory. The LVGL host renderer uses actual UI/font code and checks glyph descriptors, geometry and all course cards; host screenshots are previews, not physical-device observations.

Run `python tools/preview_korean.py` with CMake and Ninja available to reproduce PPM screenshots after the firmware gate has downloaded the pinned components.

On the device, check startup, all five entries, long-press navigation, audible clips/replay/cancellation, audio-unavailable degradation, Han/Korean rendering and longest labels, repeated page changes, wrong-answer review, two correct answers clearing a mistake, reboot preservation, dim/off/wake and plausible battery readings. Inspect free heap and largest block while navigating/playing. Verify that an input waking a dark screen does not answer a question. If storage fails, the warning must remain visible while the app runs in RAM.

Flash only after authorization. Write the verified merged `full.bin` at `0x0`; this replaces the installed application and may reset NVS data. Do not use the app-only image at `0x0` or perform a full-chip erase as a routine step. The exact build hash and automated results belong to the delivered validation report.
