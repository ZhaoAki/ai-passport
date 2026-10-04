"""Persistence: data survives a real process restart against the same file (A05)."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from typing import Any

# Import-path bootstrap; see services/lanlan/tests/bootstrap.py.
try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests import bootstrap  # noqa: F401  (sys.path + fast PBKDF2)
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests import bootstrap  # noqa: F401

try:  # discovery can start at the repository root or at services/
    from services.lanlan.tests.harness import CAREGIVER_PASSWORDS, LanlanTestCase
except ImportError:  # pragma: no cover - direct run from services/
    from lanlan.tests.harness import CAREGIVER_PASSWORDS, LanlanTestCase

REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
SERVICES_DIR = os.path.join(REPO_ROOT, "services")


class RestartPersistenceTest(LanlanTestCase):
    def test_records_survive_a_server_restart(self) -> None:
        client = self.caregiver("hehe")
        record = self.create_record(
            client, category="meal", amount_value=88.0, amount_unit="g", note="重启前"
        )
        edited = client.patch(
            "/api/v1/records/%s" % record["id"],
            {"expected_version": 1, "category": "meal", "amount_value": 99.0,
             "amount_unit": "g", "note": "重启前（改）"},
        )
        self.assertEqual(200, edited.status, edited.text())
        doomed = self.create_record(client, category="water")
        client.post("/api/v1/records/%s/revoke" % doomed["id"], {"expected_version": 1})
        reminder = client.get("/api/v1/reminders").json()["reminders"][0]
        self.assertEqual(
            200,
            client.patch(
                "/api/v1/reminders/%s" % reminder["id"],
                {"enabled": True, "time_local": "06:40"},
            ).status,
        )
        device_created = client.post("/api/v1/devices", {"label": "persistent-passport"}).json()

        self.server.restart()

        fresh = self.server.client()
        logged_in = fresh.login("hehe", CAREGIVER_PASSWORDS["hehe"])
        self.assertEqual(200, logged_in.status, logged_in.text())

        detail = fresh.get("/api/v1/records/%s" % record["id"]).json()
        self.assertEqual(2, detail["record"]["version"])
        self.assertEqual(99.0, detail["record"]["amount_value"])
        self.assertEqual("重启前（改）", detail["record"]["note"])
        self.assertEqual([2, 1], [row["version"] for row in detail["revisions"]])
        self.assertEqual(1, detail["revisions"][1]["version"])

        revoked = fresh.get("/api/v1/records/%s" % doomed["id"]).json()["record"]
        self.assertEqual("revoked", revoked["status"])

        reminders = {
            row["id"]: row for row in fresh.get("/api/v1/reminders").json()["reminders"]
        }
        self.assertTrue(reminders[reminder["id"]]["enabled"])
        self.assertEqual("06:40", reminders[reminder["id"]]["time_local"])

        devices = fresh.get("/api/v1/devices").json()["devices"]
        self.assertIn(device_created["device"]["id"], [row["id"] for row in devices])

        # The old device token still works after the restart.
        device_client = self.server.client()
        sync = device_client.get(
            "/api/v1/sync/changes?cursor=0", token=device_created["token"]
        )
        self.assertEqual(200, sync.status, sync.text())
        self.assertGreaterEqual(len(sync.json()["records"]), 2)

    def test_summary_after_restart_matches_before(self) -> None:
        client = self.caregiver("hehe")
        for index in range(3):
            self.create_record(client, category="meal", amount_value=10.0, amount_unit="g")
        before = client.get("/api/v1/summary/today").json()
        self.server.restart()
        again = self.server.caregiver("hehe").get("/api/v1/summary/today").json()
        self.assertEqual(before["counts"], again["counts"])
        self.assertEqual(before["amounts_by_unit"], again["amounts_by_unit"])


class ProcessRestartTest(unittest.TestCase):
    """Restart the service in a separate process, as an operator would."""

    def setUp(self) -> None:
        self.directory = tempfile.mkdtemp(prefix="lanlan-process-tests-")
        self.addCleanup(self._cleanup)
        self.db_path = os.path.join(self.directory, "lanlan.sqlite3")
        self.env = dict(os.environ)
        self.env.update(
            {
                "LANLAN_DB": self.db_path,
                "LANLAN_PBKDF2_ITERATIONS": "1000",
                "LANLAN_HOST": "127.0.0.1",
                "PYTHONDONTWRITEBYTECODE": "1",
                "PYTHONPATH": SERVICES_DIR,
            }
        )

    def _cleanup(self) -> None:
        import shutil

        shutil.rmtree(self.directory, ignore_errors=True)

    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-m", "lanlan", *args],
            cwd=SERVICES_DIR,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_cli_init_then_device_list_uses_the_same_database(self) -> None:
        initialized = self.run_cli(
            "init", "--password", "hehe=process-test-1", "--password", "yangyang=process-test-2"
        )
        self.assertEqual(0, initialized.returncode, initialized.stderr)
        self.assertIn("reminders created: 7", initialized.stdout)

        created = self.run_cli("device-create", "--label", "process-passport")
        self.assertEqual(0, created.returncode, created.stderr)
        self.assertIn("token (shown once", created.stdout)
        device_id = [
            line.split(": ", 1)[1]
            for line in created.stdout.splitlines()
            if line.startswith("device id: ")
        ][0]

        listed = self.run_cli("device-list")
        self.assertEqual(0, listed.returncode, listed.stderr)
        self.assertIn(device_id, listed.stdout)

        revoked = self.run_cli("device-revoke", device_id)
        self.assertEqual(0, revoked.returncode, revoked.stderr)
        listed_again = self.run_cli("device-list")
        row_text = [
            line for line in listed_again.stdout.splitlines() if line.startswith(device_id)
        ][0]
        self.assertNotEqual("-", row_text.split()[-1])

        # Re-running init without --force refuses to add a second family.
        again = self.run_cli("init")
        self.assertNotEqual(0, again.returncode)
        self.assertIn("--force", again.stdout + again.stderr)

    def test_backup_and_restore_through_the_cli(self) -> None:
        self.assertEqual(0, self.run_cli("init").returncode)
        out_dir = os.path.join(self.directory, "backups")
        backup = self.run_cli("backup", "--out", out_dir)
        self.assertEqual(0, backup.returncode, backup.stderr)
        self.assertIn("sha256:", backup.stdout)
        files = os.listdir(out_dir)
        backup_files = [name for name in files if name.endswith(".sqlite3")]
        checksum_files = [name for name in files if name.endswith(".sha256")]
        self.assertEqual(1, len(backup_files))
        self.assertEqual(1, len(checksum_files))

        target = os.path.join(self.directory, "restored.sqlite3")
        restore = self.run_cli(
            "restore", "--from", os.path.join(out_dir, backup_files[0]), "--db", target
        )
        self.assertEqual(0, restore.returncode, restore.stderr)

        # The restored database refuses a second restore without --force.
        refused = self.run_cli(
            "restore", "--from", os.path.join(out_dir, backup_files[0]), "--db", target
        )
        self.assertNotEqual(0, refused.returncode)
        forced = self.run_cli(
            "restore", "--from", os.path.join(out_dir, backup_files[0]), "--db", target,
            "--force",
        )
        self.assertEqual(0, forced.returncode, forced.stderr)
        self.assertEqual(0, restore.returncode, restore.stderr)

        export = self.run_cli("export", "--db", target, "--format", "csv", "--out", "-")
        self.assertEqual(0, export.returncode, export.stderr)
        self.assertTrue(export.stdout.startswith("seq,id,version,status"))

    def test_serve_subcommand_accepts_help(self) -> None:
        result = subprocess.run(
            [sys.executable, "-m", "lanlan", "--help"],
            cwd=SERVICES_DIR,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        for subcommand in (
            "serve", "init", "device-create", "device-list", "device-revoke",
            "backup", "restore", "export", "check-config",
        ):
            self.assertIn(subcommand, result.stdout)


if __name__ == "__main__":
    unittest.main()
