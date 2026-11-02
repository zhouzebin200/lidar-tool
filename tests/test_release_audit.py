from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
import io
import zipfile


SCRIPT = Path(__file__).resolve().parents[1] / "packaging" / "audit_public_release.py"
SPEC = importlib.util.spec_from_file_location("ulv_release_audit", SCRIPT)
assert SPEC and SPEC.loader
release_audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_audit)


class PublicReleaseAuditTests(unittest.TestCase):
    def test_rejects_compressed_and_nested_secrets(self):
        nested = io.BytesIO()
        with zipfile.ZipFile(nested, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("config.json", '{"private_key":"dummy-test-data"}')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with zipfile.ZipFile(root / "customer.zip", "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("nested.zip", nested.getvalue())
                archive.writestr("license.key", "dummy")
            problems = "\n".join(release_audit.audit(root))
            self.assertIn("nested.zip!config.json", problems)
            self.assertIn("license.key", problems)

    def test_rejects_invalid_zip(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "customer.zip").write_bytes(b"broken")
            self.assertIn("invalid ZIP", "\n".join(release_audit.audit(root)))

    def test_accepts_runtime_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "index.html").write_text("<title>ULV</title>", encoding="utf-8")
            (root / "viewer.wasm").write_bytes(b"\x00asm")
            (root / "decoder.dll").write_bytes(b"MZ")
            self.assertEqual(release_audit.audit(root), [])

    def test_rejects_source_debug_and_private_material(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "decoder.cpp").write_text("int main(){}", encoding="utf-8")
            (root / "viewer.js.map").write_text("{}", encoding="utf-8")
            (root / "config.json").write_text('{"private_key":"secret"}', encoding="utf-8")
            problems = "\n".join(release_audit.audit(root))
            self.assertIn("decoder.cpp", problems)
            self.assertIn("viewer.js.map", problems)
            self.assertIn("config.json", problems)


if __name__ == "__main__":
    unittest.main()
