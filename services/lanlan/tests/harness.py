"""Shared test helpers: a real HTTP server on 127.0.0.1 and a small client.

Every test drives the service through real HTTP requests against a temporary
database, which is what the acceptance criteria ask for.
"""

from __future__ import annotations

import http.client
import io
import json
import os
import shutil
import tempfile
import threading
import unittest
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import urlsplit

from lanlan import admin
from lanlan import db as db_module
from lanlan.config import Config
from lanlan.server import LanlanServer

CAREGIVER_PASSWORDS = {"hehe": "hehe-test-password", "yangyang": "yangyang-test-password"}


class HttpResponse:
    def __init__(self, status: int, headers: Any, body: bytes) -> None:
        self.status = status
        self.headers = headers
        self.body = body

    def json(self) -> Any:
        if not self.body:
            return None
        return json.loads(self.body.decode("utf-8"))

    def text(self) -> str:
        return self.body.decode("utf-8")


class Client:
    """A cookie-aware HTTP client; each instance keeps its own session."""

    def __init__(self, host: str, port: int, origin: Optional[str] = None) -> None:
        self.host = host
        self.port = port
        self.origin = origin or "http://%s:%d" % (host, port)
        self.cookie: Optional[str] = None
        self.csrf: Optional[str] = None
        self.device_token: Optional[str] = None

    def _connection(self) -> http.client.HTTPConnection:
        return http.client.HTTPConnection(self.host, self.port, timeout=10)

    def request(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        token: Optional[str] = None,
        headers: Optional[Dict[str, str]] = None,
        origin: Optional[str] = None,
        csrf: Optional[str] = None,
        raw_body: Optional[bytes] = None,
        send_origin: bool = True,
    ) -> HttpResponse:
        payload: Optional[bytes] = None
        request_headers: Dict[str, str] = {"Accept": "application/json"}
        if raw_body is not None:
            payload = raw_body
        elif body is not None:
            payload = json.dumps(body).encode("utf-8")
            request_headers["Content-Type"] = "application/json"
        if payload is not None:
            request_headers["Content-Length"] = str(len(payload))
        if send_origin:
            request_headers["Origin"] = self.origin if origin is None else origin
        if self.cookie:
            request_headers["Cookie"] = self.cookie
        write_method = method.upper() in ("POST", "PATCH", "PUT", "DELETE")
        if write_method:
            effective_csrf = csrf if csrf is not None else self.csrf
            if effective_csrf:
                request_headers["X-Lanlan-CSRF"] = effective_csrf
        bearer = token if token is not None else None
        if bearer:
            request_headers["Authorization"] = "Bearer %s" % bearer
        for name, value in (headers or {}).items():
            request_headers[name] = value

        connection = self._connection()
        try:
            connection.request(method.upper(), path, body=payload, headers=request_headers)
            response = connection.getresponse()
            response_body = response.read()
            set_cookie = response.getheader("Set-Cookie")
            if set_cookie and "lanlan_session=" in set_cookie:
                value = set_cookie.split(";")[0].split("=", 1)[1]
                self.cookie = "lanlan_session=%s" % value if value else None
            return HttpResponse(response.status, response, response_body)
        finally:
            connection.close()

    def get(self, path: str, **kwargs: Any) -> HttpResponse:
        return self.request("GET", path, **kwargs)

    def post(self, path: str, body: Any = None, **kwargs: Any) -> HttpResponse:
        return self.request("POST", path, body, **kwargs)

    def patch(self, path: str, body: Any = None, **kwargs: Any) -> HttpResponse:
        return self.request("PATCH", path, body, **kwargs)

    def login(self, username: str, password: Optional[str] = None) -> HttpResponse:
        response = self.post(
            "/api/v1/auth/login",
            {"username": username, "password": password or CAREGIVER_PASSWORDS[username]},
        )
        if response.status == 200:
            self.csrf = response.json()["csrf_token"]
        return response

    def login_ok(self, username: str, password: Optional[str] = None) -> Dict[str, Any]:
        response = self.login(username, password)
        if response.status != 200:
            raise AssertionError(
                "login for %s failed: %s %s" % (username, response.status, response.text())
            )
        return response.json()


class LanlanTestServer:
    """A running server bound to an ephemeral 127.0.0.1 port."""

    def __init__(
        self,
        directory: Optional[str] = None,
        db_name: str = "lanlan-test.sqlite3",
        web_dir: Optional[str] = None,
        initialize: bool = True,
    ) -> None:
        self._owns_directory = directory is None
        self.directory = directory or tempfile.mkdtemp(prefix="lanlan-tests-")
        self.db_path = os.path.join(self.directory, db_name)
        self.web_dir = web_dir
        self.config = Config(
            db_path=self.db_path,
            host="127.0.0.1",
            port=0,
            env="development",
            secure_cookies=False,
            pbkdf2_iterations=1000,
            session_days=30,
            db_path_explicit=True,
        )
        if initialize:
            self.init_database()
        self.server: Optional[LanlanServer] = None
        self.thread: Optional[threading.Thread] = None
        # The service still logs method, path, status and duration; tests keep
        # that output out of the unittest report.
        self.log_stream = io.StringIO()

    # -- lifecycle --------------------------------------------------------
    def init_database(
        self,
        family_name: str = "Test family",
        timezone_name: str = "Asia/Shanghai",
        passwords: Optional[Dict[str, str]] = None,
        force: bool = False,
        caregivers: Optional[List[Tuple[str, str]]] = None,
    ) -> Dict[str, Any]:
        connection = db_module.connect(self.db_path)
        try:
            db_module.initialize(connection)
            return admin.initialize_database(
                connection,
                self.config,
                family_name=family_name,
                timezone_name=timezone_name,
                passwords=dict(passwords or CAREGIVER_PASSWORDS),
                force=force,
                caregivers=caregivers,
            )
        finally:
            connection.close()

    def start(self) -> "LanlanTestServer":
        if self.server is not None:
            raise RuntimeError("server already running")
        self.server = LanlanServer(self.config, self.db_path, web_dir=self.web_dir)
        self.server.log_stream = self.log_stream
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05})
        self.thread.daemon = True
        self.thread.start()
        return self

    def stop(self) -> None:
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
            self.server = None
        if self.thread is not None:
            self.thread.join(timeout=5)
            self.thread = None

    def restart(self) -> "LanlanTestServer":
        self.stop()
        return self.start()

    def close(self) -> None:
        self.stop()
        if self._owns_directory:
            shutil.rmtree(self.directory, ignore_errors=True)

    @property
    def port(self) -> int:
        if self.server is None:
            raise RuntimeError("server is not running")
        return self.server.port

    @property
    def base_url(self) -> str:
        return "http://127.0.0.1:%d" % self.port

    # -- clients ----------------------------------------------------------
    def client(self) -> Client:
        return Client("127.0.0.1", self.port)

    def caregiver(self, username: str = "hehe") -> Client:
        client = self.client()
        client.login_ok(username)
        return client

    def caregiver_in_family(self, username: str, password: str) -> Client:
        client = self.client()
        client.login_ok(username, password)
        return client

    def device(self, label: str = "test-passport") -> Tuple[Client, str]:
        connection = db_module.connect(self.db_path)
        try:
            family_id = admin.ensure_family(connection)
            device_id, token = admin.create_device(connection, family_id, label)
        finally:
            connection.close()
        return self.client(), token

    # -- database helpers -------------------------------------------------
    def connect(self):
        return db_module.connect(self.db_path)

    def family_id(self) -> str:
        connection = self.connect()
        try:
            return admin.ensure_family(connection)
        finally:
            connection.close()

    def record_revision_count(self, record_id: str) -> int:
        connection = self.connect()
        try:
            row = connection.execute(
                "SELECT COUNT(*) AS n FROM records WHERE id=?", (record_id,)
            ).fetchone()
            return int(row["n"])
        finally:
            connection.close()

    def reminder_revision_count(self, reminder_id: str) -> int:
        connection = self.connect()
        try:
            row = connection.execute(
                "SELECT COUNT(*) AS n FROM reminders WHERE id=?", (reminder_id,)
            ).fetchone()
            return int(row["n"])
        finally:
            connection.close()


class LanlanTestCase(unittest.TestCase):
    """Base class that manages one server per test."""

    start_server = True
    server_kwargs: Dict[str, Any] = {}

    def setUp(self) -> None:
        self.server = LanlanTestServer(**self.server_kwargs)
        self.addCleanup(self.server.close)
        if self.start_server:
            self.server.start()

    # -- convenience ------------------------------------------------------
    def caregiver(self, username: str = "hehe") -> Client:
        return self.server.caregiver(username)

    def device_client(self, label: str = "test-passport") -> Tuple[Client, str]:
        return self.server.device(label)

    def create_record(self, client: Client, **fields: Any) -> Dict[str, Any]:
        payload = {"category": "meal"}
        payload.update(fields)
        response = client.post("/api/v1/records", payload)
        self.assertEqual(200, response.status, response.text())
        return response.json()["record"]

    def members(self, client: Client) -> Dict[str, str]:
        response = client.get("/api/v1/me")
        self.assertEqual(200, response.status, response.text())
        return {member["username"]: member["id"] for member in response.json()["members"]}


def origin_of(url: str) -> str:
    parts = urlsplit(url)
    return "%s://%s" % (parts.scheme, parts.netloc)


__all__ = [
    "CAREGIVER_PASSWORDS",
    "Client",
    "HttpResponse",
    "LanlanServer",
    "LanlanTestCase",
    "LanlanTestServer",
    "origin_of",
]
