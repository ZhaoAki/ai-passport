#!/usr/bin/env python3
"""Convert the generated 3x2 RGBA atlas to device frames (requires Pillow).

This only performs fixed grid slicing, downscaling, background compositing and
RGB565 encoding. It does not redraw or generate character artwork.
"""
import hashlib
import json
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'assets/images/lanlan-v2/atlas.png'
DEST = SOURCE.parent
NAMES = ('idle_0', 'idle_1', 'blink', 'tilt', 'happy', 'bark')
SIZE = 96


def main():
    atlas = Image.open(SOURCE)
    if atlas.mode != 'RGBA' or atlas.width % 3 or atlas.height % 2:
        raise ValueError('Expected a 3x2 RGBA atlas')
    width, height = atlas.width // 3, atlas.height // 2
    if width != height or atlas.getchannel('A').getextrema()[0] != 0:
        raise ValueError('Expected square cells and real alpha transparency')
    packed = bytearray()
    frames = []
    for index, name in enumerate(NAMES):
        x, y = (index % 3)*width, (index // 3)*height
        cell = atlas.crop((x, y, x+width, y+height))
        if index == 0:
            cell.save(ROOT / 'web/lanlan/lanlan.png')
        frame = cell.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        # Match the existing white companion card; preserve opaque white fur.
        background = Image.new('RGBA', (SIZE, SIZE), (255,255,255,255))
        image = Image.alpha_composite(background, frame).convert('RGB')
        image.save(DEST / (name + '.png'))
        raw = bytearray()
        for r,g,b in image.getdata():
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            raw.extend((value >> 8, value & 255))
        frames.append({'name':name, 'offset':len(packed), 'bytes':len(raw),
                       'sha256':hashlib.sha256(raw).hexdigest()})
        packed.extend(raw)
    (DEST/'frames.rgb565').write_bytes(packed)
    (DEST/'manifest.json').write_text(json.dumps({
        'source':'atlas.png', 'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'source_kind':'built-in imagegen; prior Lanlan design identity and owner-provided GIF motion references',
        'format':'RGB565 big-endian', 'width':SIZE, 'height':SIZE,
        'sha256':hashlib.sha256(packed).hexdigest(), 'frames':frames,
    },indent=2)+'\n')
    print(f'Converted {len(frames)} frames, {len(packed)} Flash bytes; one {SIZE*SIZE*2}-byte RAM canvas')


if __name__ == '__main__':
    main()
