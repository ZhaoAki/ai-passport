#!/usr/bin/env python3
"""Generate a fixed offline course and personal TTS audio. See the paired app guide."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def cstr(value):
    return json.dumps(value, ensure_ascii=False)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--font', type=Path)
    p.add_argument('--converter', type=Path)
    p.add_argument('--node', default='node')
    p.add_argument('--audio', action='store_true')
    args = p.parse_args()
    course = json.loads((ROOT / 'main/korean/course.json').read_text())
    assert course['version'] == 1 and len(course['letters']) == 24 and len(course['words']) == 36
    items = course['letters'] + course['words']
    assert len({x[0] for x in items}) == len(items)
    lines = ['// Generated from main/korean/course.json. Do not edit.', '#include "korean_model.h"',
             'const ko_item_t ko_items[KO_COUNT] = {']
    for item in items:
        lines.append('    {' + ', '.join(cstr(x) for x in item) + '},')
    lines.append('};\n')
    (ROOT / 'main/korean_course.c').write_text('\n'.join(lines))
    if args.audio:
        packed = bytearray()
        offsets = []
        import numpy as np
        import torch
        from transformers import AutoTokenizer, VitsModel
        revision = '1b6491366d2ed6ea8e4e735607155d9f0110df29'
        model = VitsModel.from_pretrained('facebook/mms-tts-kor', revision=revision)
        tokenizer = AutoTokenizer.from_pretrained('facebook/mms-tts-kor', revision=revision)
        assert model.config.sampling_rate == 16000
        torch.set_num_threads(2)
        for i, item in enumerate(items):
            torch.manual_seed(20261002 + i)
            # The published Korean tokenizer reserves the literal 'u' as a special token.
            # Tokenize it as a regular character, retaining interspersed blanks.
            inputs = tokenizer(item[3], return_tensors='pt', split_special_tokens=True)
            assert inputs['input_ids'].numel() > 1, 'empty tokens: ' + item[3]
            with torch.no_grad():
                samples_float = model(**inputs).waveform[0].cpu().numpy()
            assert np.isfinite(samples_float).all()
            samples = np.clip(samples_float * 25000, -32767, 32767).astype('<i2')
            voiced = np.flatnonzero(np.abs(samples.astype(np.int32)) > 160)
            assert len(voiced), 'silent audio: ' + item[0]
            lo = max(0, int(voiced[0]) - 1280)
            hi = min(len(samples), int(voiced[-1]) + 1920)
            clip = samples[lo:hi].tobytes() + bytes(3200)
            offsets.append((len(packed), len(clip)))
            packed.extend(clip)
            print('Synthesized', i + 1, item[0], flush=True)
        dest = ROOT / 'assets/music/korean_mms_16k.pcm'
        dest.write_bytes(packed)
        lines = ['// Generated PCM byte offsets; every clip is 16 kHz mono signed little-endian 16-bit.',
                 '#include "korean_audio_data.h"', 'const ko_audio_clip_t ko_audio_clips[KO_COUNT] = {']
        lines += ['    {%du, %du},' % x for x in offsets]
        lines.append('};\n')
        (ROOT / 'main/korean_audio_data.c').write_text('\n'.join(lines))
        manifest = {'voice': 'facebook/mms-tts-kor', 'revision': revision, 'seed_base': 20261002, 'license': 'CC-BY-NC-4.0', 'transformers': '4.57.1', 'torch': '2.9.1', 'format': 'PCM s16le mono 16000 Hz',
                    'sha256': hashlib.sha256(packed).hexdigest(),
                    'clips': [{'text': x[3], 'offset': off, 'bytes': count}
                              for x, (off, count) in zip(items, offsets)]}
        (ROOT / 'assets/music/korean_mms_manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        print('Audio:', len(packed), 'bytes,', len(items), 'clips')
    if args.font:
        from fontTools.ttLib import TTFont
        source = TTFont(str(args.font))
        cmap = source.getBestCmap()
        text = ''.join(''.join(item) for item in items)
        text += (ROOT / 'main/korean_ui.c').read_text()
        text += ''.join(chr(n) for n in range(32, 127))
        symbols = ''.join(sorted(set(c for c in text if c.isprintable())))
        missing = [c for c in symbols if ord(c) not in cmap]
        assert not missing, 'source font missing: ' + repr(missing)
        (ROOT / 'assets/fonts/korean_symbols.txt').write_text(symbols + '\n')
        for size in [16, 28]:
            subprocess.run([args.node, str(args.converter), '--font', str(args.font), '--symbols', symbols,
                            '--size', str(size), '--bpp', '2', '--format', 'lvgl', '--no-compress',
                            '--lv-font-name', 'korean_font_' + str(size), '--lv-include', 'lvgl.h',
                            '-o', str(ROOT / ('assets/fonts/korean_font_%d.c' % size))], check=True)
        print('Fonts:', len(symbols), 'verified source glyphs at 16 and 28 px')

if __name__ == '__main__':
    main()
