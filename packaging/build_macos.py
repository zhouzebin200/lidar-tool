"""Build a self-contained macOS app and drag-to-install DMG."""

from __future__ import annotations

import os
import plistlib
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
APP_NAME = "砺见智行工作室激光雷达测试工具"
DIST = ROOT / "dist"
APP = DIST / f"{APP_NAME}.app"
DMG = DIST / f"{APP_NAME}-macOS-arm64.dmg"


def run(*arguments: str) -> None:
    subprocess.run(arguments, cwd=ROOT, check=True)


def main() -> int:
    command = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--windowed",
        "--onedir",
        "--name",
        APP_NAME,
        "--osx-bundle-identifier",
        "com.lijianzhixing.universal-lidar-viewer",
    ]
    for directory in ("web", "corrections", "config"):
        command += ["--add-data", f"{ROOT / directory}{os.pathsep}{directory}"]
    for plugin in sorted((ROOT / "plugins").glob("*.dylib")):
        command += ["--add-binary", f"{plugin}{os.pathsep}plugins"]
    command.append(str(ROOT / "run.py"))
    run(*command)

    plist_path = APP / "Contents" / "Info.plist"
    with plist_path.open("rb") as stream:
        info = plistlib.load(stream)
    info.update(
        {
            "CFBundleDisplayName": APP_NAME,
            "CFBundleName": APP_NAME,
            "CFBundleShortVersionString": "0.2.0",
            "CFBundleVersion": "0.2.0",
            "LSMinimumSystemVersion": "11.0",
            "NSLocalNetworkUsageDescription": "用于发现激光雷达并接收实时点云数据。",
        }
    )
    with plist_path.open("wb") as stream:
        plistlib.dump(info, stream, sort_keys=False)

    # Ad-hoc signing keeps all nested dylibs and the application bundle
    # internally consistent. Distribution outside this Mac still requires an
    # Apple Developer ID signature/notarization to avoid Gatekeeper warnings.
    run("/usr/bin/codesign", "--force", "--deep", "--sign", "-", str(APP))

    staging = ROOT / "build" / "macos-dmg"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    shutil.copytree(APP, staging / APP.name, symlinks=True)
    (staging / "Applications").symlink_to("/Applications")
    if DMG.exists():
        DMG.unlink()
    run(
        "/usr/bin/hdiutil",
        "create",
        "-volname",
        APP_NAME,
        "-srcfolder",
        str(staging),
        "-ov",
        "-format",
        "UDZO",
        str(DMG),
    )
    print(DMG)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
