from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RecoverableNetworkMutationTests(unittest.TestCase):
    def test_runtime_uses_recoverable_elevated_network_manager(self):
        self.assertTrue((ROOT / "ulv" / "network_bootstrap.py").exists())
        self.assertFalse((ROOT / "ulv" / "auto_network.py").exists())
        runtime = (ROOT / "ulv" / "network_bootstrap.py").read_text(encoding="utf-8")
        for required in ("ensure_windows_admin", "ShellExecuteW", "New-NetIPAddress", "Set-NetAdapter"):
            self.assertIn(required, runtime)
        self.assertIn("network-recovery.json", (ROOT / "ulv" / "server.py").read_text(encoding="utf-8"))
        self.assertLess(runtime.index("_write_records([record])"), runtime.index("New-NetIPAddress"))

    def test_viewer_builds_request_uac_admin(self):
        build_report = (ROOT / "packaging" / "build_report.py").read_text(encoding="utf-8")
        build_portable = (ROOT / "packaging" / "legacy" / "build_portable.py").read_text(encoding="utf-8")
        self.assertIn("--windows-uac-admin", build_report)
        self.assertIn("--uac-admin", build_portable)
        for spec in ROOT.glob("*.spec"):
            if "LicenseAdmin" not in spec.name and "LidarReportStudio" not in spec.name:
                self.assertIn("uac_admin=True", spec.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
