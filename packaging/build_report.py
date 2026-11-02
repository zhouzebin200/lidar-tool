"""Compile and audit the licensed integrated viewer; never build an issuer here."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile

from audit_public_release import audit

ROOT = Path(__file__).resolve().parents[1]
NAME = "Lijian-LidarTool-Licensed"


def validate_plugins(directory: Path) -> None:
    for vendor in ("robosense", "hesai", "huawei_l306", "velodyne", "seyond"):
        path = directory / f"libulv_{vendor}.dll"
        if not path.is_file():
            raise RuntimeError(f"missing required decoder: {path.name}")
        data = path.read_bytes()
        try:
            offset = struct.unpack_from("<I", data, 60)[0]
            valid = (data[:2] == b"MZ" and data[offset:offset + 4] == b"PE\0\0"
                     and struct.unpack_from("<H", data, offset + 4)[0] == 0x8664)
        except struct.error:
            valid = False
        if not valid:
            raise RuntimeError(f"decoder must be Windows x64 PE: {path.name}")


def main() -> None:
    destination = ROOT / "dist" / "customer" / NAME
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite an existing customer release: {destination}")
    validate_plugins(ROOT / "plugins")
    output = ROOT / "build" / "licensed-native"
    command = [
        sys.executable, "-m", "nuitka", "--mode=standalone", "--mingw64",
        "--assume-yes-for-downloads", "--windows-console-mode=disable",
        "--windows-uac-admin", "--include-package=ulv", "--jobs=4",
        f"--output-dir={output}", f"--output-filename={NAME}.exe",
        f"--report={output / 'compilation-report.xml'}",
        f"--include-data-dir={ROOT / 'web'}=web",
        "--nofollow-import-to=unittest,setuptools,licensing,tests",
        str(ROOT / "run_licensed.py"),
    ]
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    # Store toolchains outside Windows Store AppData redirection so gcc finds its headers.
    environment.setdefault("NUITKA_CACHE_DIR", str(Path.home() / ".cache" / "nuitka"))
    subprocess.run(command, cwd=ROOT, env=environment, check=True)
    compiled = output / "run_licensed.dist"
    # Nuitka's data-dir filter skips .bin calibration files; copy all calibration assets explicitly.
    shutil.copytree(ROOT / "corrections", compiled / "corrections", dirs_exist_ok=True)
    (compiled / "plugins").mkdir(exist_ok=True)
    for plugin in (ROOT / "plugins").glob("*.dll"):
        shutil.copy2(plugin, compiled / "plugins" / plugin.name)
    shutil.copy2(ROOT / "licensing" / "LICENSE-INSTRUCTIONS.txt", compiled / "授权说明.txt")
    problems = audit(compiled)
    if problems:
        raise RuntimeError("Release audit failed:\n" + "\n".join(problems))
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(compiled, destination)
    archive = destination.with_suffix(".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(destination.rglob("*")):
            if path.is_file():
                bundle.write(path, Path(NAME) / path.relative_to(destination))
    problems = audit(destination.parent)
    if problems:
        archive.unlink(missing_ok=True)
        raise RuntimeError("Final release audit failed:\n" + "\n".join(problems))
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{checksum}  {archive.name}\n", encoding="ascii")
    print(json.dumps({"release": str(destination), "zip": str(archive), "sha256": checksum}, indent=2))


if __name__ == "__main__":
    main()
