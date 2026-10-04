"""Web assets: every file is served, mobile metadata, no CDN or absolute hosts."""

from __future__ import annotations

import os
import re
import unittest
from typing import List, Set

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

from lanlan import server as server_module
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import LanlanTestCase


WEB_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
    "web",
    "lanlan",
)

CDN_PATTERN = re.compile(
    r"(?:src|href)\s*=\s*[\"']((?:https?:)?//[^\"']+)[\"']", re.IGNORECASE
)
ABSOLUTE_SCRIPT_PATTERN = re.compile(r"<script[^>]+src=[\"']/", re.IGNORECASE)
VIEWPORT_PATTERN = re.compile(
    r"<meta[^>]+name=[\"']viewport[\"'][^>]*>", re.IGNORECASE
)
URL_PATTERN = re.compile(r"url\(\s*[\"']?([^\"')]+)", re.IGNORECASE)


def web_files() -> List[str]:
    found: List[str] = []
    for root, dirs, files in os.walk(WEB_DIR):
        dirs[:] = [name for name in dirs if not name.startswith(".")]
        for name in sorted(files):
            if name.startswith("."):
                continue
            relative = os.path.relpath(os.path.join(root, name), WEB_DIR)
            found.append(relative.replace(os.sep, "/"))
    return sorted(found)


class WebAssetServingTest(LanlanTestCase):
    def test_web_directory_exists(self) -> None:
        self.assertTrue(os.path.isdir(WEB_DIR), WEB_DIR)
        self.assertIn("index.html", web_files())

    def test_every_asset_is_served(self) -> None:
        client = self.server.client()
        for relative in web_files():
            with self.subTest(asset=relative):
                response = client.get("/" + relative)
                self.assertEqual(200, response.status, "%s -> %s" % (relative, response.text()))
                self.assertTrue(response.body, relative)
                self.assertIn("charset=utf-8", response.headers.getheader("Content-Type") or "")

    def test_index_is_served_at_the_root(self) -> None:
        client = self.server.client()
        root = client.get("/")
        self.assertEqual(200, root.status, root.text())
        direct = client.get("/index.html")
        self.assertEqual(root.body, direct.body)

    def test_api_paths_win_over_static_files(self) -> None:
        client = self.server.client()
        response = client.get("/api/v1/records")
        self.assertEqual(401, response.status)
        self.assertIn("application/json", response.headers.getheader("Content-Type") or "")

    def test_unknown_static_path_is_404(self) -> None:
        client = self.server.client()
        self.assertEqual(404, client.get("/nope.html").status)

    def test_path_traversal_is_refused(self) -> None:
        client = self.server.client()
        for path in (
            "/../services/lanlan/config.py",
            "/..%2fservices%2flanlan%2fconfig.py",
            "/%2e%2e/%2e%2e/etc/passwd",
            "/assets/../../services/lanlan/api.py",
        ):
            with self.subTest(path=path):
                response = client.get(path)
                self.assertIn(response.status, (400, 404), response.text())
                self.assertNotIn(b"LANLAN_DB", response.body)
                self.assertNotIn(b"def ", response.body)

    def test_static_traversal_helper(self) -> None:
        self.assertIsNone(server_module.resolve_static_path(WEB_DIR, "/../secret.txt"))
        self.assertIsNone(server_module.resolve_static_path(WEB_DIR, "/etc/passwd"))
        self.assertIsNone(server_module.resolve_static_path(WEB_DIR, "/index.html/../../x"))
        self.assertIsNotNone(server_module.resolve_static_path(WEB_DIR, "/index.html"))
        self.assertIsNotNone(server_module.resolve_static_path(WEB_DIR, "/styles.css"))

    def test_symlink_escape_is_refused(self) -> None:
        link = os.path.join(WEB_DIR, "escape-link.html")
        created = False
        try:
            os.symlink(os.path.join(os.path.dirname(WEB_DIR), "..", "AGENTS.md"), link)
            created = True
        except (OSError, NotImplementedError):
            created = False
        if not created:
            self.skipTest("symlinks are not available here")
        try:
            self.assertIsNone(server_module.resolve_static_path(WEB_DIR, "/escape-link.html"))
            client = self.server.client()
            self.assertEqual(404, client.get("/escape-link.html").status)
        finally:
            os.remove(link)


class HtmlContractTest(LanlanTestCase):
    def read(self, name: str) -> str:
        with open(os.path.join(WEB_DIR, name), "r", encoding="utf-8") as handle:
            return handle.read()

    def test_viewport_meta_is_present(self) -> None:
        html = self.read("index.html")
        match = VIEWPORT_PATTERN.search(html)
        self.assertIsNotNone(match, "index.html needs a viewport meta tag")
        tag = match.group(0)
        self.assertIn("width=device-width", tag)
        self.assertIn("initial-scale=1", tag)

    def test_required_views_exist(self) -> None:
        html = self.read("index.html")
        for view in (
            "login", "overview", "record", "records", "detail", "reminders",
            "profile", "account",
        ):
            with self.subTest(view=view):
                self.assertIn('id="view-%s"' % view, html)

    def test_no_cdn_or_absolute_hosts(self) -> None:
        for name in web_files():
            text = self.read(name)
            for match in CDN_PATTERN.findall(text):
                self.fail("%s references an absolute URL: %s" % (name, match))
            for match in ABSOLUTE_SCRIPT_PATTERN.findall(text):
                self.fail("%s references an absolute script path: %s" % (name, match))
            for match in URL_PATTERN.findall(text):
                if match.startswith(("http://", "https://", "//", "data:")):
                    self.fail("%s loads external resource: %s" % (name, match))

    def test_asset_references_are_relative(self) -> None:
        html = self.read("index.html")
        self.assertIn('href="styles.css"', html)
        self.assertIn('src="app.js"', html)
        self.assertIn('href="api/v1/export/records.csv"', html)
        self.assertIn('href="api/v1/export/records.json"', html)

    def test_no_inline_secret_or_token(self) -> None:
        html = self.read("index.html")
        self.assertNotIn("Bearer ", html)
        self.assertNotIn("token=", html)
        self.assertIsNone(re.search(r"lanlan_session\s*=", html))

    def test_chinese_labels_are_present(self) -> None:
        html = self.read("index.html")
        for label in ("澜澜记录", "登录", "记录", "提醒", "宠物资料", "账号与导出"):
            self.assertIn(label, html)

    def test_dismissal_note_is_explicit(self) -> None:
        html = self.read("index.html")
        self.assertIn("不算完成", html)
        self.assertIn("提交一笔记录", html)


class JavascriptContractTest(LanlanTestCase):
    def read(self, name: str) -> str:
        with open(os.path.join(WEB_DIR, name), "r", encoding="utf-8") as handle:
            return handle.read()

    def test_client_request_id_is_generated_and_reused(self) -> None:
        script = self.read("app.js")
        self.assertIn("client_request_id", script)
        self.assertIn("randomUUID", script)
        # The identifier belongs to the form instance, not the request.
        self.assertIn("state.formId = newRequestId()", script)
        self.assertIn("body.client_request_id = state.formId", script)

    def test_failed_submit_keeps_values_and_offers_retry(self) -> None:
        script = self.read("app.js")
        self.assertIn("recordRetry", script)
        self.assertIn("已保留填写内容", script)
        self.assertIn("state.formDirty = true", script)

    def test_success_is_only_shown_after_the_server_answers(self) -> None:
        script = self.read("app.js")
        self.assertIn('api(method, path, body).then(function (result)', script)
        self.assertIn("if (!result.ok || !result.data)", script)
        self.assertIn("重置", script.replace("resetRecordForm", "重置"))

    def test_unknown_values_render_as_unfilled(self) -> None:
        script = self.read("app.js")
        self.assertIn('var UNKNOWN = "未填写"', script)
        self.assertNotIn('"null"', script)
        self.assertNotIn('"undefined"', script)
        self.assertNotIn("isNaN", script)
        # Every formatting helper falls back to the explicit unknown label.
        self.assertGreaterEqual(script.count("UNKNOWN"), 8)

    def test_no_null_undefined_or_nan_literal_is_written_to_the_page(self) -> None:
        script = self.read("app.js")
        for pattern in (r"textContent\s*=\s*null", r"textContent\s*=\s*undefined"):
            self.assertIsNone(re.search(pattern, script))
        self.assertNotIn("String(null)", script)
        self.assertNotIn("String(undefined)", script)

    def test_conflict_is_visible(self) -> None:
        script = self.read("app.js")
        self.assertIn("result.status === 409", script)
        self.assertIn("服务器上的版本更新", script)
        self.assertIn("recordConflict", script)

    def test_only_relative_requests(self) -> None:
        script = self.read("app.js")
        for match in re.findall(r"(?:fetch|api)\(\s*[\"']([^\"']+)", script):
            self.assertFalse(match.startswith(("http://", "https://", "//")), match)
        self.assertIn('"api/v1/', script)

    def test_scripts_do_not_reference_unknown_element_ids(self) -> None:
        script = self.read("app.js")
        html = self.read("index.html")
        referenced: Set[str] = set(re.findall(r"byId\(\s*[\"']([^\"']+)[\"']\s*\)", script))
        self.assertTrue(referenced)
        missing = sorted(name for name in referenced if 'id="%s"' % name not in html)
        self.assertEqual([], missing, "app.js references ids missing from index.html")

    def test_brace_balance_is_sane(self) -> None:
        script = self.read("app.js")
        self.assertEqual(script.count("{"), script.count("}"))
        self.assertEqual(script.count("("), script.count(")"))
        self.assertEqual(script.count("["), script.count("]"))


class CssContractTest(LanlanTestCase):
    def read(self, name: str) -> str:
        with open(os.path.join(WEB_DIR, name), "r", encoding="utf-8") as handle:
            return handle.read()

    def test_mobile_first_touch_targets(self) -> None:
        css = self.read("styles.css")
        self.assertIn("min-height: 48px", css)
        self.assertIn("button.big", css)
        self.assertIn("safe-area-inset-bottom", css)

    def test_no_external_font_or_image(self) -> None:
        css = self.read("styles.css")
        self.assertNotIn("@import", css)
        for match in URL_PATTERN.findall(css):
            self.assertFalse(match.startswith(("http://", "https://", "//")), match)


if __name__ == "__main__":
    unittest.main()
