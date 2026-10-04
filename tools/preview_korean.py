#!/usr/bin/env python3
"""Render the actual Korean application using the pinned managed LVGL on the host."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build/korean-preview')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/korean-preview/screens')
    args = parser.parse_args()
    pins = (ROOT / 'components/bsp/include/bsp_pins.h').read_text()
    assert re.search(r'#define BSP_LCD_W\s+240\b', pins)
    assert re.search(r'#define BSP_LCD_H\s+320\b', pins)
    display_header = (ROOT / 'components/bsp/include/bsp_display.h').read_text()
    assert '#define BSP_LVGL_SCREEN_RADIUS 30' in display_header
    assert (ROOT / 'managed_components/lvgl__lvgl/lvgl.h').exists(), 'Run the firmware gate to download pinned LVGL first'
    args.output.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cmake', '-S', str(ROOT / 'tests/korean_ui'), '-B', str(args.build), '-G', 'Ninja'], check=True)
    subprocess.run(['cmake', '--build', str(args.build), '-j', '4'], check=True)
    subprocess.run([str(args.build / 'korean_ui_preview'), str(args.output)], check=True)
    print('Actual LVGL screenshots:', args.output)


if __name__ == '__main__':
    main()
