"""Validate the generated Cyber Lanlan assets without the ESP-IDF toolchain.

Run from the repository root:
    python3 -m unittest tests.test_lanlan_assets -v
    # or, matching tools/validate.sh:
    python3 tests/test_lanlan_assets.py
"""
import hashlib
import json
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SPRITE_FILES = ('lanlan_sprites.c', 'lanlan_sprites.h')
FONT_SIZES = (16, 24)


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


def c_literals(text):
    """Return every double-quoted string literal, decoded as UTF-8 text."""
    literals = []
    for match in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        raw = match.group(1)
        try:
            literals.append(raw.encode('utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8'))
        except (UnicodeDecodeError, UnicodeEncodeError):
            # Not a plain C escape sequence: keep the raw text, which is what the
            # compiler sees for ordinary UTF-8 literals.
            literals.append(raw)
    return literals


def required_characters():
    """Every printable character of the fixed inventory is a build requirement."""
    document = json.loads(read('main/lanlan/strings.json'))
    text = ''.join(value for group in document.values() if isinstance(group, dict)
                   for value in group.values())
    for source in ('main/lanlan_record.c', 'main/lanlan_model.c'):
        text += ''.join(c_literals(read(source)))
    return {c for c in text if c.isprintable()}


class LanlanAssets(unittest.TestCase):
    def test_generated_strings_match_the_json_source(self):
        document = json.loads(read('main/lanlan/strings.json'))
        header = read('main/lanlan_strings.h')
        source = read('main/lanlan_strings.c')
        identifiers = set()
        for group, items in document.items():
            if group.startswith('_'):
                continue
            self.assertIsInstance(items, dict, group)
            for key, value in items.items():
                identifier = 'LANLAN_STR_%s_%s' % (
                    ''.join(ch if ch.isalnum() else '_' for ch in group.upper()).strip('_'),
                    ''.join(ch if ch.isalnum() else '_' for ch in key.upper()).strip('_'))
                self.assertNotIn(identifier, identifiers, 'duplicate id %s' % identifier)
                identifiers.add(identifier)
                self.assertIn(identifier, header,
                              '%s is missing from the generated header' % identifier)
                self.assertIn(json.dumps(value, ensure_ascii=False), header,
                              '%s must keep the exact literal %r' % (identifier, value))
                self.assertIn(identifier, source)
                self.assertIn(',', source)
        # The table is walkable by id, which is what the font coverage and the UI
        # rely on.
        self.assertIn('const char *const lanlan_strings[LANLAN_STR_COUNT]', source)
        self.assertIn('LANLAN_STR_COUNT', header)
        self.assertIn('#define LANLAN_TXT(id) (lanlan_strings[(id)])', header)
        self.assertGreater(len(identifiers), 80)

    def test_font_inventory_covers_every_fixed_character(self):
        inventory_text = read('assets/fonts/lanlan_symbols.txt').strip('\n')
        inventory = set(inventory_text)
        required = required_characters()
        missing = sorted(c for c in required if c not in inventory)
        self.assertFalse(
            missing,
            'code points missing from assets/fonts/lanlan_symbols.txt: %s'
            % ' '.join('U+%04X' % ord(c) for c in missing))
        self.assertIn(' ', inventory)
        self.assertIn('0', inventory)

    def test_generated_fonts_exist_and_contain_every_character(self):
        required = required_characters()
        for size in FONT_SIZES:
            path = 'assets/fonts/lanlan_font_%d.c' % size
            body = read(path)
            self.assertIn('lanlan_font_%d' % size, body)
            self.assertIn('--size %d' % size, body)
            self.assertIn('lv_font_conv.js', body)
            self.assertIn('NotoSansCJKsc-Regular.otf', body)
            # The converter emits one "U+XXXX" comment per glyph, so the subset
            # itself proves coverage; a placeholder glyph is not coverage.
            for character in sorted(required):
                code = 'U+%04X' % ord(character)
                self.assertIn(code, body, '%s is missing %s' % (path, code))
            # A known-missing negative case keeps the check from passing when
            # the file failed to generate: U+9F98 is not in the fixed strings.
            self.assertNotIn('U+9F98', body)
        self.assertGreaterEqual(len(required), 200)

    def test_placeholder_art_is_marked_and_correctly_sized(self):
        for name in SPRITE_FILES:
            self.assertTrue((ROOT / 'assets/images' / name).is_file(), name)
        body = read('assets/images/lanlan_sprites.c')
        # The banner must say, in capital letters, that this is not final art.
        self.assertIn('PLACEHOLDER ART', body)
        self.assertIn('NOT THE OWNER-APPROVED FINAL APPEARANCE', body)
        frames = re.findall(r'(\w+_data)\[\] = \{', body)
        self.assertEqual(len(frames), 5, 'expected idle x2, blink, happy and bark frames')
        self.assertTrue(any('idle' in name for name in frames))
        self.assertTrue(any('blink' in name for name in frames))
        self.assertTrue(any('happy' in name for name in frames))
        self.assertTrue(any('bark' in name for name in frames))
        header = read('assets/images/lanlan_sprites.h')
        self.assertIn('LANLAN_SPRITE_WIDTH 96', header)
        self.assertIn('LANLAN_SPRITE_HEIGHT 96', header)
        # The descriptor enum lives in the hand-written header; the generated
        # table must expose one entry per state.
        for state in ('IDLE_0', 'IDLE_1', 'BLINK', 'HAPPY', 'BARK'):
            self.assertIn('LANLAN_SPRITE_%s' % state, header)
        self.assertIn('lanlan_sprites[LANLAN_SPRITE_COUNT]', body)
        self.assertEqual(body.count(', 96, 96}'), 5)
        # The frames are real RGB565 data, not an empty placeholder: one of the
        # palette colours must appear in the payload.
        rows = re.findall(r'0x([0-9A-F]{2})', body)
        self.assertGreater(len(rows), 90000)
        # The cream body colour must appear in the payload, so the frames are
        # real drawn data rather than an empty placeholder buffer.
        self.assertGreater(body.count('0xFE, 0xF3'), 1000)

    def test_pcm_manifest_matches_the_pcm_file(self):
        pcm = (ROOT / 'assets/music/lanlan_sfx_16k.pcm').read_bytes()
        manifest = json.loads(read('assets/music/lanlan_sfx_manifest.json'))
        self.assertEqual(manifest['format'], 'PCM s16le mono 16000 Hz')
        self.assertEqual(hashlib.sha256(pcm).hexdigest(), manifest['sha256'])
        self.assertGreaterEqual(len(manifest['clips']), 3)
        names = [clip['name'] for clip in manifest['clips']]
        self.assertEqual(sorted(names), sorted(set(names)), 'clip names must be unique')
        for expected in ('bark', 'chirp', 'reminder'):
            self.assertIn(expected, names)
        offset = 0
        for clip in manifest['clips']:
            with self.subTest(clip=clip['name']):
                self.assertEqual(clip['offset'], offset)
                self.assertEqual(clip['bytes'] % 2, 0)
                self.assertGreater(clip['bytes'], 16000 * 0.1 * 2)   # > 0.1 s
                self.assertLess(clip['bytes'], 16000 * 2 * 2)        # < 2 s
                data = pcm[offset:offset + clip['bytes']]
                self.assertEqual(len(data), clip['bytes'])
                self.assertEqual(hashlib.sha256(data).hexdigest(), clip['sha256'])
                samples = struct.unpack('<' + 'h' * (clip['bytes'] // 2), data)
                self.assertGreater(max(abs(x) for x in samples), 1000)
                offset += clip['bytes']
        self.assertEqual(offset, len(pcm))
        self.assertLess(len(pcm), 256 * 1024)

    def test_sfx_header_matches_the_manifest(self):
        manifest = json.loads(read('assets/music/lanlan_sfx_manifest.json'))
        header = read('main/lanlan_sfx_data.h')
        source = read('main/lanlan_sfx_data.c')
        for clip in manifest['clips']:
            self.assertIn('"%s"' % clip['name'], source)
            self.assertIn('%du' % clip['bytes'], source)
            self.assertIn('LANLAN_SFX_%s' % clip['name'].upper(), header)
        self.assertIn('LANLAN_SFX_COUNT', header)
        self.assertIn('lanlan_sfx_clips[LANLAN_SFX_COUNT]', source)


if __name__ == '__main__':
    unittest.main()
