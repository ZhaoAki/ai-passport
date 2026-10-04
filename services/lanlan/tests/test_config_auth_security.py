"""Configuration, password hashing and session handling.

These checks are cheap and catch the operational rules the specification states
explicitly: production needs an explicit database path and secure cookies,
passwords use PBKDF2-HMAC-SHA256 with a per-user salt, and sessions expire.
"""

from __future__ import annotations

import unittest

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

from lanlan import auth, model
from lanlan.config import (
    Config,
    ConfigError,
    DEFAULT_PBKDF2_ITERATIONS,
    DEFAULT_PORT,
    ENV_PRODUCTION,
    check_config,
)
from services.lanlan.tests.harness import CAREGIVER_PASSWORDS, LanlanTestCase


class ConfigDefaultsTest(unittest.TestCase):
    def test_documented_defaults(self) -> None:
        config = Config.from_env({})
        self.assertEqual("./lanlan.sqlite3", config.db_path)
        self.assertEqual("127.0.0.1", config.host)
        self.assertEqual(DEFAULT_PORT, config.port)
        self.assertEqual("development", config.env)
        self.assertFalse(config.secure_cookies)
        self.assertEqual(DEFAULT_PBKDF2_ITERATIONS, config.pbkdf2_iterations)
        self.assertEqual(210000, config.pbkdf2_iterations)
        self.assertEqual(30, config.session_days)
        self.assertEqual("Asia/Shanghai", config.timezone)
        self.assertFalse(config.db_path_explicit)

    def test_environment_overrides(self) -> None:
        config = Config.from_env(
            {
                "LANLAN_DB": "/tmp/example.sqlite3",
                "LANLAN_HOST": "0.0.0.0",
                "LANLAN_PORT": "9000",
                "LANLAN_ENV": "production",
                "LANLAN_SECURE_COOKIES": "1",
                "LANLAN_PBKDF2_ITERATIONS": "1000",
                "LANLAN_SESSION_DAYS": "7",
                "LANLAN_TIMEZONE": "Europe/Berlin",
                "LANLAN_FAMILY_NAME": "Example family",
            }
        )
        self.assertEqual("/tmp/example.sqlite3", config.db_path)
        self.assertTrue(config.db_path_explicit)
        self.assertEqual("0.0.0.0", config.host)
        self.assertEqual(9000, config.port)
        self.assertTrue(config.production)
        self.assertTrue(config.secure_cookies)
        self.assertEqual(1000, config.pbkdf2_iterations)
        self.assertEqual(7, config.session_days)
        config.validate_for_serve()  # must not raise

    def test_invalid_values_are_rejected(self) -> None:
        for env in (
            {"LANLAN_PORT": "0"},
            {"LANLAN_PORT": "not-a-number"},
            {"LANLAN_ENV": "staging"},
            {"LANLAN_SECURE_COOKIES": "maybe"},
            {"LANLAN_PBKDF2_ITERATIONS": "0"},
        ):
            with self.subTest(env=env):
                with self.assertRaises(ConfigError):
                    Config.from_env(env)

    def test_production_refuses_unsafe_configuration(self) -> None:
        # A production default turns cookies secure, so the missing explicit
        # database path is the first refusal.
        default_secure = Config.from_env({"LANLAN_ENV": ENV_PRODUCTION})
        self.assertTrue(default_secure.secure_cookies)
        with self.assertRaises(ConfigError) as caught:
            default_secure.validate_for_serve()
        self.assertIn("LANLAN_DB", str(caught.exception))

        # Explicitly disabling secure cookies is refused as well.
        insecure = Config.from_env(
            {"LANLAN_ENV": ENV_PRODUCTION, "LANLAN_SECURE_COOKIES": "0"}
        )
        self.assertFalse(insecure.secure_cookies)
        with self.assertRaises(ConfigError) as caught:
            insecure.validate_for_serve()
        self.assertIn("LANLAN_SECURE_COOKIES", str(caught.exception))

        # Only the deliberate override starts it.
        overridden = Config.from_env(
            {"LANLAN_ENV": ENV_PRODUCTION, "LANLAN_ALLOW_INSECURE": "1"}
        )
        overridden.validate_for_serve()

        # A complete production configuration also starts.
        complete = Config.from_env(
            {
                "LANLAN_ENV": ENV_PRODUCTION,
                "LANLAN_DB": "/tmp/example.sqlite3",
                "LANLAN_SECURE_COOKIES": "1",
            }
        )
        complete.validate_for_serve()

    def test_check_config_reports_ok_and_failure(self) -> None:
        _, report = check_config({})
        self.assertIn("check-config: OK", report)
        self.assertIn("LANLAN_DB=./lanlan.sqlite3 (default)", report)

        _, failing = check_config({"LANLAN_ENV": "production"})
        self.assertIn("check-config: FAIL", failing)


class PasswordHashingTest(unittest.TestCase):
    def test_pbkdf2_sha256_format_and_verification(self) -> None:
        stored = auth.hash_password("example-password", 1000)
        parts = stored.split("$")
        self.assertEqual(4, len(parts))
        self.assertEqual("pbkdf2_sha256", parts[0])
        self.assertEqual("1000", parts[1])
        self.assertEqual(32, len(parts[2]))  # 16-byte salt as hex
        self.assertEqual(64, len(parts[3]))  # 32-byte digest as hex
        self.assertTrue(auth.verify_password(stored, "example-password"))
        self.assertFalse(auth.verify_password(stored, "example-password "))
        self.assertFalse(auth.verify_password(stored, "EXAMPLE-PASSWORD"))
        self.assertFalse(auth.verify_password("garbage", "example-password"))
        self.assertFalse(auth.verify_password("pbkdf2_sha256$abc$1$2", "example-password"))

    def test_two_hashes_of_the_same_password_differ(self) -> None:
        self.assertNotEqual(
            auth.hash_password("example-password", 1000),
            auth.hash_password("example-password", 1000),
        )

    def test_password_strength_rules(self) -> None:
        with self.assertRaises(model.FieldError):
            auth.validate_password_strength("short")
        auth.validate_password_strength("long-enough-password")

    def test_token_hash_is_stable_and_irreversible(self) -> None:
        token = auth.new_token()
        self.assertNotEqual(token, auth.token_hash(token))
        self.assertEqual(auth.token_hash(token), auth.token_hash(token))
        self.assertEqual(64, len(auth.token_hash(token)))

    def test_login_throttle_backoff_grows(self) -> None:
        from lanlan import db as db_module

        connection = db_module.connect(":memory:")
        try:
            db_module.initialize(connection)
            key = auth.throttle_key("127.0.0.1", "hehe")
            self.assertIsNone(auth.throttled_for(connection, key))
            self.assertIsNone(auth.record_login_failure(connection, key))
            self.assertIsNone(auth.record_login_failure(connection, key))
            self.assertIsNone(auth.record_login_failure(connection, key))
            self.assertIsNotNone(auth.record_login_failure(connection, key))
            self.assertIsNotNone(auth.throttled_for(connection, key))
            auth.clear_login_failures(connection, key)
            self.assertIsNone(auth.throttled_for(connection, key))
        finally:
            connection.close()


class SessionBehaviourTest(LanlanTestCase):
    def test_session_cookie_attributes(self) -> None:
        client = self.server.client()
        response = client.login("hehe")
        self.assertEqual(200, response.status, response.text())
        cookie = response.headers.getheader("Set-Cookie") or ""
        self.assertIn("lanlan_session=", cookie)
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Lax", cookie)
        self.assertIn("Path=/", cookie)
        self.assertNotIn("Secure", cookie)  # development over plain HTTP

    def test_expired_session_is_rejected(self) -> None:
        client = self.caregiver("hehe")
        self.assertEqual(200, client.get("/api/v1/me").status)
        connection = self.server.connect()
        try:
            connection.execute(
                "UPDATE sessions SET expires_at='2000-01-01T00:00:00Z'"
            )
        finally:
            connection.close()
        self.assertEqual(401, client.get("/api/v1/me").status)

    def test_logout_invalidates_the_session(self) -> None:
        client = self.caregiver("hehe")
        self.assertEqual(200, client.post("/api/v1/auth/logout", {}).status)
        # The client keeps the old cookie value on purpose here.
        self.assertEqual(401, client.get("/api/v1/me").status)

    def test_me_lists_both_caregivers(self) -> None:
        client = self.caregiver("hehe")
        payload = client.get("/api/v1/me").json()
        self.assertEqual(["hehe", "yangyang"], [m["username"] for m in payload["members"]])
        self.assertEqual("user", payload["role"])
        self.assertEqual(2, len(payload["members"]))

    def test_change_password_requires_the_current_one(self) -> None:
        client = self.caregiver("hehe")
        wrong = client.post(
            "/api/v1/auth/change-password",
            {"current_password": "not-it", "new_password": "a-new-password"},
        )
        self.assertEqual(401, wrong.status, wrong.text())
        short = client.post(
            "/api/v1/auth/change-password",
            {"current_password": CAREGIVER_PASSWORDS["hehe"], "new_password": "short"},
        )
        self.assertEqual(422, short.status, short.text())
        ok = client.post(
            "/api/v1/auth/change-password",
            {"current_password": CAREGIVER_PASSWORDS["hehe"], "new_password": "a-new-password"},
        )
        self.assertEqual(200, ok.status, ok.text())

        fresh = self.server.client()
        self.assertEqual(401, fresh.login("hehe", CAREGIVER_PASSWORDS["hehe"]).status)
        self.assertEqual(200, fresh.login("hehe", "a-new-password").status)

    def test_login_returns_family_and_csrf(self) -> None:
        payload = self.server.client().login_ok("yangyang")
        self.assertEqual("Test family", payload["family"]["name"])
        self.assertEqual("Asia/Shanghai", payload["family"]["timezone"])
        self.assertEqual("yangyang", payload["user"]["username"])
        self.assertTrue(payload["csrf_token"])
        self.assertNotIn("password", payload["user"])


class ProfileTest(LanlanTestCase):
    def test_profile_round_trip_and_validation(self) -> None:
        client = self.caregiver("hehe")
        empty = client.get("/api/v1/profile").json()["profile"]
        self.assertIsNone(empty["pet_name"])
        saved = client.patch(
            "/api/v1/profile",
            {
                "pet_name": "澜澜",
                "birthday": "2024-05-01",
                "breed": "狸花猫",
                "weight_grams": 4200,
                "notes": "示例资料，不是真实数据。",
            },
        )
        self.assertEqual(200, saved.status, saved.text())
        profile = saved.json()["profile"]
        self.assertEqual("澜澜", profile["pet_name"])
        self.assertEqual("2024-05-01", profile["birthday"])
        self.assertEqual(4200, profile["weight_grams"])

        bad_date = client.patch("/api/v1/profile", {"birthday": "2024/05/01"})
        self.assertEqual(422, bad_date.status, bad_date.text())
        bad_weight = client.patch("/api/v1/profile", {"weight_grams": 0})
        self.assertEqual(422, bad_weight.status, bad_weight.text())
        too_long = client.patch("/api/v1/profile", {"notes": "x" * 201})
        self.assertEqual(422, too_long.status, too_long.text())


if __name__ == "__main__":
    unittest.main()
