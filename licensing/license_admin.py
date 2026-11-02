"""Private offline license issuer. Never distribute this with the viewer."""

from __future__ import annotations

import argparse
import base64
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


PRODUCT = "UniversalLidarViewer-Licensed"
PREFIX = "ULV1"
MACHINE_PATTERN = re.compile(r"^ULV(?:-[A-Z2-7]{5}){4}$")
ROOT = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[1]))
DEFAULT_KEY = ROOT / "licensing" / "private" / "license_private_key.json"


def encode64(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def decode64(value: str) -> bytes:
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def main() -> int:
    parser = argparse.ArgumentParser(description="ULV private offline license issuer")
    parser.add_argument("machine", help="ULV-xxxxx-xxxxx-xxxxx-xxxxx machine code")
    parser.add_argument("--customer", default="licensed-user")
    parser.add_argument("--days", type=int, help="validity in days; omit for permanent")
    parser.add_argument("--output", type=Path, default=Path("license.key"))
    parser.add_argument("--private-key", type=Path, default=DEFAULT_KEY)
    args = parser.parse_args()
    if args.days is not None and args.days <= 0:
        parser.error("--days must be a positive integer")
    machine = args.machine.strip().upper()
    if not MACHINE_PATTERN.fullmatch(machine):
        parser.error("invalid ULV machine code")
    private_data = json.loads(args.private_key.read_text(encoding="utf-8"))
    private_key = Ed25519PrivateKey.from_private_bytes(decode64(private_data["private_key"]))
    now = datetime.now(timezone.utc).replace(microsecond=0)
    payload = {
        "version": 1,
        "product": PRODUCT,
        "machine": machine,
        "customer": args.customer,
        "issued": now.isoformat().replace("+00:00", "Z"),
        "expires": (now + timedelta(days=args.days)).isoformat().replace("+00:00", "Z") if args.days else None,
    }
    payload_bytes = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    code = f"{PREFIX}.{encode64(payload_bytes)}.{encode64(private_key.sign(payload_bytes))}"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(code + "\n", encoding="ascii")
    print(f"License issued for {machine}: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
