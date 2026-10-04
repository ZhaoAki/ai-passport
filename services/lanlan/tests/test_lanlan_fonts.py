"""Chinese glyph inventory for the mobile page (A09, web part).

The passport renders Chinese from a subset font; the web page does not embed a
font at all, so the requirement that matters here is that every character the
page can show is declared in one inventory file. The inventory is checked in as
codepoints, so adding a new visible string forces an explicit decision about
glyph coverage instead of silently relying on the platform font.

The device-side counterpart is ``tests/test_lanlan_fonts.py`` in the repository
root; this module keeps the web surface honest until the shared subset exists.
"""

from __future__ import annotations

import os
import re
import unittest
from typing import Dict, List, Set

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import LanlanTestCase


TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
INVENTORY_PATH = os.path.join(TESTS_DIR, "lanlan_ui_glyphs.txt")
# services/lanlan/tests -> services -> repository root
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(TESTS_DIR)))
WEB_DIR = os.path.join(REPO_ROOT, "web", "lanlan")

CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")
# Strings that must be present in the web surface, from the specification.
REQUIRED_STRINGS = (
    "未填写",
    "登录",
    "记录",
    "提醒",
    "关闭或查看提示不算完成照护",
    "数量",
    "发生时间",
    "执行人",
    "备注",
    "宠物资料",
    "账号与导出",
    "设备",
    "导出",
    "修改密码",
)


def web_text_files() -> List[str]:
    names = []
    for name in sorted(os.listdir(WEB_DIR)):
        if os.path.splitext(name)[1].lower() in (".html", ".css", ".js"):
            names.append(name)
    return names


def codepoints_in(text: str) -> Set[str]:
    return {"U+%04X" % ord(character) for character in CJK_RE.findall(text)}


def load_inventory() -> Dict[str, bool]:
    inventory: Dict[str, bool] = {}
    with open(INVENTORY_PATH, "r", encoding="ascii") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            fields = stripped.split()
            if len(fields) < 2:
                raise AssertionError("inventory line is malformed: %r" % line)
            if fields[0] in inventory:
                raise AssertionError("duplicate codepoint %s" % fields[0])
            inventory[fields[0]] = fields[1] == "1"
    return inventory


class GlyphInventoryTest(LanlanTestCase):
    start_server = False

    def test_inventory_file_exists_and_is_sorted(self) -> None:
        self.assertTrue(os.path.isfile(INVENTORY_PATH), INVENTORY_PATH)
        inventory = load_inventory()
        self.assertGreater(len(inventory), 50)
        codepoints = list(inventory)
        self.assertEqual(codepoints, sorted(codepoints))
        for codepoint, covered in inventory.items():
            with self.subTest(codepoint=codepoint):
                self.assertRegex(codepoint, r"^U\+[0-9A-F]{4,5}$")
                self.assertIsInstance(covered, bool)

    def test_every_visible_character_is_declared(self) -> None:
        inventory = load_inventory()
        used: Set[str] = set()
        for name in web_text_files():
            with open(os.path.join(WEB_DIR, name), "r", encoding="utf-8") as handle:
                used |= codepoints_in(handle.read())
        missing = sorted(used - set(inventory))
        self.assertEqual([], missing, "undeclared CJK codepoints in web/lanlan")

    def test_no_inventory_entry_is_marked_uncovered(self) -> None:
        inventory = load_inventory()
        uncovered = sorted(codepoint for codepoint, covered in inventory.items() if not covered)
        self.assertEqual([], uncovered, "a required glyph is marked as covered=0")

    def test_inventory_has_no_stale_entries(self) -> None:
        inventory = load_inventory()
        used: Set[str] = set()
        for name in web_text_files():
            with open(os.path.join(WEB_DIR, name), "r", encoding="utf-8") as handle:
                used |= codepoints_in(handle.read())
        stale = sorted(set(inventory) - used)
        self.assertEqual([], stale, "inventory declares glyphs the page no longer uses")

    def test_required_strings_are_present(self) -> None:
        combined = ""
        for name in web_text_files():
            with open(os.path.join(WEB_DIR, name), "r", encoding="utf-8") as handle:
                combined += handle.read()
        for required in REQUIRED_STRINGS:
            with self.subTest(required=required):
                self.assertIn(required, combined)

    def test_unknown_amount_label_is_the_declared_fallback(self) -> None:
        with open(os.path.join(WEB_DIR, "app.js"), "r", encoding="utf-8") as handle:
            script = handle.read()
        self.assertIn('var UNKNOWN = "未填写"', script)
        inventory = load_inventory()
        for character in "未填写":
            self.assertIn("U+%04X" % ord(character), inventory)


if __name__ == "__main__":
    unittest.main()
