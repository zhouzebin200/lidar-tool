import base64
from datetime import datetime, timezone
import json
import unittest
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

    def license(self, machine=None):
        payload = json.dumps({
            "version": 1,
            "product": licensing.PRODUCT,
            "machine": machine or self.machine,
            "customer": "test",
            "issued": datetime.now(timezone.utc).isoformat(),
            "expires": None,
        }, separators=(",", ":"), sort_keys=True).encode()
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


if __name__ == "__main__":
    unittest.main()
