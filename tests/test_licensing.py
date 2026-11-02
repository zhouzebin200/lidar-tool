import base64
from datetime import datetime, timezone
import json
import unittest
from pathlib import Path
import tempfile
import types
import sys
from unittest.mock import Mock
from unittest.mock import patch

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from ulv import licensing


def encoded(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


class LicensingTests(unittest.TestCase):
    def setUp(self):
        self.private = Ed25519PrivateKey.generate()
        public = self.private.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
        self.public_patch = patch.object(licensing, "PUBLIC_KEY_B64", encoded(public))
        self.public_patch.start()
        self.machine = "ULV-AAAAA-BBBBB-CCCCC-DDDDD"

    def tearDown(self):
        self.public_patch.stop()

    def license(self, machine=None, **changes):
        value = {
            "version": 1,
            "product": licensing.PRODUCT,
            "machine": machine or self.machine,
            "customer": "test",
            "issued": datetime.now(timezone.utc).isoformat(),
            "expires": None,
        }
        value.update(changes)
        return self.sign(value)

    def sign(self, value):
        payload = json.dumps(value, separators=(",", ":"), sort_keys=True).encode()
        return f"ULV1.{encoded(payload)}.{encoded(self.private.sign(payload))}"

    def test_signed_license_is_machine_bound(self):
        self.assertTrue(licensing.verify_license_text(self.license(), self.machine).valid)
        result = licensing.verify_license_text(
            self.license(), "ULV-22222-33333-44444-55555"
        )
        self.assertFalse(result.valid)
        self.assertIn("机器码", result.reason)

    def test_modified_license_is_rejected(self):
        code = self.license()
        prefix, payload, signature = code.split(".")
        modified = f"{prefix}.{payload[:-1]}A.{signature}"
        self.assertFalse(licensing.verify_license_text(modified, self.machine).valid)

    def test_machine_code_has_stable_display_format(self):
        self.assertRegex(licensing.machine_code(), licensing.MACHINE_PATTERN)

    def test_rejects_invalid_signed_fields_and_expired_license(self):
        for changes in (
            {"version": 2}, {"version": True}, {"product": "other"},
            {"expires": "2030-01-01T00:00:00"}, {"expires": 0},
            {"issued": "2099-01-01T00:00:00Z"},
            {"issued": "2000-01-01T00:00:00Z", "expires": "2001-01-01T00:00:00Z"},
        ):
            with self.subTest(changes=changes):
                self.assertFalse(licensing.verify_license_text(self.license(**changes), self.machine).valid)
        self.assertFalse(licensing.verify_license_text(self.sign([]), self.machine).valid)
        self.assertFalse(licensing.verify_license_text("x" * 20000, self.machine).valid)

    def test_corrupt_license_file_and_invalid_install_do_not_crash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "license.key"
            path.write_bytes(b"\xff\xfe")
            with patch.object(licensing, "license_paths", return_value=(path,)):
                self.assertFalse(licensing.find_license().valid)
                self.assertFalse(licensing.install_license(str(path)).valid)
                self.assertFalse(licensing.install_license("x" * 20000).valid)

    def test_dpapi_round_trip(self):
        encrypted = licensing._clock_data(b"clock-test", True)
        self.assertEqual(licensing._clock_data(encrypted, False), b"clock-test")

    def test_runtime_revokes_deleted_license_and_detects_clock_rollback(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "license.key"
            path.write_text(self.license(), encoding="ascii")
            with patch.object(licensing, "license_paths", return_value=(path,)), patch.object(licensing, "machine_code", return_value=self.machine):
                session = licensing.LicenseSession()
                self.assertTrue(session.check(force=True).valid)
                state = session._clock_path
                state.write_bytes(licensing._clock_data(json.dumps({"machine": self.machine, "last_seen": datetime.now(timezone.utc).timestamp() + 3600}).encode(), True))
                self.assertFalse(session.check(force=True).valid)
                state.unlink()
                path.unlink()
                self.assertFalse(session.check(force=True).valid)

    def test_in_process_clock_cannot_be_reset_by_deleting_state(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "license.key"
            with patch.object(licensing, "license_paths", return_value=(path,)), patch.object(licensing, "find_license", return_value=licensing.LicenseResult(True, "ok")):
                session = licensing.LicenseSession()
                self.assertTrue(session.check(force=True).valid)
                session._last_wall += 3600
                session._clock_path.unlink()
                self.assertFalse(session.check(force=True).valid)

    def test_corrupt_clock_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "license.key"
            with patch.object(licensing, "license_paths", return_value=(path,)), patch.object(licensing, "find_license", return_value=licensing.LicenseResult(True, "ok")):
                session = licensing.LicenseSession()
                session._clock_path.write_bytes(b"broken")
                self.assertFalse(session.check(force=True).valid)

    def test_licensed_entry_blocks_startup_and_passes_session(self):
        import run_licensed
        session = Mock()
        server = types.ModuleType("ulv.server")
        server.main = Mock(return_value=0)
        with patch.object(run_licensed, "LicenseSession", return_value=session), patch.object(run_licensed, "show_license_error"), patch.object(sys, "argv", ["run_licensed.py"]), patch.dict(sys.modules, {"ulv.server": server}):
            session.check.return_value = licensing.LicenseResult(False, "blocked")
            self.assertEqual(run_licensed.main(), 3)
            server.main.assert_not_called()
            session.check.return_value = licensing.LicenseResult(True, "ok")
            self.assertEqual(run_licensed.main(), 0)
            server.main.assert_called_once_with(license_session=session)


if __name__ == "__main__":
    unittest.main()
