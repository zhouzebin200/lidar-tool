"""Fail a public release when developer material or secrets are included."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


BLOCKED_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp",
    ".py", ".pyc", ".pyo", ".pdb", ".map", ".spec",
    ".ps1", ".bat", ".cmd", ".sh", ".pem", ".key", ".pfx", ".p12",
    ".jks", ".keystore", ".env",
}
BLOCKED_NAMES = {
    ".git", ".github", "cmakelists.txt", "makefile", "license.key",
    "license_private_key.json", "ulv-licenseadmin.exe",
}
BLOCKED_PARTS = {
    "tests", "test", "third_party", "private", "generated-licenses",
    "__pycache__", ".pytest_cache", "build", "docs",
}
SECRET_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----",
    b"-----BEGIN OPENSSH PRIVATE KEY-----",
    b'"private_key"',
    b"license_private_key",
)


def contains_secret_marker(path: Path) -> bool:
    longest = max(map(len, SECRET_MARKERS))
    tail = b""
    with path.open("rb") as stream:
        while chunk := stream.read(256 * 1024):
            data = tail + chunk
            if any(marker in data for marker in SECRET_MARKERS):
                return True
            tail = data[-(longest - 1):]
    return False


def audit(root: Path) -> list[str]:
    root = root.resolve()
    if not root.is_dir():
        return [f"not a directory: {root}"]
    problems: list[str] = []
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        lowered_parts = {part.lower() for part in relative.parts}
        if lowered_parts & BLOCKED_PARTS:
            problems.append(f"blocked development path: {relative}")
            continue
        if path.is_dir():
            if path.name.lower() in BLOCKED_NAMES:
                problems.append(f"blocked directory: {relative}")
            continue
        lowered_name = path.name.lower()
        if lowered_name in BLOCKED_NAMES:
            problems.append(f"blocked sensitive file: {relative}")
            continue
        if path.suffix.lower() in BLOCKED_SUFFIXES or lowered_name.endswith(".js.map"):
            problems.append(f"blocked source/debug file: {relative}")
            continue
        try:
            has_secret = contains_secret_marker(path)
        except OSError as error:
            problems.append(f"cannot inspect {relative}: {error}")
            continue
        if has_secret:
            problems.append(f"possible embedded private material: {relative}")
    return sorted(set(problems))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="staged public release directory")
    args = parser.parse_args()
    problems = audit(args.directory)
    if problems:
        print("PUBLIC RELEASE AUDIT FAILED", file=sys.stderr)
        for problem in problems:
            print(f"- {problem}", file=sys.stderr)
        return 1
    print(f"PUBLIC RELEASE AUDIT PASSED: {args.directory.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
