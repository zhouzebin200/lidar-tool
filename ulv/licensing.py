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
import tempfile
import threading
import time

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
    if getattr(sys, "frozen", False) or "__compiled__" in globals():
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
        if len(text) > 16384:
            return LicenseResult(False, "许可证内容过长")
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
    if not isinstance(payload, dict) or type(payload.get("version")) is not int or payload["version"] != 1:
        return LicenseResult(False, "许可证内容或版本无效")
    if payload.get("product") != PRODUCT:
        return LicenseResult(False, "许可证不属于本产品")
    if payload.get("machine") != expected_machine:
        return LicenseResult(False, "许可证与本机机器码不匹配")
    try:
        issued = datetime.fromisoformat(payload["issued"].replace("Z", "+00:00"))
        if issued.tzinfo is None:
            raise ValueError("timezone missing")
        if issued.timestamp() > datetime.now(timezone.utc).timestamp() + 300:
            return LicenseResult(False, "系统时间早于许可证签发时间，请校准系统时间")
        expires = payload.get("expires")
        expiry = datetime.fromisoformat(expires.replace("Z", "+00:00")) if expires is not None else None
        if expiry is not None and (expiry.tzinfo is None or expiry <= issued):
            raise ValueError("invalid expiry")
    except (ValueError, TypeError, AttributeError, KeyError, OverflowError):
        return LicenseResult(False, "许可证有效期格式错误")
    if expiry is not None:
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
        except (OSError, UnicodeError) as exc:
            last = LicenseResult(False, f"无法读取许可证：{exc}", path=path)
            continue
        if result.valid:
            return LicenseResult(True, result.reason, result.payload, path)
        last = LicenseResult(False, result.reason, result.payload, path)
    return last


def install_license(value: str) -> LicenseResult:
    try:
        source = Path(value)
        text = source.read_text(encoding="ascii") if source.is_file() else value
    except (OSError, ValueError, UnicodeError):
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


def _clock_data(data: bytes, protect: bool) -> bytes:
    if os.name != "nt":
        return data

    class Blob(ctypes.Structure):
        _fields_ = [("size", ctypes.c_uint32), ("data", ctypes.POINTER(ctypes.c_char))]

    buffer = ctypes.create_string_buffer(data)
    source = Blob(len(data), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_char)))
    output = Blob()
    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    function = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
    if not function(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(output)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return ctypes.string_at(output.data, output.size)
    finally:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        kernel32.LocalFree.restype = ctypes.c_void_p
        kernel32.LocalFree(output.data)


class LicenseSession:
    """Revalidate permissions and keep a per-user, DPAPI-protected clock watermark."""

    def __init__(self) -> None:
        self.result = LicenseResult(False, "尚未检查许可证")
        self._lock = threading.Lock()
        self._next_check = 0.0
        self._last_wall = 0.0
        self._last_monotonic = 0.0
        self._next_save = 0.0
        self._clock_path = license_paths()[-1].parent / "license-clock.bin"

    def check(self, force: bool = False) -> LicenseResult:
        with self._lock:
            tick = time.monotonic()
            if not force and tick < self._next_check:
                return self.result
            self._next_check = tick + 1
            result = find_license()
            if result.valid:
                try:
                    now = datetime.now(timezone.utc).timestamp()
                    floor = self._last_wall + max(0, tick - self._last_monotonic) if self._last_wall else 0
                    if self._clock_path.exists():
                        state = json.loads(_clock_data(self._clock_path.read_bytes(), False))
                        if state["machine"] != machine_code():
                            raise ValueError("clock machine mismatch")
                        floor = max(floor, float(state["last_seen"]))
                    if now < floor - 300:
                        result = LicenseResult(False, "检测到系统时间回拨，请校准时间后重启")
                    else:
                        self._last_wall = max(now, floor)
                        self._last_monotonic = tick
                        if tick >= self._next_save:
                            data = json.dumps({"machine": machine_code(), "last_seen": self._last_wall}).encode()
                            self._clock_path.parent.mkdir(parents=True, exist_ok=True)
                            temporary = None
                            try:
                                with tempfile.NamedTemporaryFile(dir=self._clock_path.parent, delete=False) as stream:
                                    temporary = Path(stream.name)
                                    stream.write(_clock_data(data, True))
                                temporary.replace(self._clock_path)
                            finally:
                                if temporary is not None:
                                    temporary.unlink(missing_ok=True)
                            self._next_save = tick + 60
                except (OSError, ValueError, TypeError, KeyError, UnicodeError):
                    result = LicenseResult(False, "授权时间记录无法读取或保存，请联系管理员")
            self.result = result
            return result

    def valid(self) -> bool:
        return self.check().valid

    # shortcut: deleting the watermark before restarting can defeat offline rollback history; use server leases for stronger enforcement.


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
