"""One-click release packager for Universal LiDAR Viewer.

Produces a clean, portable, and one-click installable Windows bundle:
  - Universal x86_64 C++ native decoder plugins (baseline compatibility)
  - Embedded Python runtime binaries & standard libraries
  - Static WebGL2 frontend & report studio (web/)
  - Nominal calibrations (corrections/) & varID configs (config/varID/)
  - Launch & desktop shortcut scripts
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
PORTABLE_DIR = DIST / "Lijian-LidarTool-Portable"
ZIP_NAME = "砺见智行激光雷达测试工具-Windows绿色免安装版.zip"
ZIP_NAME_EN = "Lijian-LidarTool-Windows-Portable.zip"


def build_native_plugins() -> None:
    portable_build_script = ROOT / "scripts" / "build_native_portable.py"
    if portable_build_script.exists():
        print("[1/4] Checking and building universal native plugins...")
        subprocess.run([sys.executable, str(portable_build_script)], cwd=ROOT, check=True)
    else:
        print("[1/4] Using pre-compiled native plugins in plugins/...")


def assemble_portable() -> Path:
    print("[2/4] Assembling portable package directory...")
    PORTABLE_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Copy web assets
    shutil.copytree(ROOT / "web", PORTABLE_DIR / "web", dirs_exist_ok=True)

    # 2. Copy calibrations
    shutil.copytree(ROOT / "corrections", PORTABLE_DIR / "corrections", dirs_exist_ok=True)

    # 3. Copy ONLY safe static configs (config/varID/), NEVER copy runtime.json or auth_config.csv!
    config_dest = PORTABLE_DIR / "config"
    config_dest.mkdir(exist_ok=True)
    if (ROOT / "config" / "varID").exists():
        shutil.copytree(ROOT / "config" / "varID", config_dest / "varID", dirs_exist_ok=True)

    # Clean any accidental runtime configs from previous runs
    for dirty in ("runtime.json", "auth_config.csv", "network-recovery.json", "gateway.json", "ptp.json"):
        (config_dest / dirty).unlink(missing_ok=True)

    # 4. Copy native plugins (DLL only, no PDB)
    plugins_dest = PORTABLE_DIR / "plugins"
    plugins_dest.mkdir(exist_ok=True)
    for dll in (ROOT / "plugins").glob("*.dll"):
        shutil.copy2(dll, plugins_dest / dll.name)
    # Remove any stray PDB in destination
    for pdb in plugins_dest.glob("*.pdb"):
        pdb.unlink(missing_ok=True)

    # 5. Ensure batch scripts are generated with correct CRLF line endings
    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    if str(ROOT / "packaging") not in sys.path:
        sys.path.insert(0, str(ROOT / "packaging"))
    from generate_bat_scripts import generate_scripts
    generate_scripts()

    print(f"Assembled portable directory at: {PORTABLE_DIR}")
    return PORTABLE_DIR


def create_zip_archive() -> tuple[Path, Path]:
    print("[3/4] Creating zip archives...")
    zip_path = DIST / ZIP_NAME
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as bundle:
        for file in sorted(PORTABLE_DIR.rglob("*")):
            if file.is_file() and not file.name.endswith(".pdb"):
                arcname = Path("Lijian-LidarTool-Portable") / file.relative_to(PORTABLE_DIR)
                bundle.write(file, arcname)

    checksum = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    sha_file = zip_path.with_suffix(".zip.sha256")
    sha_file.write_text(f"{checksum}  {zip_path.name}\n", encoding="utf-8")

    # Also write English-named copy for standard tooling/script compatibility
    zip_path_en = DIST / ZIP_NAME_EN
    shutil.copy2(zip_path, zip_path_en)
    sha_file_en = zip_path_en.with_suffix(".zip.sha256")
    sha_file_en.write_text(f"{checksum}  {zip_path_en.name}\n", encoding="utf-8")

    print(f"[4/4] Package complete:\n  CN Archive: {zip_path}\n  EN Archive: {zip_path_en}\n  SHA-256: {checksum}")
    return zip_path, zip_path_en


def main() -> None:
    build_native_plugins()
    assemble_portable()
    create_zip_archive()


if __name__ == "__main__":
    main()
