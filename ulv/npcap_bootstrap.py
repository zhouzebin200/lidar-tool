"""Offline, interactive Npcap installation before capture handles are opened."""
from pathlib import Path
import ctypes
import hashlib
import json
import re
import subprocess
import sys

INSTALLER_SHA256 = '682b57e1c05ef375931975c9d6ded40888f4bfdf9f4b11571e654f962f1199b6'
MIN_NPCAP_VERSION = (1, 0)


def installed_version(state: dict) -> tuple[int, int]:
    match = re.match(r'^(\d+)\.(\d+)(?:\D|$)', str(state.get('version', '')))
    return tuple(map(int, match.groups())) if match else (0, 0)


def is_usable(state: dict) -> bool:
    if not state.get('dll_present') or not state.get('running'):
        return False
    v = installed_version(state)
    return v >= MIN_NPCAP_VERSION if v != (0, 0) else True


def probe() -> dict:
    script = r'''
$ErrorActionPreference='Stop'
$path=Join-Path $env:SystemRoot 'System32\Npcap\wpcap.dll'
$v=''
foreach ($key in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NpcapInst','HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\NpcapInst')) {
  try { $v=(Get-ItemProperty $key -ErrorAction Stop).DisplayVersion; if ($v) { break } } catch {}
}
$s=Get-Service npcap -ErrorAction SilentlyContinue
@{version=[string]$v;dll_present=[bool](Test-Path $path);running=($null -ne $s -and $s.Status -eq 'Running')} | ConvertTo-Json -Compress
'''
    try:
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
                                capture_output=True, text=True, timeout=30,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        return json.loads(result.stdout) if result.returncode == 0 else {}
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return {}


def notify(message: str) -> None:
    ctypes.windll.user32.MessageBoxW(None, message, 'Npcap 安装', 0x30)


def ensure_npcap(root: Path) -> bool:
    if sys.platform != 'win32':
        return True
    state = probe()
    if is_usable(state):
        return True
    if installed_version(state) >= MIN_NPCAP_VERSION:
        notify('Npcap 已安装，但驱动或组件不可用。请重启电脑或修复已安装的 Npcap；不会覆盖现有版本。')
        return False
    installer = root / 'vendor' / 'npcap' / 'npcap-1.90.exe'
    try:
        if hashlib.sha256(installer.read_bytes()).hexdigest() != INSTALLER_SHA256:
            raise ValueError('安装程序完整性校验失败')
        # Free edition requires its normal GUI and license acceptance. Never pass /S.
        result = subprocess.run([str(installer), '/no_kill=yes'], check=False)
        if result.returncode in (3010, 350):
            notify('Npcap 需要重新启动电脑。请重启后再次打开雷达工具；可先在界面使用普通 UDP 模式。')
            return False
        if is_usable(probe()):
            return True
        notify('Npcap 尚未安装完成或驱动不可用。可在界面使用普通 UDP 模式；下次启动会再次检测。')
    except (OSError, ValueError) as error:
        notify(f'无法安装 Npcap：{error}。可在界面使用普通 UDP 模式。')
    return False
