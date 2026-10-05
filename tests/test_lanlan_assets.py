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


def parse_generated_font(path):
    """Return (adv_w in 1/16 px per glyph id, codepoint -> glyph id).

    The converter writes one glyph_dsc entry per glyph and a small cmap either
    as a dense range or as a sparse delta list relative to range_start, so the
    generated C file alone is enough to measure rendered text width.
    """
    text = read(path)
    advances = [int(value) for value in re.findall(r'\.adv_w\s*=\s*(\d+)', text)]
    cmap = {}
    sparse_arrays = re.findall(
        r'static const uint16_t unicode_list_\d+\[\] = \{(.*?)\};', text, re.S)
    sparse_index = 0
    for match in re.finditer(
            r'\{\s*\.range_start = (\d+), \.range_length = (\d+), '
            r'\.glyph_id_start = (\d+),(.*?)\}', text, re.S):
        start, length, gid = int(match.group(1)), int(match.group(2)), int(match.group(3))
        if 'SPARSE' in match.group(4):
            deltas = [int(value, 16) for value in
                      re.findall(r'0x([0-9a-fA-F]+)', sparse_arrays[sparse_index])]
            sparse_index += 1
            for index, delta in enumerate(deltas):
                cmap[start + delta] = gid + index
        else:
            for index in range(length):
                cmap[start + index] = gid + index
    return advances, cmap


def rendered_width_1_16(path, text):
    advances, cmap = parse_generated_font(path)
    total = 0
    missing = []
    for character in text:
        gid = cmap.get(ord(character))
        if gid is None or gid >= len(advances):
            missing.append(character)
            continue
        total += advances[gid]
    return total, missing


class LanlanAssets(unittest.TestCase):
    def test_host_display_pool_matches_firmware(self):
        firmware = int(re.search(r'CONFIG_LV_MEM_SIZE_KILOBYTES=(\d+)', read('sdkconfig.defaults')).group(1))
        host = int(re.search(r'#define LV_MEM_SIZE \((\d+) \* 1024\)', read('tests/lanlan_ui/lv_conf.h')).group(1))
        self.assertEqual(firmware, host, 'UI stress must use the firmware allocation budget')

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

    def test_character_frames_match_converted_atlas_and_memory_budget(self):
        directory = ROOT / 'assets/images/lanlan-v2'
        manifest = json.loads((directory / 'manifest.json').read_text())
        raw = (directory / 'frames.rgb565').read_bytes()
        self.assertEqual(hashlib.sha256((directory / 'atlas.png').read_bytes()).hexdigest(), manifest['source_sha256'])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), manifest['sha256'])
        self.assertEqual(manifest['format'], 'RGB565 big-endian')
        self.assertEqual((manifest['width'], manifest['height']), (96, 96))
        self.assertEqual([f['name'] for f in manifest['frames']],
                         ['idle_0', 'idle_1', 'blink', 'tilt', 'happy', 'bark'])
        source = read('assets/images/lanlan_sprites.c')
        encoded = bytes(int(v, 16) for v in re.findall(r'0x([0-9A-F]{2})', source))
        self.assertEqual(encoded, raw, 'compiled pixels must match converted assets')
        self.assertEqual(len(raw), 6 * 96 * 96 * 2)
        self.assertEqual(len({f['sha256'] for f in manifest['frames']}), 6)
        for index, frame in enumerate(manifest['frames']):
            self.assertEqual(frame['offset'], index * 18432)
            self.assertEqual(frame['bytes'], 18432)
            data = raw[frame['offset']:frame['offset'] + frame['bytes']]
            self.assertEqual(hashlib.sha256(data).hexdigest(), frame['sha256'])
        header = read('assets/images/lanlan_sprites.h')
        self.assertIn('LANLAN_SPRITE_WIDTH 96', header)
        self.assertIn('LANLAN_SPRITE_HEIGHT 96', header)
        for state in ('IDLE_0', 'IDLE_1', 'BLINK', 'TILT', 'HAPPY', 'BARK'):
            self.assertIn('LANLAN_SPRITE_' + state, header)
        self.assertEqual(source.count(', 96, 96}'), 6)

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

    def test_hint_and_timeout_strings_fit_their_widgets(self):
        document = json.loads(read('main/lanlan/strings.json'))
        hints = document['hints']
        settings = document['settings']
        # The three key hints plus the composed bar the UI draws as one label.
        for key in ('up_down', 'select', 'ok', 'confirm', 'long_press', 'back'):
            self.assertIn(key, hints)
            self.assertTrue(hints[key])
        bar = document['hint_bar']['bar']
        self.assertIn(hints['up_down'], bar)
        self.assertIn(hints['select'], bar)
        self.assertIn(hints['ok'], bar)
        self.assertIn(hints['confirm'], bar)
        self.assertIn(hints['long_press'], bar)
        self.assertIn(hints['back'], bar)

        # The hint label in main/lanlan_ui.c is x=12, width=216 at 16 px.
        band_1_16, missing = rendered_width_1_16('assets/fonts/lanlan_font_16.c', bar)
        self.assertFalse(missing, 'hint bar needs missing glyphs: %r' % missing)
        self.assertLessEqual(band_1_16 / 16.0, 216.0,
                             'the hint bar is %0.2f px wide and would overflow the 216 px band'
                             % (band_1_16 / 16.0))
        self.assertLess(band_1_16 / 16.0, 240.0)

        # Both new settings row labels fit their 130 px label box at 16 px.
        for key in ('row_dim', 'row_screen_off'):
            width_1_16, missing = rendered_width_1_16('assets/fonts/lanlan_font_16.c',
                                                      settings[key])
            self.assertFalse(missing, '%s needs missing glyphs: %r' % (key, missing))
            self.assertLessEqual(width_1_16 / 16.0, 130.0)

        # Every allowed step has an exact value literal in the generated table.
        for key in ('value_dim_15s', 'value_dim_30s', 'value_dim_60s', 'value_dim_120s',
                    'value_screen_off_60s', 'value_screen_off_90s',
                    'value_screen_off_180s', 'value_screen_off_300s',
                    'dim_seconds', 'dim_minutes'):
            self.assertIn(key, settings)
            self.assertTrue(settings[key])

    def test_new_strings_are_covered_by_both_font_sizes(self):
        document = json.loads(read('main/lanlan/strings.json'))
        new_texts = [document['hint_bar']['bar']] + list(document['hints'].values()) + [
            document['settings'][key] for key in
            ('row_dim', 'row_screen_off', 'value_dim_15s', 'value_dim_30s', 'value_dim_60s',
             'value_dim_120s', 'value_screen_off_60s', 'value_screen_off_90s',
             'value_screen_off_180s', 'value_screen_off_300s')]
        for size in FONT_SIZES:
            body = read('assets/fonts/lanlan_font_%d.c' % size)
            for text in new_texts:
                for character in text:
                    if character in (' ', '\n'):
                        continue
                    code = 'U+%04X' % ord(character)
                    self.assertIn(code, body,
                                  'lanlan_font_%d is missing %s from %r' % (size, code, text))
        # The four characters that were only introduced by this round.
        for character in ('\u8c03', '\u7184', '\u5c4f', '\u79d2'):
            code = 'U+%04X' % ord(character)
            for size in FONT_SIZES:
                self.assertIn(code, read('assets/fonts/lanlan_font_%d.c' % size))

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
