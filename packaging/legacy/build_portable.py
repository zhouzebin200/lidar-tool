"""Build ordinary, machine-licensed, and private license-admin Windows apps."""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "web"
CORRECTIONS = ROOT / "corrections"
PRIVATE_KEY = ROOT / "licensing" / "private" / "license_private_key.json"


def pyinstaller(
    entry: Path,
    name: str,
    *,
    windowed: bool,
    include_viewer_assets: bool,
    extra_data: tuple[tuple[Path, str], ...] = (),
    private: bool = False,
) -> Path:
    command = [
        sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile",
        "--name", name,
    ]
    destination = ROOT / "licensing" / "private" / "releases" if private else ROOT / "dist"
    if private:
        command.extend(["--distpath", str(destination), "--workpath", str(destination.parent / "build")])
    if os.name == "nt" and windowed:
        command.append("--windowed")
    if os.name == "nt" and include_viewer_assets:
        command.append("--uac-admin")
    if include_viewer_assets:
        command.extend(["--add-data", f"{WEB}{os.pathsep}web"])
        command.extend(["--add-data", f"{CORRECTIONS}{os.pathsep}corrections"])
        for extension in ("*.dll", "*.so", "*.dylib"):
            for plugin in (ROOT / "plugins").glob(extension):
                command.extend(["--add-binary", f"{plugin}{os.pathsep}plugins"])
    for source, destination in extra_data:
        command.extend(["--add-data", f"{source}{os.pathsep}{destination}"])
    command.append(str(entry))
    subprocess.run(command, cwd=ROOT, check=True)
    suffix = ".exe" if os.name == "nt" else ""
    return destination / f"{name}{suffix}"


def copy_common_documentation(bundle: Path) -> None:
    bundle.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "README.md", bundle / "README.md")


def build_ordinary() -> Path:
    executable = pyinstaller(
        ROOT / "run.py", "UniversalLidarViewer",
        windowed=True, include_viewer_assets=True,
    )
    bundle = ROOT / "dist" / "UniversalLidarViewer-portable"
    copy_common_documentation(bundle)
    shutil.copy2(executable, bundle / executable.name)
    plugin_bundle = bundle / "plugins"
    plugin_bundle.mkdir(parents=True, exist_ok=True)
    for extension in ("*.dll", "*.so", "*.dylib"):
        for plugin in (ROOT / "plugins").glob(extension):
            shutil.copy2(plugin, plugin_bundle / plugin.name)
    correction_bundle = bundle / "corrections"
    if correction_bundle.exists():
        shutil.rmtree(correction_bundle)
    shutil.copytree(CORRECTIONS, correction_bundle)
    return bundle


def build_licensed() -> Path:
    executable = pyinstaller(
        ROOT / "run_licensed.py", "UniversalLidarViewer-Licensed",
        windowed=True, include_viewer_assets=True,
    )
    bundle = ROOT / "dist" / "UniversalLidarViewer-licensed"
    if bundle.exists():
        shutil.rmtree(bundle)
    copy_common_documentation(bundle)
    shutil.copy2(executable, bundle / executable.name)
    shutil.copy2(ROOT / "licensing" / "LICENSE-INSTRUCTIONS.txt", bundle / "授权说明.txt")
    return bundle


def build_private_admin() -> Path:
    if not PRIVATE_KEY.is_file():
        raise FileNotFoundError("Private license key missing; run licensing/generate_keys.py once")
    executable = pyinstaller(
        ROOT / "licensing" / "license_admin.py", "ULV-LicenseAdmin",
        windowed=False, include_viewer_assets=False,
        extra_data=((PRIVATE_KEY, "licensing/private"),),
        private=True,
    )
    bundle = ROOT / "licensing" / "private" / "releases" / "ULV-LicenseAdmin-private"
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    shutil.copy2(executable, bundle / executable.name)
    shutil.copy2(ROOT / "licensing" / "ADMIN-INSTRUCTIONS.txt", bundle / "发码工具说明.txt")
    return bundle


def main() -> None:
    ordinary = build_ordinary()
    licensed = build_licensed()
    admin = build_private_admin()
    print(f"Ordinary portable application: {ordinary}")
    print(f"Licensed single-file application: {licensed}")
    print(f"PRIVATE license issuer (do not distribute): {admin}")


if __name__ == "__main__":
    main()
