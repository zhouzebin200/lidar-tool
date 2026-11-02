import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ulv_portable_build", ROOT / "packaging/legacy/build_portable.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class PrivateReleaseTests(unittest.TestCase):
    def test_private_issuer_build_and_cache_stay_outside_dist(self):
        with patch.object(builder.subprocess, "run") as run:
            output = builder.pyinstaller(ROOT / "licensing/license_admin.py", "ULV-LicenseAdmin", windowed=False, include_viewer_assets=False, private=True)
        command = run.call_args.args[0]
        for flag in ("--distpath", "--workpath"):
            directory = Path(command[command.index(flag) + 1])
            self.assertTrue(directory.is_relative_to(ROOT / "licensing/private"))
            self.assertFalse(directory.is_relative_to(ROOT / "dist"))
        self.assertTrue(output.is_relative_to(ROOT / "licensing/private"))


if __name__ == "__main__":
    unittest.main()
