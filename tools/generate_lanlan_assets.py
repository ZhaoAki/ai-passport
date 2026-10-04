#!/usr/bin/env python3
"""Generate every fixed asset of the Cyber Lanlan application.

Usage:
    python3 tools/generate_lanlan_assets.py [--converter JS] [--node NODE] [--skip-fonts]

Running with no arguments regenerates everything from main/lanlan/strings.json:

  * main/lanlan_strings.h / main/lanlan_strings.c   fixed UI strings (LANLAN_STR_*)
  * assets/fonts/lanlan_symbols.txt                 code-point inventory
  * assets/fonts/lanlan_font_16.c, lanlan_font_24.c 2-bpp uncompressed LVGL subsets
  * assets/images/lanlan_sprites.h/.c               96x96 RGB565 placeholder frames
  * assets/music/lanlan_sfx_16k.pcm                 16 kHz signed 16-bit mono clips
  * assets/music/lanlan_sfx_manifest.json           clip offsets, sizes and SHA-256
  * main/lanlan_sfx_data.h                          PCM clip table offsets

Only the standard library and the pinned font converter are used. The default
converter path is the JS entry point of lv_font_conv 1.5.3; the .bin shell
wrapper cannot be launched through the node binary directly. Everything is
deterministic: the same inputs produce byte-identical output, which is verified
by tests/test_lanlan_assets.py and by re-running this script.
"""
import argparse
import hashlib
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DEFAULT_NODE = Path(
    "/Users/zhaowenxuan/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/node/bin/node")
DEFAULT_CONVERTER = Path(
    "/Users/zhaowenxuan/Documents/Codex/2026-10-02/"
    "https-github-com-folotoy-ai-passport/work/font-tools/node_modules/"
    "lv_font_conv/lv_font_conv.js")

STRINGS_JSON = ROOT / "main/lanlan/strings.json"
FONT_SOURCE = ROOT / "assets/fonts/NotoSansCJKsc-Regular.otf"
SYMBOLS_TXT = ROOT / "assets/fonts/lanlan_symbols.txt"
FONT_SIZES = (16, 24)
# Files whose full-width literals must be covered by the font subset. The font
# cannot be regenerated before the labels exist, so the inventory is derived
# from the single string source plus these translation units.
LABEL_SOURCES = (
    ROOT / "main/lanlan_record.c",
    ROOT / "main/lanlan_model.c",
)

PLACEHOLDER_BANNER = (
    "THIS FILE CONTAINS PLACEHOLDER ART, NOT THE OWNER-APPROVED FINAL "
    "APPEARANCE. IT EXISTS ONLY TO VALIDATE LAYOUT, ANIMATION TIMING AND "
    "RENDERING UNTIL THE OWNER APPROVES A FROZEN SPRITE SET."
)


def identify(name):
    return "".join(c if c.isalnum() else "_" for c in name.upper()).strip("_")


def write_text(path, text):
    """Write UTF-8 with LF endings; Path.write_text has no newline argument on 3.9."""
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def flatten_strings(document):
    """Flatten the grouped JSON document into stable (LANLAN_STR_<GROUP>_<KEY>, value) ids."""
    entries = []
    seen = set()
    for group, items in document.items():
        if group.startswith("_"):
            continue
        if not isinstance(items, dict):
            raise SystemExit("group %r must be an object of string values" % group)
        for key, value in items.items():
            if not isinstance(value, str):
                raise SystemExit("string %s.%s must be text" % (group, key))
            identifier = "LANLAN_STR_%s_%s" % (identify(group), identify(key))
            if identifier in seen:
                raise SystemExit("duplicate string id %s" % identifier)
            seen.add(identifier)
            entries.append((group, key, identifier, value))
    return entries


def generate_strings(entries):
    """Emit the string inventory as a compile-time array plus a runtime table.

    Each id is a plain array declaration, so it can be used in any constant
    context (including a file-scope initializer) without a function call. The
    parallel table exists so tests and diagnostics can walk every literal by
    id, which is what the font coverage test does.
    """
    header = [
        "/* Generated from main/lanlan/strings.json by tools/generate_lanlan_assets.py.",
        " * Do not edit: add the string to the JSON and regenerate.",
        " *",
        " * These are the fixed user-visible strings of the application. Free caregiver",
        " * notes come from the server and are deliberately not part of this table. */",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "",
        "/* Stable string ids, in table order. */",
        "enum {",
    ]
    for _, _, identifier, _ in entries:
        header.append("    %s," % identifier)
    header += [
        "    LANLAN_STR_COUNT",
        "};",
        "",
        "/* Exact UTF-8 literals. Usable in any constant expression, for example a",
        " * file-scope initializer. */",
    ]
    for _, _, identifier, value in entries:
        header.append("#define %s %s" % (identifier, json.dumps(value, ensure_ascii=False)))
    header += [
        "",
        "/* Look up the same literal by id at runtime. */",
        "#define LANLAN_TXT(id) (lanlan_strings[(id)])",
        "",
        "/* Index -> exact UTF-8 literal, for table-driven tests and diagnostics. */",
        "extern const char *const lanlan_strings[LANLAN_STR_COUNT];",
        "/* Group name of each string, for diagnostics and the font coverage test. */",
        "extern const char *const lanlan_string_groups[LANLAN_STR_COUNT];",
        "extern const char *const lanlan_string_keys[LANLAN_STR_COUNT];",
        "",
    ]
    source = [
        "/* Generated from main/lanlan/strings.json by tools/generate_lanlan_assets.py. */",
        '#include "lanlan_strings.h"',
        "",
        "const char *const lanlan_strings[LANLAN_STR_COUNT] = {",
    ]
    for _, _, identifier, _ in entries:
        source.append("    %s," % identifier)
    source += [
        "};",
        "",
        "const char *const lanlan_string_groups[LANLAN_STR_COUNT] = {",
    ]
    for group, _, _, _ in entries:
        source.append("    %s," % json.dumps(group))
    source += [
        "};",
        "",
        "const char *const lanlan_string_keys[LANLAN_STR_COUNT] = {",
    ]
    for _, key, _, _ in entries:
        source.append("    %s," % json.dumps(key))
    source += [
        "};",
        "",
    ]
    write_text(ROOT / "main/lanlan_strings.h", "\n".join(header))
    write_text(ROOT / "main/lanlan_strings.c", "\n".join(source))


def required_codepoints(entries):
    """Every non-ASCII character the application can render from fixed text."""
    text = "".join(value for _, _, _, value in entries)
    text += "".join(path.read_text(encoding="utf-8") for path in LABEL_SOURCES)
    text += "".join(chr(n) for n in range(0x20, 0x7F))
    return sorted({c for c in text if c.isprintable()})


def verify_generated_coverage(output, codepoints):
    """Prove the subset really contains every requested code point.

    The converter writes a `U+XXXX` comment per glyph, so the generated file
    itself is the evidence; no Python font library is needed and the check runs
    on the same bytes the firmware compiles.
    """
    body = output.read_text(encoding="utf-8")
    missing = [c for c in codepoints if ("U+%04X" % ord(c)) not in body]
    if missing:
        raise SystemExit("%s is missing glyphs: %r" % (output.name, missing))
    return len(codepoints)


def generate_fonts(codepoints, node, converter):
    symbols = "".join(codepoints)
    write_text(SYMBOLS_TXT, symbols + "\n")
    if not converter.is_file():
        raise SystemExit(
            "missing lv_font_conv entry point %s (pass --converter)" % converter)
    for size in FONT_SIZES:
        output = ROOT / ("assets/fonts/lanlan_font_%d.c" % size)
        arguments = [
            "--font", str(FONT_SOURCE.relative_to(ROOT)),
            "--symbols", symbols,
            "--size", str(size),
            "--bpp", "2",
            "--format", "lvgl",
            "--no-compress",
            "--lv-font-name", "lanlan_font_%d" % size,
            "--lv-include", "lvgl.h",
            "-o", str(output.relative_to(ROOT)),
        ]
        command = [str(node), str(converter)] + arguments
        subprocess.run(command, cwd=str(ROOT), check=True)
        body = output.read_text(encoding="utf-8")
        # The full --symbols list is already in the converter's own "Opts:"
        # header; repeat the command with it abbreviated so this banner stays
        # readable while still being the exact invocation.
        banner_command = list(command)
        banner_command[banner_command.index("--symbols") + 1] = (
            "<%d code points, see assets/fonts/lanlan_symbols.txt>" % len(codepoints))
        banner = (
            "/* Cyber Lanlan font subset, generated by tools/generate_lanlan_assets.py.\n"
            " * Source: assets/fonts/NotoSansCJKsc-Regular.otf (Noto CJK, SIL OFL).\n"
            " * Converter: lv_font_conv 1.5.3 (pinned).\n"
            " * Exact command line:\n"
            " *   %s\n"
            " */\n" % " ".join(banner_command)
        )
        write_text(output, banner + body)
        # Re-read so the check covers the final on-disk bytes.
        verified = verify_generated_coverage(output, codepoints)
    return verified


# ----------------------------------------------------------------------- art --

SPRITE_SIZE = 96
# A small fixed palette keeps the frame data readable in the generated file and
# makes the placeholder unmistakably a placeholder.
SPRITE_COLORS = {
    "background": 0xFFFF,   # white
    "cream": 0xFEF3,        # cream body
    "shade": 0xE6AC,        # cream shade
    "dark": 0x39E7,         # dark eyes and nose
    "white": 0xFFFF,        # white chest and muzzle
    "tuft": 300,            # placeholder, replaced below
}
SPRITE_COLORS["tuft"] = 0xFE0F
HEAD_CENTER = (48, 44)
HEAD_RADII = (30, 26)
BODY_ELLIPSE = ((48, 74), (26, 20))
CHEST_ELLIPSE = ((48, 80), (14, 14))
MUZZLE_ELLIPSE = ((48, 66), (13, 9))
EAR_ELLIPSES = (((30, 52), (8, 14)), ((66, 52), (8, 14)))
TUFT_ELLIPSES = (((40, 22), (8, 7)), ((48, 18), (9, 8)), ((56, 22), (8, 7)))
EYE_ELLIPSES = (((39, 42), (5, 6)), ((57, 42), (5, 6)))
BLINK_ROWS = (42, 45)
NOSE_ROWS = (58, 61)


def sprite_frames():
    """Draw the idle/blink/happy/bark frames with simple primitives.

    The shape is a cream puppy silhouette: a round head with a fluffy tuft,
    drop ears, two dark eyes, a white chest and a small muzzle. It is drawn
    from ellipses and rectangles only so the drawing stays dependency-free and
    reviewable; the owner-approved art will replace it wholesale.
    """
    frames = []

    def blank():
        return [[SPRITE_COLORS["background"]] * SPRITE_SIZE for _ in range(SPRITE_SIZE)]

    def ellipse(canvas, cx, cy, rx, ry, color):
        for y in range(max(0, cy - ry), min(SPRITE_SIZE, cy + ry + 1)):
            for x in range(max(0, cx - rx), min(SPRITE_SIZE, cx + rx + 1)):
                dx = (x - cx) / float(rx)
                dy = (y - cy) / float(ry)
                if dx * dx + dy * dy <= 1.0:
                    canvas[y][x] = color

    def draw(dy, blink):
        canvas = blank()
        # Drop ears behind the head, then the body behind the head.
        for center, radii in EAR_ELLIPSES:
            ellipse(canvas, center[0], center[1], radii[0], radii[1], SPRITE_COLORS["shade"])
        ellipse(canvas, BODY_ELLIPSE[0][0], BODY_ELLIPSE[0][1], BODY_ELLIPSE[1][0],
                BODY_ELLIPSE[1][1], SPRITE_COLORS["cream"])
        # Head, then the white chest and muzzle in front of it.
        ellipse(canvas, HEAD_CENTER[0], HEAD_CENTER[1], HEAD_RADII[0], HEAD_RADII[1],
                SPRITE_COLORS["cream"])
        ellipse(canvas, CHEST_ELLIPSE[0][0], CHEST_ELLIPSE[0][1], CHEST_ELLIPSE[1][0],
                CHEST_ELLIPSE[1][1], SPRITE_COLORS["white"])
        ellipse(canvas, MUZZLE_ELLIPSE[0][0], MUZZLE_ELLIPSE[0][1], MUZZLE_ELLIPSE[1][0],
                MUZZLE_ELLIPSE[1][1], SPRITE_COLORS["white"])
        # Fluffy head tuft.
        for center, radii in TUFT_ELLIPSES:
            ellipse(canvas, center[0], center[1], radii[0], radii[1], SPRITE_COLORS["tuft"])
        # Eyes: closed rows when blinking, otherwise two dark ovals.
        if blink:
            for y in range(BLINK_ROWS[0], BLINK_ROWS[1]):
                for x in range(EYE_ELLIPSES[0][0][0] - 6, EYE_ELLIPSES[1][0][0] + 7):
                    canvas[y][x] = SPRITE_COLORS["dark"]
        else:
            for center, radii in EYE_ELLIPSES:
                ellipse(canvas, center[0], center[1], radii[0], radii[1], SPRITE_COLORS["dark"])
        # Nose.
        for y in range(NOSE_ROWS[0], NOSE_ROWS[1]):
            for x in range(45, 52):
                canvas[y][x] = SPRITE_COLORS["dark"]
        if dy:
            shifted = [[SPRITE_COLORS["background"]] * SPRITE_SIZE for _ in range(SPRITE_SIZE)]
            for y in range(SPRITE_SIZE):
                source = y - dy
                if 0 <= source < SPRITE_SIZE:
                    shifted[y] = canvas[source]
            canvas = shifted
        return canvas

    # idle: two breathing frames; blink: one closed-eye frame; happy and bark
    # are single reaction frames.
    frames.append(("lanlan_frame_idle_0", draw(0, False)))
    frames.append(("lanlan_frame_idle_1", draw(1, False)))
    frames.append(("lanlan_frame_blink", draw(0, True)))
    frames.append(("lanlan_frame_happy", draw(0, False)))
    frames.append(("lanlan_frame_bark", draw(0, False)))
    return frames


def rgb565(pixel):
    red = (pixel >> 11) & 0x1F
    green = (pixel >> 5) & 0x3F
    blue = pixel & 0x1F
    return (red << 11) | (green << 5) | blue


def generate_sprites():
    frames = sprite_frames()
    lines = [
        "/* %s" % PLACEHOLDER_BANNER,
        " *",
        " * Generated by tools/generate_lanlan_assets.py. 96x96 RGB565 frames stored",
        " * big-endian (byte 0 = high byte), which is the layout the BSP canvas uses.",
        " * Frames are const flash data; the animation copies one frame at a time.",
        " */",
        '#include "lanlan_sprites.h"',
        "",
    ]
    offsets = []
    payload = []
    for name, canvas in frames:
        offsets.append((name, len(payload)))
        for row in canvas:
            for pixel in row:
                value = rgb565(pixel)
                payload.append((value >> 8) & 0xFF)
                payload.append(value & 0xFF)
    for name, _ in offsets:
        lines.append("/* %s: %d bytes */" % (name, SPRITE_SIZE * SPRITE_SIZE * 2))
        lines.append("static const uint8_t %s_data[] = {" % name)
        start = dict(offsets)[name]
        data = payload[start:start + SPRITE_SIZE * SPRITE_SIZE * 2]
        for index in range(0, len(data), 16):
            lines.append("    " + ", ".join("0x%02X" % byte for byte in data[index:index + 16]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("const lanlan_sprite_t lanlan_sprites[LANLAN_SPRITE_COUNT] = {")
    for name, _ in offsets:
        lines.append("    {%s_data, %d, %d}," % (name, SPRITE_SIZE, SPRITE_SIZE))
    lines.append("};")
    lines.append("")
    write_text(ROOT / "assets/images/lanlan_sprites.c", "\n".join(lines))
    return len(payload)


# --------------------------------------------------------------------- audio --

SAMPLE_RATE = 16000


def pcm_clip(name, seconds, render):
    count = int(SAMPLE_RATE * seconds)
    samples = []
    for index in range(count):
        time = index / float(SAMPLE_RATE)
        value = render(time, index, count)
        value = max(-1.0, min(1.0, value))
        samples.append(int(round(value * 32000.0)))
    return name, struct.pack("<%dh" % len(samples), *samples)


def clip_bark():
    """Short bark: a fast downward sweep with an exponential decay envelope."""

    def render(time, index, count):
        envelope = math.exp(-9.0 * time) * (1.0 - math.exp(-90.0 * time))
        frequency = 620.0 - 240.0 * (time / 0.34)
        tone = math.sin(2.0 * math.pi * frequency * time)
        # Self-produced noise shaped by the same envelope: no sampled material.
        noise = math.sin(2.0 * math.pi * 1733.0 * time) * math.sin(2.0 * math.pi * 2311.0 * time)
        return 0.85 * envelope * tone + 0.25 * envelope * noise

    return pcm_clip("bark", 0.34, render)


def clip_chirp():
    """Happy chirp: two rising sine notes with a soft cosine envelope."""

    def render(time, index, count):
        if time < 0.12:
            frequency = 780.0 + 900.0 * (time / 0.12)
            envelope = math.sin(math.pi * min(1.0, time / 0.12))
        else:
            local = (time - 0.12) / 0.14
            frequency = 1180.0 + 700.0 * local
            envelope = math.sin(math.pi * min(1.0, local))
        return 0.6 * envelope * math.sin(2.0 * math.pi * frequency * time)

    return pcm_clip("chirp", 0.26, render)


def clip_reminder():
    """Soft reminder tone: a gentle two-note chime with a slow attack."""

    def render(time, index, count):
        if time < 0.2:
            frequency, local, length = 660.0, time, 0.2
        else:
            frequency, local, length = 880.0, time - 0.2, 0.24
        envelope = math.sin(math.pi * min(1.0, local / length)) ** 2
        return 0.35 * envelope * math.sin(2.0 * math.pi * frequency * time)

    return pcm_clip("reminder", 0.44, render)


def generate_audio():
    clips = [clip_bark(), clip_chirp(), clip_reminder()]
    packed = bytearray()
    manifest_clips = []
    offsets = []
    for name, data in clips:
        offsets.append((name, len(packed), len(data)))
        manifest_clips.append({
            "name": name,
            "offset": len(packed),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
        packed.extend(data)
    pcm_path = ROOT / "assets/music/lanlan_sfx_16k.pcm"
    pcm_path.write_bytes(bytes(packed))
    manifest = {
        "format": "PCM s16le mono 16000 Hz",
        "license": "self-produced tones and synthetic noise, no third-party audio",
        "sha256": hashlib.sha256(bytes(packed)).hexdigest(),
        "clips": manifest_clips,
    }
    write_text(ROOT / "assets/music/lanlan_sfx_manifest.json",
               json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    header = [
        "/* Generated by tools/generate_lanlan_assets.py. Do not edit.",
        " *",
        " * Byte offsets into assets/music/lanlan_sfx_16k.pcm, a 16 kHz signed 16-bit",
        " * little-endian mono pack. The glue layer feeds one clip at a time.",
        " */",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    const char *name;",
        "    uint32_t offset;",
        "    uint32_t bytes;",
        "} lanlan_sfx_clip_t;",
        "",
        "enum {",
    ]
    for index, (name, _, _) in enumerate(offsets):
        header.append("    LANLAN_SFX_%s = %d," % (identify(name), index))
    header += [
        "    LANLAN_SFX_COUNT",
        "};",
        "",
        "extern const lanlan_sfx_clip_t lanlan_sfx_clips[LANLAN_SFX_COUNT];",
        "",
    ]
    write_text(ROOT / "main/lanlan_sfx_data.h", "\n".join(header))
    source = [
        "/* Generated by tools/generate_lanlan_assets.py. Do not edit. */",
        '#include "lanlan_sfx_data.h"',
        "",
        "const lanlan_sfx_clip_t lanlan_sfx_clips[LANLAN_SFX_COUNT] = {",
    ]
    for name, offset, length in offsets:
        source.append('    {%s, %du, %du},' % (json.dumps(name), offset, length))
    source += [
        "};",
        "",
    ]
    write_text(ROOT / "main/lanlan_sfx_data.c", "\n".join(source))
    return len(packed)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--converter", type=Path, default=DEFAULT_CONVERTER,
                        help="lv_font_conv JS entry point (default: pinned 1.5.3)")
    parser.add_argument("--node", type=Path, default=DEFAULT_NODE,
                        help="node executable used to run the converter")
    parser.add_argument("--skip-fonts", action="store_true",
                        help="regenerate strings/art/audio without invoking the converter")
    arguments = parser.parse_args()

    document = json.loads(STRINGS_JSON.read_text(encoding="utf-8"))
    entries = flatten_strings(document)
    generate_strings(entries)
    codepoints = required_codepoints(entries)
    if not FONT_SOURCE.is_file():
        raise SystemExit("missing source font %s" % FONT_SOURCE)
    if arguments.skip_fonts:
        verified = len(codepoints)
    else:
        verified = generate_fonts(codepoints, arguments.node, arguments.converter)
    sprite_bytes = generate_sprites()
    pcm_bytes = generate_audio()
    print("Strings: %d fixed literals" % len(entries))
    print("Fonts: %d code points, sizes %s"
          % (verified, ", ".join(str(size) for size in FONT_SIZES)))
    print("Sprites: %d bytes of RGB565 frame data" % sprite_bytes)
    print("Audio: %d bytes of 16 kHz mono PCM" % pcm_bytes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
