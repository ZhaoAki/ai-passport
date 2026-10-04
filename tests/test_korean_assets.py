"""Validate curriculum/audio/font integration without the ESP-IDF toolchain."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]

class KoreanAssets(unittest.TestCase):
    def test_audio_pack(self):
        course = json.loads((ROOT / 'main/korean/course.json').read_text())
        items = course['letters'] + course['words']
        pack = (ROOT / 'assets/music/korean_mms_16k.pcm').read_bytes()
        manifest = json.loads((ROOT / 'assets/music/korean_mms_manifest.json').read_text())
        self.assertEqual(len(items), 60)
        self.assertEqual(len(manifest['clips']), 60)
        self.assertEqual(hashlib.sha256(pack).hexdigest(), manifest['sha256'])
        offset = 0
        for item, clip in zip(items, manifest['clips']):
            with self.subTest(text=item[0]):
                self.assertEqual(clip['text'], item[3])
                self.assertEqual(clip['offset'], offset)
                n = clip['bytes']
                self.assertEqual(n % 2, 0)
                self.assertGreater(n, 3200)
                self.assertLess(n, 16000 * 2 * 10)
                samples = struct.unpack('<' + 'h' * (n // 2), pack[offset:offset + n])
                self.assertGreater(max(abs(x) for x in samples), 160)
                offset += n
        self.assertEqual(offset, len(pack))
        self.assertLess(len(pack), 2 * 1024 * 1024)

    def test_fixed_font_inventory(self):
        inventory = set((ROOT / 'assets/fonts/korean_symbols.txt').read_text())
        texts = (ROOT / 'main/korean_ui.c').read_text() + (ROOT / 'main/korean/course.json').read_text()
        required = {c for c in texts if ord(c) > 127 and c.isprintable()}
        self.assertFalse(required - inventory, 'new UI/course glyphs require font regeneration')
        for size in (16, 28):
            path = ROOT / ('assets/fonts/korean_font_%d.c' % size)
            self.assertIn('korean_font_%d' % size, path.read_text())

    def test_course_choices_are_unambiguous(self):
        course = json.loads((ROOT / 'main/korean/course.json').read_text())
        for key, count in [('letters', 24), ('words', 36)]:
            items = course[key]
            self.assertEqual(len(items), count)
            self.assertEqual(len({x[0] for x in items}), count)
            self.assertEqual(len({x[1] for x in items}), count)
            for row in items:
                self.assertEqual(len(row), 5)
                self.assertTrue(all(row))

if __name__ == '__main__':
    unittest.main()
