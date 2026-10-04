"""Path and environment bootstrap shared by the service tests.

Import this module first in every test module: unittest may load a test module
before the ``tests`` package body runs, so the bootstrap cannot live in
``__init__.py`` alone.
"""

from __future__ import annotations

import os
import sys

# Keep password hashing cheap inside the suite without touching the code under
# test. ``setdefault`` lets a developer raise it deliberately.
os.environ.setdefault("LANLAN_PBKDF2_ITERATIONS", "1000")
os.environ.setdefault("LANLAN_ENV", "development")

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))


def _services_dir(start: str) -> str:
    """Find the directory that holds the ``lanlan`` package.

    Unittest discovery may be started from the repository root, where the
    package is ``services.lanlan``, or from ``services``, where it is
    ``lanlan``. Walking up until the package directory is found works for both.
    """
    current = start
    for _ in range(6):
        if os.path.isfile(os.path.join(current, "lanlan", "__init__.py")):
            return current
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    return os.path.dirname(os.path.dirname(start))


_SERVICES_DIR = _services_dir(_TESTS_DIR)
if _SERVICES_DIR not in sys.path:
    sys.path.insert(0, _SERVICES_DIR)
