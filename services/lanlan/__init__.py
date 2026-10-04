"""Cyber Lanlan care-recording service (standard library only).

The package implements the service described in
``docs/applications/cyber-lanlan-service.md``:

* SQLite persistence with append-only revision logs for records and reminders,
* caregiver sessions with CSRF protection and a separate read-only device
  credential, and
* a small JSON API plus an incremental sync protocol for the passport.

Nothing here uses third-party packages, so the package runs on any CPython
3.9 through 3.13 installation.
"""

from __future__ import annotations

__all__ = ["__version__"]

__version__ = "1.0.0"
