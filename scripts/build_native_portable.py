"""Cross-platform portable builder for ULV native plugins.

Uses Zig/Clang targeting baseline x86_64 Windows (no AVX-512 or host-specific instructions)
so the generated DLLs run on any x86_64 Windows machine and inside VMs (Parallels, VMware, etc.).
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build" / "native-zig-universal"
TOOL_DIR = ROOT / "build" / "native-zig-tools"


def find_tool(name: str, module_name: str | None = None) -> Path:
    # 1. Check in sys.executable directory (e.g. venv/bin or venv/Scripts)
    parent_bin = Path(sys.executable).parent
    for suffix in ("", ".exe", ".cmd"):
        candidate = parent_bin / f"{name}{suffix}"
        if candidate.is_file():
            return candidate

    # 2. Check common virtualenv locations (.venv, /tmp/ulv-venv, etc.)
    for extra_dir in (ROOT / ".venv", Path("/tmp/ulv-venv"), Path.home() / ".venv"):
        for sub in ("bin", "Scripts"):
            candidate = extra_dir / sub / name
            for suffix in ("", ".exe", ".cmd"):
                cand_file = candidate.with_suffix(suffix) if suffix else candidate
                if cand_file.is_file():
                    return cand_file

    # 3. Check via python module (including site-packages of other venvs if present)
    for test_dir in (Path(sys.executable).parent.parent, ROOT / ".venv", Path("/tmp/ulv-venv")):
        for sp in test_dir.glob("lib/python*/site-packages"):
            if (sp / "ziglang").is_dir():
                for suffix in ("", ".exe"):
                    candidate = sp / "ziglang" / f"{name}{suffix}"
                    if candidate.is_file():
                        return candidate

    # 2. Check via python module
    if module_name:
        try:
            mod = __import__(module_name)
            if hasattr(mod, "__file__") and mod.__file__:
                mod_dir = Path(mod.__file__).parent
                # ziglang package puts zig or zig.exe inside package dir
                for suffix in ("", ".exe"):
                    candidate = mod_dir / f"{name}{suffix}"
                    if candidate.is_file():
                        return candidate
                if hasattr(mod, "BIN_DIR"):
                    candidate = Path(mod.BIN_DIR) / f"{name}{suffix}"
                    if candidate.is_file():
                        return candidate
                if hasattr(mod, "CMAKE_BIN_DIR"):
                    candidate = Path(mod.CMAKE_BIN_DIR) / f"{name}{suffix}"
                    if candidate.is_file():
                        return candidate
        except ImportError:
            pass

    # 3. Check system PATH
    which_path = shutil.which(name)
    if which_path:
        return Path(which_path)

    raise FileNotFoundError(
        f"Required build tool '{name}' not found. Please run: pip install cmake ninja ziglang"
    )


def main() -> None:
    zig = find_tool("zig", "ziglang")
    ninja = find_tool("ninja", "ninja")
    cmake = find_tool("cmake", "cmake")

    print(f"Using build tools:\n  cmake: {cmake}\n  ninja: {ninja}\n  zig:   {zig}")

    TOOL_DIR.mkdir(parents=True, exist_ok=True)
    is_windows = os.name == "nt"

    if is_windows:
        ar_wrapper = TOOL_DIR / "zig-ar.cmd"
        ranlib_wrapper = TOOL_DIR / "zig-ranlib.cmd"
        c_wrapper = TOOL_DIR / "zig-cc.cmd"
        cxx_wrapper = TOOL_DIR / "zig-cxx.cmd"
        ar_wrapper.write_text(f'@echo off\r\n"{zig}" ar %*\r\n', encoding="ascii")
        ranlib_wrapper.write_text(f'@echo off\r\n"{zig}" ranlib %*\r\n', encoding="ascii")
        c_wrapper.write_text(f'@echo off\r\n"{zig}" cc -target x86_64-windows-gnu -mcpu=x86_64 %*\r\n', encoding="ascii")
        cxx_wrapper.write_text(f'@echo off\r\n"{zig}" c++ -target x86_64-windows-gnu -mcpu=x86_64 %*\r\n', encoding="ascii")
    else:
        ar_wrapper = TOOL_DIR / "zig-ar.sh"
        ranlib_wrapper = TOOL_DIR / "zig-ranlib.sh"
        c_wrapper = TOOL_DIR / "zig-cc.sh"
        cxx_wrapper = TOOL_DIR / "zig-cxx.sh"
        ar_wrapper.write_text(f'#!/bin/sh\nexec "{zig}" ar "$@"\n', encoding="utf-8")
        ranlib_wrapper.write_text(f'#!/bin/sh\nexec "{zig}" ranlib "$@"\n', encoding="utf-8")
        c_wrapper.write_text(f'#!/bin/sh\nexec "{zig}" cc -target x86_64-windows-gnu -mcpu=x86_64 "$@"\n', encoding="utf-8")
        cxx_wrapper.write_text(f'#!/bin/sh\nexec "{zig}" c++ -target x86_64-windows-gnu -mcpu=x86_64 "$@"\n', encoding="utf-8")
        for script in (ar_wrapper, ranlib_wrapper, c_wrapper, cxx_wrapper):
            script.chmod(0o755)

    configure_cmd = [
        str(cmake),
        "-S", str(ROOT / "native"),
        "-B", str(BUILD_DIR),
        "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={ninja}",
        f"-DCMAKE_C_COMPILER={c_wrapper}",
        f"-DCMAKE_CXX_COMPILER={cxx_wrapper}",
        f"-DCMAKE_AR={ar_wrapper}",
        f"-DCMAKE_RANLIB={ranlib_wrapper}",
        f"-DCMAKE_CXX_COMPILER_AR={ar_wrapper}",
        f"-DCMAKE_CXX_COMPILER_RANLIB={ranlib_wrapper}",
        "-DCMAKE_SYSTEM_NAME=Windows",
        "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    ]

    print("Running CMake configure...")
    subprocess.run(configure_cmd, check=True)

    print("Running Ninja build...")
    subprocess.run([str(cmake), "--build", str(BUILD_DIR), "--config", "Release", "-j", "4"], check=True)
    print("Build complete! Clean plugins are ready in plugins/.")


if __name__ == "__main__":
    main()
