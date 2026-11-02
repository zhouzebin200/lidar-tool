"""Fail a public release when developer material or secrets are included."""

from __future__ import annotations

import argparse
import io
from pathlib import Path
import sys
import tempfile
import zipfile


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


def inspect_container(data: bytes, label: str, depth: int = 0) -> list[str]:
    if depth > 4:
        return [f"archive nesting limit exceeded: {label}"]
    problems = []
    members = []
    try:
        if zipfile.is_zipfile(io.BytesIO(data)):
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                if sum(item.file_size for item in archive.infolist()) > 256 * 1024 * 1024:
                    return [f"archive inspection size limit exceeded: {label}"]
                members = [(item.filename, archive.read(item)) for item in archive.infolist() if not item.is_dir()]
        elif data.startswith(b"MZ") and b"MEI\x0c\x0b\x0a\x0b\x0e" in data[-8192:]:
            from PyInstaller.archive.readers import CArchiveReader
            with tempfile.TemporaryDirectory(prefix="ulv-audit-") as temporary:
                executable = Path(temporary) / "bundle.exe"
                executable.write_bytes(data)
                archive = CArchiveReader(str(executable))
                if sum(entry[2] for entry in archive.toc.values()) > 256 * 1024 * 1024:
                    return [f"EXE inspection size limit exceeded: {label}"]
                members = [(name, archive.extract(name)) for name, entry in archive.toc.items() if entry[-1] not in ("o", "d")]
        elif label.lower().endswith(".zip"):
            return [f"invalid ZIP archive: {label}"]
    except Exception as error:
        return [f"cannot inspect container {label}: {type(error).__name__}"]
    for name, content in members:
        parts = name.replace("\\", "/").lower().split("/")
        location = f"{label}!{name}"
        if (set(parts) & (BLOCKED_NAMES | BLOCKED_PARTS)
                or Path(parts[-1]).suffix in BLOCKED_SUFFIXES):
            problems.append(f"blocked archive member: {location}")
        elif any(marker in content for marker in SECRET_MARKERS):
            problems.append(f"possible embedded private material: {location}")
        problems.extend(inspect_container(content, location, depth + 1))
    return problems


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
        try:
            if path.suffix.lower() in (".zip", ".exe"):
                problems.extend(inspect_container(path.read_bytes(), str(relative)))
        except OSError as error:
            problems.append(f"cannot inspect {relative}: {error}")
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
