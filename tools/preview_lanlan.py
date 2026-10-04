#!/usr/bin/env python3
"""Render the REAL Cyber Lanlan application UI with host LVGL.

The harness under tests/lanlan_ui compiles main/lanlan_ui.c unchanged against
the pinned managed LVGL component (component version 9.5.0) and a few ESP-IDF
stub headers, feeds it a realistic synthetic cache and writes one PPM per
screen. This driver configures and builds that harness with cmake + ninja,
runs it, converts every PPM to PNG with a small pure-Python writer (zlib and
struct only, so no image library is required) and prints a summary table with
the pixel dimensions.

Python 3.9 compatible, standard library only.

Run it from an activated ESP-IDF environment, because cmake and ninja come from
the ESP-IDF tools:

    source .../activate-idf.sh
    python3 tools/preview_lanlan.py

Outputs:
    build/lanlan-preview/screens/*.ppm   raw captures (gitignored)
    build/lanlan-preview/screens/*.png   converted captures
    <workspace>/lanlan-preview/*.png     copies for review (default)
"""
import argparse
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
PPM_MAGIC = b"P6"
PANEL_W = 240
PANEL_H = 320


def parse_ppm(path):
    """Return (width, height, rgb_bytes) for a binary P6 PPM."""
    data = path.read_bytes()
    if not data.startswith(PPM_MAGIC):
        raise SystemExit("{}: not a binary P6 PPM".format(path))
    fields = []
    index = len(PPM_MAGIC)
    while len(fields) < 3:
        while index < len(data) and data[index : index + 1].isspace():
            index += 1
        if data[index : index + 1] == b"#":
            while index < len(data) and data[index : index + 1] != b"\n":
                index += 1
            continue
        start = index
        while index < len(data) and not data[index : index + 1].isspace():
            index += 1
        fields.append(int(data[start:index]))
    index += 1  # exactly one whitespace byte separates the header from the data
    width, height, maxval = fields
    if maxval != 255:
        raise SystemExit("{}: unsupported maxval {}".format(path, maxval))
    expected = width * height * 3
    pixels = data[index : index + expected]
    if len(pixels) != expected:
        raise SystemExit(
            "{}: truncated pixel data ({} of {} bytes)".format(path, len(pixels), expected)
        )
    return width, height, pixels


def png_chunk(tag, payload):
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def write_png(path, width, height, pixels):
    """Write an 8-bit truecolour PNG without any image library."""
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += pixels[y * stride : (y + 1) * stride]
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def png_size(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("{}: not a PNG".format(path))
    width, height = struct.unpack(">II", data[16:24])
    return width, height


def check_environment():
    pins = (ROOT / "components/bsp/include/bsp_pins.h").read_text()
    if not re.search(r"#define\s+BSP_LCD_W\s+240\b", pins):
        raise SystemExit("BSP_LCD_W is not the expected 240 in bsp_pins.h")
    if not re.search(r"#define\s+BSP_LCD_H\s+320\b", pins):
        raise SystemExit("BSP_LCD_H is not the expected 320 in bsp_pins.h")
    lvgl = ROOT / "managed_components/lvgl__lvgl/lvgl.h"
    if not lvgl.exists():
        raise SystemExit(
            "Pinned host LVGL is missing at {}.\n"
            "Run ./tools/validate.sh --firmware once so the component manager "
            "downloads lvgl 9.5.0, then re-run this driver.".format(lvgl)
        )
    version = (ROOT / "managed_components/lvgl__lvgl/lv_version.h").read_text()
    if "#define LVGL_VERSION_MAJOR 9" not in version:
        raise SystemExit("managed LVGL is not version 9.x")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--build",
        type=Path,
        default=ROOT / "build/lanlan-preview",
        help="cmake build directory (default: build/lanlan-preview)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "build/lanlan-preview/screens",
        help="capture directory for the PPM and PNG files "
        "(default: build/lanlan-preview/screens)",
    )
    parser.add_argument(
        "--copy-dir",
        type=Path,
        default=WORKSPACE / "lanlan-preview",
        help="directory the final PNGs are copied to (default: <workspace>/lanlan-preview)",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="skip the cmake configure and build step and only run the existing binary",
    )
    parser.add_argument(
        "--no-copy", action="store_true", help="do not copy the PNGs out of the build directory"
    )
    args = parser.parse_args()

    check_environment()
    args.out.mkdir(parents=True, exist_ok=True)

    binary = args.build / "lanlan_ui_preview"
    if not args.no_build:
        for tool in ("cmake", "ninja"):
            if shutil.which(tool) is None:
                raise SystemExit(
                    "{} is not on PATH. Activate ESP-IDF first, for example:\n"
                    "  source /Users/zhaowenxuan/Documents/Codex/2026-10-02/"
                    "https-github-com-folotoy-ai-passport/work/activate-idf.sh".format(tool)
                )
        subprocess.run(
            [
                "cmake",
                "-S",
                str(ROOT / "tests/lanlan_ui"),
                "-B",
                str(args.build),
                "-G",
                "Ninja",
            ],
            check=True,
        )
        subprocess.run(
            ["cmake", "--build", str(args.build), "-j", "4"],
            check=True,
        )
    elif not binary.exists():
        raise SystemExit("{} does not exist; drop --no-build".format(binary))

    subprocess.run([str(binary), str(args.out)], check=True)

    ppm_files = sorted(args.out.glob("*.ppm"))
    if not ppm_files:
        raise SystemExit("the harness produced no PPM captures in {}".format(args.out))

    rows = []
    for ppm in ppm_files:
        width, height, pixels = parse_ppm(ppm)
        png = ppm.with_suffix(".png")
        write_png(png, width, height, pixels)
        check_w, check_h = png_size(png)
        if (check_w, check_h) != (width, height) or (width, height) != (PANEL_W, PANEL_H):
            raise SystemExit(
                "{}: unexpected dimensions {}x{}".format(png, check_w, check_h)
            )
        rows.append((png.name, check_w, check_h, png.stat().st_size))

    copied = 0
    if not args.no_copy:
        args.copy_dir.mkdir(parents=True, exist_ok=True)
        for ppm in ppm_files:
            source = ppm.with_suffix(".png")
            shutil.copy2(source, args.copy_dir / source.name)
            copied += 1

    name_width = max(len(row[0]) for row in rows)
    print("")
    print("Cyber Lanlan host LVGL preview")
    print("{:<{w}}  {:>5}  {:>6}  {:>9}".format("file", "w", "h", "bytes", w=name_width))
    for name, width, height, size in rows:
        print(
            "{:<{w}}  {:>5}  {:>6}  {:>9}".format(name, width, height, size, w=name_width)
        )
    print("")
    print("{} PNG files in {}".format(len(rows), args.out))
    if not args.no_copy:
        print("{} PNG files copied to {}".format(copied, args.copy_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
