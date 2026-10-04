"""HTTP server and static-file host for the Lanlan service.

``ThreadingHTTPServer`` gives one thread per connection; every request opens its
own SQLite connection, so threads never share a connection and a write request
holds exactly one transaction.

Static files are served from ``web/lanlan`` with explicit traversal checks: dot
segments, absolute paths, NUL bytes and symlinks that escape the root are all
rejected before the file is opened.
"""

from __future__ import annotations

import http.server
import json
import os
import posixpath
import socketserver
import sys
import threading
import time
from typing import Any, Dict, Optional, Tuple
from urllib.parse import unquote, urlsplit

from . import api as api_module
from . import db as db_module
from .config import Config

STATIC_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".webmanifest": "application/manifest+json",
    ".txt": "text/plain; charset=utf-8",
}

DEFAULT_WEB_SUBDIR = os.path.join("web", "lanlan")

#: Where the privacy-safe request log goes. Tests point this at a buffer.
DEFAULT_LOG_STREAM = sys.stderr

SECURITY_HEADERS: Tuple[Tuple[str, str], ...] = (
    ("X-Content-Type-Options", "nosniff"),
    ("Referrer-Policy", "same-origin"),
    ("X-Frame-Options", "DENY"),
    ("Cache-Control", "no-store"),
)


def default_web_dir() -> str:
    """Locate ``web/lanlan`` relative to this checkout or an installed copy."""
    package_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(package_dir, DEFAULT_WEB_SUBDIR),
        os.path.join(os.path.dirname(os.path.dirname(package_dir)), DEFAULT_WEB_SUBDIR),
        os.path.join(os.getcwd(), DEFAULT_WEB_SUBDIR),
    ]
    for candidate in candidates:
        if os.path.isdir(candidate):
            return os.path.abspath(candidate)
    return os.path.abspath(candidates[0])


def resolve_static_path(root: str, url_path: str) -> Optional[str]:
    """Map a URL path to a file inside ``root``.

    Returns ``None`` when the request must be refused: encoded separators,
    ``..`` segments, absolute paths, NUL bytes, or a symlink whose target
    leaves the root.
    """
    if "\x00" in url_path:
        return None
    decoded = unquote(url_path)
    if "\x00" in decoded or "\\" in decoded:
        return None
    if decoded.startswith("/"):
        decoded = decoded[1:]
    if not decoded or decoded.endswith("/"):
        decoded = decoded + "index.html"
    parts = []
    for part in decoded.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            return None
        parts.append(part)
    if not parts:
        return None
    normalised = posixpath.join(*parts)
    if normalised.startswith("/") or normalised.startswith("../"):
        return None
    root_real = os.path.realpath(root)
    candidate = os.path.realpath(os.path.join(root_real, *parts))
    if candidate != root_real and not candidate.startswith(root_real + os.sep):
        return None
    if not os.path.isfile(candidate):
        return None
    return candidate


class LanlanHandler(http.server.BaseHTTPRequestHandler):
    server_version = "LanlanService/1.0"
    protocol_version = "HTTP/1.1"
    sys_version = ""

    # -- BaseHTTPRequestHandler hooks -------------------------------------
    def log_message(self, fmt: str, *args: Any) -> None:  # noqa: A003 - stdlib name
        """Suppress the stdlib access log; we emit our own privacy-safe line."""

    def log_error(self, fmt: str, *args: Any) -> None:  # noqa: A003 - stdlib name
        """Suppress the stdlib error log as well."""

    def do_GET(self) -> None:  # noqa: N802 - stdlib name
        self._serve("GET")

    def do_POST(self) -> None:  # noqa: N802 - stdlib name
        self._serve("POST")

    def do_PATCH(self) -> None:  # noqa: N802 - stdlib name
        self._serve("PATCH")

    def do_PUT(self) -> None:  # noqa: N802 - stdlib name
        self._serve("PUT")

    def do_DELETE(self) -> None:  # noqa: N802 - stdlib name
        self._serve("DELETE")

    def do_HEAD(self) -> None:  # noqa: N802 - stdlib name
        self._serve("HEAD")

    def do_OPTIONS(self) -> None:  # noqa: N802 - stdlib name
        self._serve("OPTIONS")

    # -- request handling --------------------------------------------------
    def _serve(self, method: str) -> None:
        started = time.monotonic()
        status = 500
        path_for_log = "-"
        try:
            parsed = urlsplit(self.path)
            path_for_log = parsed.path
            body, too_large = self._read_body()
            if too_large:
                status = 413
                self._send_json(
                    413,
                    {"error": {"code": "payload_too_large", "message": "request body is too large"}},
                )
                return
            if parsed.path.startswith("/api/v1"):
                status = self._handle_api(method, path_for_log, body)
            else:
                status = self._handle_static(method, path_for_log)
        except BrokenPipeError:
            status = 499
        except Exception as error:  # pragma: no cover - last-resort guard
            status = 500
            try:
                self._send_json(
                    500,
                    {"error": {"code": "internal", "message": "internal server error"}},
                )
            except Exception:
                pass
            self._log_line(method, path_for_log, status, started, detail=type(error).__name__)
            return
        finally:
            self._log_line(method, path_for_log, status, started)

    def _read_body(self) -> Tuple[bytes, bool]:
        raw_length = self.headers.get("Content-Length")
        if raw_length:
            try:
                length = int(raw_length)
            except ValueError:
                return b"", False
            if length < 0:
                return b"", False
            maximum = self.server.max_body_bytes  # type: ignore[attr-defined]
            if length > maximum:
                return b"", True
            return self.rfile.read(length), False
        if (self.headers.get("Transfer-Encoding") or "").lower() == "chunked":
            chunks = []
            total = 0
            maximum = self.server.max_body_bytes  # type: ignore[attr-defined]
            while True:
                line = self.rfile.readline(65536).strip()
                try:
                    size = int(line.split(b";")[0], 16)
                except ValueError:
                    break
                if size == 0:
                    self.rfile.readline(65536)
                    break
                total += size
                if total > maximum:
                    return b"", True
                chunks.append(self.rfile.read(size))
                self.rfile.readline(65536)
            return b"".join(chunks), False
        return b"", False

    def _handle_api(self, method: str, path: str, body: bytes) -> int:
        server = self.server  # type: ignore[assignment]
        connection = db_module.connect(server.db_path)  # type: ignore[attr-defined]
        try:
            request = api_module.Request(
                method=method,
                target=self.path,
                headers=self.headers,
                body=body,
                source_address=self.client_address[0] if self.client_address else "",
            )
            writes = method in ("POST", "PATCH", "PUT", "DELETE")
            if writes:
                with db_module.transaction(connection):
                    response = server.api.handle(request, connection)  # type: ignore[attr-defined]
            elif path.endswith("/sync/changes") or path.endswith("/sync/snapshot"):
                with db_module.read_transaction(connection):
                    response = server.api.handle(request, connection)  # type: ignore[attr-defined]
            else:
                response = server.api.handle(request, connection)  # type: ignore[attr-defined]
            self._send(response.status, response.body, response.content_type, response.headers)
            return response.status
        finally:
            connection.close()

    def _handle_static(self, method: str, path: str) -> int:
        server = self.server  # type: ignore[assignment]
        if method not in ("GET", "HEAD"):
            self._send_json(405, {"error": {"code": "method_not_allowed", "message": method}})
            return 405
        web_dir = server.web_dir  # type: ignore[attr-defined]
        if not web_dir or not os.path.isdir(web_dir):
            self._send_json(404, {"error": {"code": "not_found", "message": "no web assets installed"}})
            return 404
        resolved = resolve_static_path(web_dir, path)
        if resolved is None:
            self._send_json(404, {"error": {"code": "not_found", "message": "no such file"}})
            return 404
        extension = os.path.splitext(resolved)[1].lower()
        content_type = STATIC_CONTENT_TYPES.get(extension, "application/octet-stream")
        with open(resolved, "rb") as handle:
            payload = handle.read()
        headers = [("Cache-Control", "no-cache")]
        if method == "HEAD":
            payload = b""
        self._send(200, payload, content_type, headers)
        return 200

    def _send_json(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8", ())

    def _send(
        self,
        status: int,
        body: bytes,
        content_type: str,
        extra_headers: Any,
    ) -> None:
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            for name, value in SECURITY_HEADERS:
                self.send_header(name, value)
            for name, value in extra_headers or ():
                self.send_header(name, value)
            self.end_headers()
            if self.command != "HEAD" and body:
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            raise BrokenPipeError("client went away")

    def _log_line(
        self, method: str, path: str, status: int, started: float, detail: Optional[str] = None
    ) -> None:
        """Log method, query-free path, status and duration only.

        Record content, request bodies, credentials, tokens and query strings are
        never logged.
        """
        duration_ms = int((time.monotonic() - started) * 1000)
        suffix = " detail=%s" % detail if detail else ""
        stream = getattr(self.server, "log_stream", DEFAULT_LOG_STREAM)
        if stream is None:
            return
        stream.write("%s %s %s %dms%s\n" % (method, path, status, duration_ms, suffix))
        try:
            stream.flush()
        except (ValueError, OSError):
            pass


class LanlanServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """Threaded HTTP server carrying the resolved configuration."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, config: Config, db_path: str, web_dir: Optional[str] = None) -> None:
        self.config = config
        self.db_path = db_path
        # Create or migrate the schema before the first request. Embedders that
        # construct the server directly (tests, future entry points) get the
        # same upgrade path as `lanlan serve`.
        connection = db_module.connect(db_path)
        try:
            db_module.initialize(connection)
        finally:
            connection.close()
        self.web_dir = web_dir if web_dir is not None else default_web_dir()
        self.api = api_module.Api(config)
        self.log_stream = DEFAULT_LOG_STREAM
        self.max_body_bytes = api_module.MAX_BODY_BYTES
        self._ready = threading.Event()
        super().__init__((config.host, config.port), LanlanHandler)

    @property
    def port(self) -> int:
        return int(self.server_address[1])

    @property
    def base_url(self) -> str:
        host = self.config.host
        if ":" in host:
            host = "[%s]" % host
        return "http://%s:%d" % (host, self.port)


def create_server(config: Config, db_path: Optional[str] = None) -> LanlanServer:
    resolved = db_module.resolve_db_path(db_path or config.db_path)
    connection = db_module.connect(resolved)
    try:
        db_module.initialize(connection)
    finally:
        connection.close()
    return LanlanServer(config, resolved, web_dir=config.web_dir)


def serve_forever(config: Config, db_path: Optional[str] = None) -> int:
    config.validate_for_serve()
    server = create_server(config, db_path)
    sys.stderr.write(
        "lanlan serving on %s (env=%s, db=%s)\n"
        % (server.base_url, config.env, server.db_path)
    )
    sys.stderr.flush()
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        sys.stderr.write("lanlan stopping\n")
    finally:
        server.server_close()
    return 0
