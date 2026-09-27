"""Offline, machine-bound license verification for the licensed build."""

from __future__ import annotations

import base64
import ctypes
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from .license_public_key import PUBLIC_KEY_B64


PRODUCT = "UniversalLidarViewer-Licensed"
LICENSE_PREFIX = "ULV1"
MACHINE_PATTERN = re.compile(r"^ULV(?:-[A-Z2-7]{5}){4}$")


@dataclass(frozen=True)
class LicenseResult:
    valid: bool
    reason: str
    payload: dict[str, object] | None = None
    path: Path | None = None


def _decode64(value: str) -> bytes:
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def _machine_guid() -> str:
    if os.name != "nt":
        machine_id = Path("/etc/machine-id")
        if machine_id.is_file():
            return machine_id.read_text(encoding="ascii", errors="ignore").strip()
        return socket.gethostname()
    try:
        import winreg
        access = winreg.KEY_READ | getattr(winreg, "KEY_WOW64_64KEY", 0)
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\Microsoft\Cryptography",
            0,
            access,
        ) as key:
            return str(winreg.QueryValueEx(key, "MachineGuid")[0])
    except OSError:
        return socket.gethostname()


def _system_volume_serial() -> str:
    if os.name != "nt":
        try:
            return str(os.stat("/").st_dev)
        except OSError:
            return "unknown-volume"
    root = os.environ.get("SystemDrive", "C:") + "\\"
    serial = ctypes.c_uint32()
    maximum_component = ctypes.c_uint32()
    flags = ctypes.c_uint32()
    ok = ctypes.windll.kernel32.GetVolumeInformationW(
        ctypes.c_wchar_p(root), None, 0, ctypes.byref(serial),
        ctypes.byref(maximum_component), ctypes.byref(flags), None, 0,
    )
    return f"{serial.value:08X}" if ok else "unknown-volume"


def machine_code() -> str:
    material = f"{PRODUCT}|{_machine_guid()}|{_system_volume_serial()}".encode("utf-8")
    digest = base64.b32encode(hashlib.sha256(material).digest()[:13]).decode("ascii")[:20]
    return "ULV-" + "-".join(digest[index:index + 5] for index in range(0, 20, 5))


def executable_directory() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[1]


def license_paths() -> tuple[Path, ...]:
    local = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    candidates = (
        executable_directory() / "license.key",
        local / "UniversalLidarViewer" / "license.key",
    )
    return tuple(dict.fromkeys(candidates))


def verify_license_text(text: str, expected_machine: str | None = None) -> LicenseResult:
    expected_machine = expected_machine or machine_code()
    try:
        prefix, payload_text, signature_text = text.strip().split(".")
        if prefix != LICENSE_PREFIX:
            return LicenseResult(False, "许可证版本不受支持")
        payload_bytes = _decode64(payload_text)
        signature = _decode64(signature_text)
        public_key = Ed25519PublicKey.from_public_bytes(_decode64(PUBLIC_KEY_B64))
        public_key.verify(signature, payload_bytes)
        payload = json.loads(payload_bytes.decode("utf-8"))
    except (ValueError, UnicodeDecodeError, json.JSONDecodeError, InvalidSignature):
        return LicenseResult(False, "许可证签名无效或内容损坏")
    if payload.get("product") != PRODUCT:
        return LicenseResult(False, "许可证不属于本产品")
    if payload.get("machine") != expected_machine:
        return LicenseResult(False, "许可证与本机机器码不匹配")
    expires = payload.get("expires")
    if expires:
        try:
            expiry = datetime.fromisoformat(str(expires).replace("Z", "+00:00"))
        except ValueError:
            return LicenseResult(False, "许可证有效期格式错误")
        if datetime.now(timezone.utc) >= expiry:
            return LicenseResult(False, "许可证已过期", payload)
    return LicenseResult(True, "许可证有效", payload)


def find_license() -> LicenseResult:
    last = LicenseResult(False, "未找到许可证")
    for path in license_paths():
        if not path.is_file():
            continue
        try:
            result = verify_license_text(path.read_text(encoding="ascii"))
        except OSError as exc:
            last = LicenseResult(False, f"无法读取许可证：{exc}", path=path)
            continue
        if result.valid:
            return LicenseResult(True, result.reason, result.payload, path)
        last = LicenseResult(False, result.reason, result.payload, path)
    return last


def install_license(value: str) -> LicenseResult:
    source = Path(value)
    try:
        text = source.read_text(encoding="ascii") if source.is_file() else value
    except (OSError, ValueError):
        text = value
    result = verify_license_text(text)
    if not result.valid:
        return result
    errors = []
    for path in license_paths():
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text.strip() + "\n", encoding="ascii")
            return LicenseResult(True, "许可证安装成功", result.payload, path)
        except OSError as exc:
            errors.append(str(exc))
    return LicenseResult(False, "许可证无法保存：" + "; ".join(errors), result.payload)


def write_machine_code_file() -> Path:
    for directory in (executable_directory(), license_paths()[-1].parent):
        path = directory / "machine-code.txt"
        try:
            directory.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "Universal LiDAR Viewer 一机一码授权申请\n"
                f"机器码：{machine_code()}\n"
                "请将本文件交给许可证管理员，收到 license.key 后放到程序同目录。\n",
                encoding="utf-8",
            )
            return path
        except OSError:
            continue
    return executable_directory() / "machine-code.txt"


def show_license_error(reason: str) -> None:
    code_file = write_machine_code_file()
    message = (
        f"{reason}\n\n机器码：{machine_code()}\n\n"
        f"机器码文件：{code_file}\n"
        "请取得对应的 license.key，放到程序同目录后重新启动。"
    )
    if os.name == "nt":
        ctypes.windll.user32.MessageBoxW(None, message, "Universal LiDAR Viewer 授权", 0x10)
    else:
        print(message, file=sys.stderr)
