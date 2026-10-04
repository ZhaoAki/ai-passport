"""Environment-driven configuration for the Lanlan service.

Every setting has a documented default; the environment variable names are
stable and are the only deployment interface. No secret material is stored in
the repository, so this module only ever reads from the process environment.
"""

from __future__ import annotations

import os
from typing import List, Mapping, Optional, Tuple

DEFAULT_DB = "./lanlan.sqlite3"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8787
DEFAULT_PBKDF2_ITERATIONS = 210000
DEFAULT_SESSION_DAYS = 30
DEFAULT_TIMEZONE = "Asia/Shanghai"
DEFAULT_FAMILY_NAME = "Lanlan family"

ENV_DEVELOPMENT = "development"
ENV_PRODUCTION = "production"
_VALID_ENVS = (ENV_DEVELOPMENT, ENV_PRODUCTION)

_TRUE_VALUES = ("1", "true", "yes", "on")
_FALSE_VALUES = ("0", "false", "no", "off", "")


class ConfigError(Exception):
    """Raised when the environment cannot produce a usable configuration."""


def _env_bool(env: Mapping[str, str], name: str, default: bool) -> bool:
    raw = env.get(name)
    if raw is None:
        return default
    value = raw.strip().lower()
    if value in _TRUE_VALUES:
        return True
    if value in _FALSE_VALUES:
        return False
    raise ConfigError("%s must be a boolean, got %r" % (name, raw))


def _env_int(env: Mapping[str, str], name: str, default: int, minimum: int = 1) -> int:
    raw = env.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = int(raw.strip())
    except ValueError:
        raise ConfigError("%s must be an integer, got %r" % (name, raw))
    if value < minimum:
        raise ConfigError("%s must be at least %d, got %d" % (name, minimum, value))
    return value


class Config:
    """Resolved runtime configuration."""

    def __init__(
        self,
        db_path: Optional[str] = None,
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
        env: str = ENV_DEVELOPMENT,
        secure_cookies: bool = False,
        web_dir: Optional[str] = None,
        pbkdf2_iterations: int = DEFAULT_PBKDF2_ITERATIONS,
        session_days: int = DEFAULT_SESSION_DAYS,
        timezone: str = DEFAULT_TIMEZONE,
        family_name: str = DEFAULT_FAMILY_NAME,
        db_path_explicit: bool = False,
        allow_insecure: bool = False,
    ) -> None:
        self.db_path = db_path if db_path is not None else DEFAULT_DB
        self.host = host
        self.port = port
        self.env = env
        self.secure_cookies = secure_cookies
        self.web_dir = web_dir
        self.pbkdf2_iterations = pbkdf2_iterations
        self.session_days = session_days
        self.timezone = timezone
        self.family_name = family_name
        self.db_path_explicit = db_path_explicit
        self.allow_insecure = allow_insecure

    @property
    def production(self) -> bool:
        return self.env == ENV_PRODUCTION

    @classmethod
    def from_env(cls, env: Optional[Mapping[str, str]] = None) -> "Config":
        source: Mapping[str, str] = os.environ if env is None else env

        raw_db = source.get("LANLAN_DB")
        db_explicit = raw_db is not None and raw_db.strip() != ""
        db_path = raw_db.strip() if db_explicit else DEFAULT_DB

        raw_env = source.get("LANLAN_ENV", ENV_DEVELOPMENT).strip().lower() or ENV_DEVELOPMENT
        if raw_env not in _VALID_ENVS:
            raise ConfigError(
                "LANLAN_ENV must be one of %s, got %r" % (", ".join(_VALID_ENVS), raw_env)
            )

        raw_host = source.get("LANLAN_HOST", DEFAULT_HOST).strip() or DEFAULT_HOST

        config = cls(
            db_path=db_path,
            host=raw_host,
            port=_env_int(source, "LANLAN_PORT", DEFAULT_PORT),
            env=raw_env,
            secure_cookies=_env_bool(source, "LANLAN_SECURE_COOKIES", raw_env == ENV_PRODUCTION),
            web_dir=source.get("LANLAN_WEB_DIR") or None,
            pbkdf2_iterations=_env_int(
                source, "LANLAN_PBKDF2_ITERATIONS", DEFAULT_PBKDF2_ITERATIONS, minimum=1
            ),
            session_days=_env_int(source, "LANLAN_SESSION_DAYS", DEFAULT_SESSION_DAYS),
            timezone=source.get("LANLAN_TIMEZONE", DEFAULT_TIMEZONE).strip() or DEFAULT_TIMEZONE,
            family_name=source.get("LANLAN_FAMILY_NAME", DEFAULT_FAMILY_NAME).strip()
            or DEFAULT_FAMILY_NAME,
            db_path_explicit=db_explicit,
            allow_insecure=_env_bool(source, "LANLAN_ALLOW_INSECURE", False),
        )
        return config

    def validate_for_serve(self) -> None:
        """Refuse unsafe production configurations.

        In production the database path must be explicit and cookies must be
        secure, unless the operator sets ``LANLAN_ALLOW_INSECURE=1`` for a
        deliberate reverse-proxy-terminated setup.
        """
        if not self.production:
            return
        problems = []
        if not self.db_path_explicit:
            problems.append("LANLAN_DB must be set explicitly in production")
        if not self.secure_cookies:
            problems.append("LANLAN_SECURE_COOKIES must be 1 in production")
        if problems and not self.allow_insecure:
            raise ConfigError(
                "refusing to start in production: "
                + "; ".join(problems)
                + " (set LANLAN_ALLOW_INSECURE=1 to override deliberately)"
            )

    def describe(self) -> str:
        """Human-readable summary that never contains secrets."""
        return "\n".join(
            [
                "LANLAN_DB=%s%s" % (self.db_path, "" if self.db_path_explicit else " (default)"),
                "LANLAN_HOST=%s" % self.host,
                "LANLAN_PORT=%d" % self.port,
                "LANLAN_ENV=%s" % self.env,
                "LANLAN_SECURE_COOKIES=%s" % ("1" if self.secure_cookies else "0"),
                "LANLAN_WEB_DIR=%s" % (self.web_dir or "(package default)"),
                "LANLAN_PBKDF2_ITERATIONS=%d" % self.pbkdf2_iterations,
                "LANLAN_SESSION_DAYS=%d" % self.session_days,
                "LANLAN_TIMEZONE=%s" % self.timezone,
                "LANLAN_FAMILY_NAME=%s" % self.family_name,
                "LANLAN_ALLOW_INSECURE=%s" % ("1" if self.allow_insecure else "0"),
            ]
        )


def check_config(env: Optional[Mapping[str, str]] = None) -> "Tuple[Config, str]":
    """Resolve the configuration and report any production-blocking problem.

    Returns ``(config, report)``; the report ends with ``OK`` or ``FAIL: ...``
    so the caller can pick the exit status without parsing the settings.
    """
    config = Config.from_env(env)
    lines = [config.describe()]
    problems: List[str] = []
    try:
        config.validate_for_serve()
    except ConfigError as error:
        problems.append(str(error))

    if config.db_path and config.db_path != ":memory:":
        directory = os.path.dirname(os.path.abspath(config.db_path)) or "."
        if not os.path.isdir(directory):
            problems.append("database directory does not exist: %s" % directory)
        elif not os.access(directory, os.W_OK):
            problems.append("database directory is not writable: %s" % directory)
    else:
        problems.append("LANLAN_DB must not be empty")

    if config.web_dir and not os.path.isdir(config.web_dir):
        problems.append("LANLAN_WEB_DIR is not a directory: %s" % config.web_dir)

    if problems:
        lines.extend("FAIL: %s" % problem for problem in problems)
        lines.append("check-config: FAIL")
    else:
        lines.append("check-config: OK")
    return config, "\n".join(lines)
