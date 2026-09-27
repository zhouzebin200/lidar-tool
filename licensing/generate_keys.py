"""Generate the offline Ed25519 licensing key pair.

The private key stays under licensing/private and must never be distributed
with the viewer. The generated public module is safe to embed in the EXE.
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


ROOT = Path(__file__).resolve().parents[1]
PRIVATE_FILE = ROOT / "licensing" / "private" / "license_private_key.json"
PUBLIC_MODULE = ROOT / "ulv" / "license_public_key.py"


def encoded(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def main() -> None:
    if PRIVATE_FILE.exists():
        raise SystemExit(f"Refusing to replace existing signing key: {PRIVATE_FILE}")
    private_key = Ed25519PrivateKey.generate()
    private_raw = private_key.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    public_raw = private_key.public_key().public_bytes(
        serialization.Encoding.Raw,
        serialization.PublicFormat.Raw,
    )
    PRIVATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    PRIVATE_FILE.write_text(
        json.dumps({"algorithm": "Ed25519", "private_key": encoded(private_raw)}, indent=2),
        encoding="utf-8",
    )
    PUBLIC_MODULE.write_text(
        '"""Generated public verification key for the licensed ULV build."""\n\n'
        f'PUBLIC_KEY_B64 = "{encoded(public_raw)}"\n',
        encoding="utf-8",
    )
    print(f"Private signing key created at {PRIVATE_FILE}")
    print(f"Public verification key created at {PUBLIC_MODULE}")


if __name__ == "__main__":
    main()
