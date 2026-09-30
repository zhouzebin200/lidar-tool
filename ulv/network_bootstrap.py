"""Recoverable host-network setup for VLAN-gated LiDAR streams."""

from __future__ import annotations

import base64
import ctypes
import ipaddress
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
try:
    import winreg
except ImportError:
    winreg = None


_GUID_RE = re.compile(r"^[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}$")
_KEYWORD_RE = re.compile(r"^[A-Za-z0-9_*.-]{1,128}$")


def _windows_interface_guid(pcap_name: str) -> str:
    match = re.search(r"\{([0-9A-Fa-f-]{36})\}", pcap_name)
    return match.group(1) if match and _GUID_RE.fullmatch(match.group(1)) else ""


def windows_is_admin() -> bool:
    if sys.platform != "win32":
        return True
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
        return False


def ensure_windows_admin() -> bool:
    """Relaunch an unpackaged Windows run once with administrator rights.

    The packaged executable has a requireAdministrator manifest, so this is
    principally for ``python run.py`` and never runs a second helper later.
    """
    if sys.platform != "win32" or windows_is_admin():
        return True
    if getattr(sys, "frozen", False):
        executable = sys.executable
        arguments = sys.argv[1:]
    else:
        executable = sys.executable
        arguments = [str(Path(sys.argv[0]).resolve()), *sys.argv[1:]]
    parameters = subprocess.list2cmdline(arguments)
    try:
        result = ctypes.windll.shell32.ShellExecuteW(
            None, "runas", executable, parameters, None, 1,
        )
    except (AttributeError, OSError):
        return False
    if int(result) > 32:
        sys.exit(0)
    return False


def ensure_macos_admin() -> bool:
    """Relaunch once through the macOS administrator authorization dialog.

    libpcap needs access to the BPF devices in order to discover LiDAR streams
    sent to arbitrary UDP ports.  A bundled Finder-launched app has no terminal
    in which to ask for ``sudo``, so use the native authorization dialog and
    leave the relaunched process running in the background.
    """
    if sys.platform != "darwin" or os.geteuid() == 0:
        return True
    if getattr(sys, "frozen", False):
        command = [sys.executable, *sys.argv[1:]]
    else:
        command = [sys.executable, str(Path(sys.argv[0]).resolve()), *sys.argv[1:]]
    log_path = "/tmp/universal-lidar-viewer.log"
    # ``nohup`` fails with "can't detach from console" for a Finder-launched
    # app on newer macOS releases. AppleScript's shell can safely detach a
    # background child once all three standard streams are redirected.
    shell_command = (
        " ".join(shlex.quote(item) for item in command)
        + " </dev/null >" + shlex.quote(log_path) + " 2>&1 &"
    )
    script = (
        "on run argv\n"
        "do shell script (item 1 of argv) with administrator privileges\n"
        "end run"
    )
    try:
        result = subprocess.run(
            ["/usr/bin/osascript", "-e", script, shell_command],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    if result.returncode == 0:
        requested_port = 17777
        arguments = sys.argv[1:]
        for index, value in enumerate(arguments):
            if value == "--port" and index + 1 < len(arguments):
                try:
                    requested_port = int(arguments[index + 1])
                except ValueError:
                    pass
        candidate_ports = [requested_port]
        if requested_port == 17777:
            candidate_ports.extend((18888, 27777, 37777))
        url = None
        for _ in range(80):
            for port in candidate_ports:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        url = f"http://127.0.0.1:{port}"
                        break
                except OSError:
                    continue
            if url:
                break
            time.sleep(0.1)
        if url:
            try:
                subprocess.Popen(
                    ["/usr/bin/open", url],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except OSError:
                pass
        raise SystemExit(0)
    return False


def _powershell_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


class MacOSNetworkBootstrapManager:
    """Add recoverable IPv4 aliases without changing DHCP or primary IP."""

    def __init__(self, journal_path: Path) -> None:
        self.journal_path = journal_path
        self._lock = threading.RLock()

    @staticmethod
    def _run(arguments: list[str]) -> tuple[bool, str]:
        try:
            result = subprocess.run(arguments, capture_output=True, text=True, timeout=10, check=False)
            return result.returncode == 0, (result.stdout if not result.returncode else result.stderr).strip()
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, str(exc)

    def _interface(self, name: str) -> tuple[bool, dict | str]:
        if not re.fullmatch(r"en\d+", name):
            return False, "only physical Ethernet interfaces are supported"
        ok, ports = self._run(["/usr/sbin/networksetup", "-listallhardwareports"])
        if not ok:
            return False, ports
        blocks = ports.split("\n\n")
        block = next((b for b in blocks if re.search(r"^Device: " + re.escape(name) + r"$", b, re.M)), "")
        label = block.splitlines()[0].casefold() if block else ""
        if not any(word in label for word in ("ethernet", "usb", "lan")) or any(
                word in label for word in ("wi-fi", "wifi", "wireless", "bridge")):
            return False, "automatic adaptation requires a physical wired Ethernet/USB adapter"
        ok, output = self._run(["/sbin/ifconfig", name])
        if not ok:
            return False, output
        mac = re.search(r"\bether ([0-9a-fA-F:]{17})", output)
        if not mac:
            return False, "adapter hardware address is unavailable"
        return True, {"interface": name, "mac": mac.group(1).lower(),
                      "addresses": re.findall(r"\binet (\d+\.\d+\.\d+\.\d+)\b", output)}

    def _records(self) -> list[dict]:
        if not self.journal_path.exists():
            return []
        value = json.loads(self.journal_path.read_text(encoding="utf-8"))
        if value.get("platform") != "darwin" or not isinstance(value.get("records"), list):
            raise ValueError("invalid macOS network recovery journal")
        for item in value["records"]:
            if (not re.fullmatch(r"en\d+", str(item.get("interface", "")))
                    or not re.fullmatch(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}", str(item.get("mac", "")))
                    or ipaddress.IPv4Address(item["requested_ip"]).is_loopback):
                raise ValueError("invalid macOS recovery record")
        return value["records"]

    def _write(self, records: list[dict]) -> None:
        self.journal_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.journal_path.with_suffix(".tmp")
        temporary.write_text(json.dumps({"platform": "darwin", "records": records}, indent=2), encoding="utf-8")
        temporary.chmod(0o600)
        temporary.replace(self.journal_path)

    def apply(self, pcap_interface_name: str, radar_ip: str,
              requested_host_ip: str, vlan_id: int) -> tuple[bool, str]:
        if sys.platform != "darwin":
            return False, "macOS network adaptation is unavailable on this platform"
        if os.geteuid() != 0:
            return False, "需要管理员权限；请重新启动程序并允许 macOS 授权"
        if vlan_id != 0:
            return False, "macOS 自动 IP 适配当前支持无 VLAN 的直连雷达"
        try:
            radar = ipaddress.IPv4Address(radar_ip)
            target = ipaddress.IPv4Address(requested_host_ip)
            if (not (radar.is_private or radar.is_link_local) or radar.is_loopback
                    or target not in ipaddress.ip_network(f"{radar}/24", strict=False)
                    or target == radar or (int(target) & 255) in (0, 1, 255)):
                return False, "invalid radar destination address"
        except ipaddress.AddressValueError as exc:
            return False, str(exc)
        with self._lock:
            try:
                records = self._records()
                ok, adapter = self._interface(pcap_interface_name)
                if not ok:
                    return False, str(adapter)
                if str(target) in adapter["addresses"]:
                    return True, f"{pcap_interface_name} 已具有目标地址 {target}"
                ok, all_interfaces = self._run(["/sbin/ifconfig", "-a"])
                if not ok:
                    return False, all_interfaces
                if str(target) in re.findall(r"\binet (\d+\.\d+\.\d+\.\d+)\b", all_interfaces):
                    return False, "目标 IP 已被其他本机网卡使用"
                record = {"interface": pcap_interface_name, "mac": adapter["mac"],
                          "requested_ip": str(target), "original_ipv4": adapter["addresses"]}
                # Persist ownership before the OS mutation, including failed
                # or interrupted commands. Recovery only removes our alias.
                records.append(record)
                self._write(records)
                ok, output = self._run(["/sbin/ifconfig", pcap_interface_name, "alias", str(target),
                                        "netmask", "255.255.255.0"])
                if not ok:
                    return False, output
                return True, f"已为 {pcap_interface_name} 添加临时地址 {target}/24；退出时移除"
            except (OSError, ValueError, KeyError, TypeError) as exc:
                return False, str(exc)

    def restore_all(self) -> tuple[bool, str]:
        with self._lock:
            try:
                records = self._records()
                if not records:
                    return True, ""
                if os.geteuid() != 0:
                    return False, "恢复临时网卡地址需要管理员权限"
                remaining = []
                errors = []
                for record in records:
                    ok, adapter = self._interface(record["interface"])
                    if not ok or adapter["mac"] != record["mac"]:
                        remaining.append(record)
                        errors.append("原 USB 网卡不可用，保留恢复记录")
                        continue
                    address = record["requested_ip"]
                    if address in record.get("original_ipv4", []):
                        remaining.append(record)
                        errors.append("恢复记录包含原有 IP，拒绝删除")
                        continue
                    if address in adapter["addresses"]:
                        ok, output = self._run(["/sbin/ifconfig", record["interface"], "-alias", address])
                        if not ok:
                            remaining.append(record)
                            errors.append(output)
                if remaining:
                    self._write(remaining)
                    return False, "; ".join(errors)
                self.journal_path.unlink(missing_ok=True)
                return True, "已移除临时 IP，原有网卡地址和 DHCP 设置保持不变"
            except (OSError, ValueError, KeyError, TypeError) as exc:
                return False, str(exc)

    def recover_pending(self) -> tuple[bool, str]:
        return self.restore_all()


class WindowsNetworkBootstrapManager:
    """Own temporary NIC VLAN/IP settings for one application lifetime."""

    def __init__(self, journal_path: Path) -> None:
        self.journal_path = journal_path
        self._lock = threading.RLock()

    @staticmethod
    def _run_powershell(script: str, timeout: int = 45) -> tuple[bool, str]:
        encoded = base64.b64encode(script.encode("utf-16le")).decode("ascii")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            result = subprocess.run(
                [
                    "powershell.exe", "-NoProfile", "-NonInteractive",
                    "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded,
                ],
                capture_output=True,
                text=True,
                timeout=timeout,
                creationflags=flags,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, str(exc)
        output = (result.stdout or "").strip()
        error = (result.stderr or "").strip()
        if result.returncode:
            return False, error or output or f"PowerShell exited with code {result.returncode}"
        return True, output

    @staticmethod
    def _run_command(arguments: list[str], timeout: int = 15) -> tuple[bool, str]:
        """Run a Windows networking command without starting PowerShell."""
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            result = subprocess.run(
                arguments, capture_output=True, text=True, timeout=timeout,
                creationflags=flags, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, str(exc)
        output = (result.stdout or "").strip()
        error = (result.stderr or "").strip()
        if result.returncode:
            return False, error or output or f"command exited with code {result.returncode}"
        return True, output

    def _load_records(self) -> list[dict]:
        if not self.journal_path.is_file():
            return []
        try:
            document = json.loads(self.journal_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, json.JSONDecodeError):
            return []
        records = document.get("adapters", []) if isinstance(document, dict) else []
        return records if isinstance(records, list) else []

    def _write_records(self, records: list[dict]) -> None:
        self.journal_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.journal_path.with_suffix(self.journal_path.suffix + ".tmp")
        temporary.write_text(
            json.dumps({"version": 1, "adapters": records}, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        temporary.replace(self.journal_path)

    @staticmethod
    def _validate_record(record: dict) -> dict:
        guid = str(record.get("guid", ""))
        keyword = str(record.get("vlan_keyword", ""))
        priority_keyword = str(record.get("priority_keyword", ""))
        requested_ip = str(ipaddress.ip_address(str(record.get("requested_ip", ""))))
        original_ips = [
            str(ipaddress.ip_address(str(value)))
            for value in record.get("original_ipv4", [])
        ]
        original_vlan = int(record.get("original_vlan"))
        active_vlan = int(record.get("active_vlan"))
        original_priority = int(record.get("original_priority", 0))
        dhcp_enabled = bool(record.get("dhcp_enabled", False))
        vlan_changed = bool(record.get("vlan_changed", True))
        if (
            not _GUID_RE.fullmatch(guid)
            or not _KEYWORD_RE.fullmatch(keyword)
            or (priority_keyword and not _KEYWORD_RE.fullmatch(priority_keyword))
            or not 0 <= original_vlan <= 4094
            or not 0 <= active_vlan <= 4094
        ):
            raise ValueError("invalid network recovery journal")
        return {
            "guid": guid,
            "vlan_keyword": keyword,
            "priority_keyword": priority_keyword,
            "requested_ip": requested_ip,
            "original_ipv4": original_ips,
            "original_vlan": original_vlan,
            "active_vlan": active_vlan,
            "original_priority": original_priority,
            "dhcp_enabled": dhcp_enabled,
            "vlan_changed": vlan_changed,
        }

    def _query_adapter(self, guid: str) -> tuple[bool, dict | str]:
        script = rf"""
$ErrorActionPreference = 'Stop'
$adapter = Get-NetAdapter | Where-Object {{ $_.InterfaceGuid.Trim('{{}}') -eq '{guid}' }} | Select-Object -First 1
if ($null -eq $adapter) {{ throw 'network adapter was not found' }}
$vlanProperty = Get-NetAdapterAdvancedProperty -Name $adapter.Name | Where-Object {{
  $_.RegistryKeyword -match '(?i)vlan' -and $_.RegistryKeyword -notmatch '(?i)priority'
}} | Select-Object -First 1
if ($null -eq $vlanProperty) {{ throw 'network adapter has no configurable VLAN ID property' }}
$priorityProperty = Get-NetAdapterAdvancedProperty -Name $adapter.Name | Where-Object {{
  $_.RegistryKeyword -match '(?i)priority.*vlan|vlan.*priority'
}} | Select-Object -First 1
$addresses = @(Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
  ForEach-Object {{ $_.IPAddress }})
$ipInterface = Get-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
[pscustomobject]@{{
  guid = '{guid}'
  vlan_keyword = $vlanProperty.RegistryKeyword
  original_vlan = [int](@($vlanProperty.RegistryValue)[0])
  priority_keyword = if ($null -eq $priorityProperty) {{ '' }} else {{ $priorityProperty.RegistryKeyword }}
  original_priority = if ($null -eq $priorityProperty) {{ 0 }} else {{ [int](@($priorityProperty.RegistryValue)[0]) }}
  dhcp_enabled = [bool]($ipInterface.Dhcp -eq 'Enabled')
  original_ipv4 = $addresses
}} | ConvertTo-Json -Compress
"""
        success, output = self._run_powershell(script)
        if not success:
            return False, output
        try:
            value = json.loads(output)
            keyword = str(value["vlan_keyword"])
            priority_keyword = str(value.get("priority_keyword", ""))
            if not _KEYWORD_RE.fullmatch(keyword):
                raise ValueError("invalid VLAN property")
            if priority_keyword and not _KEYWORD_RE.fullmatch(priority_keyword):
                raise ValueError("invalid priority/VLAN property")
            addresses = value.get("original_ipv4", [])
            if isinstance(addresses, str):
                addresses = [addresses]
            return True, {
                "guid": guid,
                "vlan_keyword": keyword,
                "original_vlan": int(value["original_vlan"]),
                "priority_keyword": priority_keyword,
                "original_priority": int(value.get("original_priority", 0)),
                "dhcp_enabled": bool(value.get("dhcp_enabled", False)),
                "original_ipv4": [str(ipaddress.ip_address(item)) for item in addresses],
            }
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
            return False, f"could not read network adapter state: {exc}"

    @staticmethod
    def _query_adapter_registry(guid: str) -> tuple[bool, dict | str]:
        """Read the pre-change adapter snapshot without loading PowerShell modules."""
        if sys.platform != "win32" or winreg is None:
            return False, "Windows registry is unavailable"
        class_root = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}"
        adapter_path = ""
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, class_root) as root:
                index = 0
                while True:
                    try:
                        child = winreg.EnumKey(root, index)
                    except OSError:
                        break
                    index += 1
                    try:
                        with winreg.OpenKey(root, child) as key:
                            instance = str(winreg.QueryValueEx(key, "NetCfgInstanceId")[0]).strip("{}")
                            if instance.casefold() == guid.casefold():
                                adapter_path = class_root + "\\" + child
                                break
                    except OSError:
                        continue
            if not adapter_path:
                return False, "network adapter registry entry was not found"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, adapter_path) as key:
                original_vlan = int(winreg.QueryValueEx(key, "RegVlanid")[0])
                try:
                    original_priority = int(winreg.QueryValueEx(key, "*PriorityVLANTag")[0])
                    priority_keyword = "*PriorityVLANTag"
                except OSError:
                    original_priority = 0
                    priority_keyword = ""
            connection_path = (
                r"SYSTEM\CurrentControlSet\Control\Network\{4d36e972-e325-11ce-bfc1-08002be10318}"
                rf"\{{{guid}}}\Connection"
            )
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, connection_path) as key:
                interface_name = str(winreg.QueryValueEx(key, "Name")[0])
            tcpip_path = rf"SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{{{guid}}}"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, tcpip_path) as key:
                dhcp_enabled = bool(int(winreg.QueryValueEx(key, "EnableDHCP")[0]))
                address_value = "DhcpIPAddress" if dhcp_enabled else "IPAddress"
                addresses = winreg.QueryValueEx(key, address_value)[0]
            if isinstance(addresses, str):
                addresses = [addresses]
            original_ipv4 = [
                str(ipaddress.ip_address(value)) for value in addresses
                if value and value != "0.0.0.0"
            ]
            if not original_ipv4:
                return False, "adapter has no usable IPv4 address in registry"
            return True, {
                "guid": guid,
                "vlan_keyword": "RegVlanid",
                "original_vlan": original_vlan,
                "priority_keyword": priority_keyword,
                "original_priority": original_priority,
                "dhcp_enabled": dhcp_enabled,
                "original_ipv4": original_ipv4,
                "adapter_registry_path": adapter_path,
                "interface_name": interface_name,
            }
        except (OSError, TypeError, ValueError) as exc:
            return False, f"could not read adapter registry snapshot: {exc}"

    def _apply_fast(self, record: dict, requested_host_ip: str, vlan_id: int) -> tuple[bool, str]:
        """Apply VLAN/IP through the registry and netsh, avoiding NetAdapter startup."""
        if winreg is None:
            return False, "Windows registry is unavailable"
        adapter_path = str(record.get("adapter_registry_path", ""))
        interface_name = str(record.get("interface_name", ""))
        if not adapter_path or not interface_name:
            return False, "fast adapter metadata is unavailable"
        disabled = False
        try:
            if record["vlan_changed"]:
                with winreg.OpenKey(
                    winreg.HKEY_LOCAL_MACHINE, adapter_path, 0,
                    winreg.KEY_QUERY_VALUE | winreg.KEY_SET_VALUE,
                ) as key:
                    _, vlan_type = winreg.QueryValueEx(key, record["vlan_keyword"])
                    vlan_value = str(vlan_id) if vlan_type in (winreg.REG_SZ, winreg.REG_EXPAND_SZ) else vlan_id
                    winreg.SetValueEx(key, record["vlan_keyword"], 0, vlan_type, vlan_value)
                    priority_keyword = record.get("priority_keyword", "")
                    if priority_keyword and vlan_id > 0:
                        _, priority_type = winreg.QueryValueEx(key, priority_keyword)
                        priority_value = "3" if priority_type in (winreg.REG_SZ, winreg.REG_EXPAND_SZ) else 3
                        winreg.SetValueEx(key, priority_keyword, 0, priority_type, priority_value)
                    winreg.FlushKey(key)

            if record["vlan_changed"]:
                success, output = self._run_command([
                    "netsh", "interface", "set", "interface",
                    f"name={interface_name}", "admin=disabled",
                ])
                if not success:
                    return False, f"could not disable adapter for VLAN reload: {output}"
                disabled = True
                success, output = self._run_command([
                    "netsh", "interface", "set", "interface",
                    f"name={interface_name}", "admin=enabled",
                ])
                if not success:
                    return False, f"could not re-enable adapter after VLAN reload: {output}"
                disabled = False

            # Some drivers clear active-only addresses during their NDIS
            # restart, so install the temporary address after the VLAN reload.
            if requested_host_ip not in record["original_ipv4"]:
                success, output = self._run_command([
                    "netsh", "interface", "ipv4", "add", "address",
                    f"name={interface_name}", f"address={requested_host_ip}",
                    "mask=255.255.255.0", "store=active",
                ])
                if not success:
                    return False, f"could not add temporary IPv4 address: {output}"
            return True, ""
        except (KeyError, OSError, TypeError, ValueError) as exc:
            return False, str(exc)
        finally:
            if disabled:
                self._run_command([
                    "netsh", "interface", "set", "interface",
                    f"name={interface_name}", "admin=enabled",
                ])

    def _restore_records(self, records: list[dict]) -> tuple[bool, str]:
        errors: list[str] = []
        for raw_record in records:
            try:
                record = self._validate_record(raw_record)
            except (TypeError, ValueError) as exc:
                errors.append(str(exc))
                continue
            guid = record["guid"]
            keyword = _powershell_string(record["vlan_keyword"])
            requested_ip = _powershell_string(record["requested_ip"])
            original_vlan = record["original_vlan"]
            priority_keyword = record["priority_keyword"]
            original_priority = record["original_priority"]
            restore_vlan = ""
            if record["vlan_changed"]:
                restore_priority = ""
                if priority_keyword:
                    restore_priority = (
                        "Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword "
                        f"{_powershell_string(priority_keyword)} -RegistryValue {original_priority} -NoRestart\n"
                    )
                restore_vlan = rf"""
{restore_priority}
$property = Get-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword {keyword} -ErrorAction Stop
$currentVlan = [int](@($property.RegistryValue)[0])
if ($currentVlan -ne {original_vlan}) {{
  Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword {keyword} -RegistryValue {original_vlan} -NoRestart
}}
Restart-NetAdapter -Name $adapter.Name -Confirm:$false
$deadline = (Get-Date).AddSeconds(15)
do {{ Start-Sleep -Milliseconds 250; $adapter = Get-NetAdapter -Name $adapter.Name }}
while ($adapter.Status -ne 'Up' -and (Get-Date) -lt $deadline)
if ($adapter.Status -ne 'Up') {{ throw 'network adapter did not return to Up state during restore' }}
"""
            restore_dhcp = ""
            if record["dhcp_enabled"]:
                restore_dhcp = "Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -Dhcp Enabled -ErrorAction Stop\n"
            remove_address = record["requested_ip"] not in record["original_ipv4"]
            remove_script = ""
            if remove_address:
                remove_script = rf"""
Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
  Where-Object {{ $_.IPAddress -eq {requested_ip} }} |
  Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
"""
            script = rf"""
$ErrorActionPreference = 'Stop'
$adapter = Get-NetAdapter | Where-Object {{ $_.InterfaceGuid.Trim('{{}}') -eq '{guid}' }} | Select-Object -First 1
if ($null -eq $adapter) {{ throw 'network adapter was not found during restore' }}
{remove_script}
{restore_vlan}
{restore_dhcp}
"""
            success, output = self._run_powershell(script)
            if not success:
                errors.append(output)
        if errors:
            return False, "; ".join(errors)
        try:
            self.journal_path.unlink(missing_ok=True)
        except OSError as exc:
            return False, f"network restored but recovery journal could not be removed: {exc}"
        return True, "original network adapter VLAN/IP configuration restored"

    def recover_pending(self) -> tuple[bool, str]:
        """Restore settings left behind by a prior abnormal termination."""
        if sys.platform != "win32":
            return True, ""
        with self._lock:
            records = self._load_records()
            if not records:
                return True, ""
            return self._restore_records(records)

    def apply(
        self,
        pcap_interface_name: str,
        radar_ip: str,
        requested_host_ip: str,
        vlan_id: int,
    ) -> tuple[bool, str]:
        if sys.platform != "win32":
            return False, "automatic NIC VLAN bootstrap is currently available on Windows only"
        if not windows_is_admin():
            return False, "administrator permission is required; restart the application as administrator"
        guid = _windows_interface_guid(pcap_interface_name)
        try:
            radar_ip = str(ipaddress.ip_address(radar_ip))
            requested_host_ip = str(ipaddress.ip_address(requested_host_ip))
            vlan_id = int(vlan_id)
        except (TypeError, ValueError) as exc:
            return False, f"invalid radar network request: {exc}"
        if not guid or not 0 <= vlan_id <= 4094:
            return False, "invalid capture interface or VLAN ID"

        with self._lock:
            existing = self._load_records()
            if existing:
                try:
                    current = self._validate_record(existing[0])
                except (TypeError, ValueError):
                    current = {}
                if (
                    current.get("guid") == guid
                    and current.get("requested_ip") == requested_host_ip
                    and current.get("active_vlan") == vlan_id
                ):
                    return True, f"NIC remains on VLAN {vlan_id} with {requested_host_ip}"
                success, message = self._restore_records(existing)
                if not success:
                    return False, f"could not restore previous NIC setup before switching: {message}"

            success, queried = self._query_adapter_registry(guid)
            if not success:
                success, queried = self._query_adapter(guid)
            if not success:
                return False, str(queried)
            record = dict(queried)
            radar_address = ipaddress.ip_address(radar_ip)
            same_subnet = any(
                radar_address in ipaddress.ip_network(f"{item}/24", strict=False)
                for item in record["original_ipv4"]
            )
            vlan_changed = (
                record["original_vlan"] != vlan_id
                or (vlan_id > 0 and record.get("priority_keyword")
                    and record.get("original_priority") != 3)
            )
            if same_subnet and not vlan_changed:
                return True, f"NIC already reaches {radar_ip} on VLAN {vlan_id}"
            record.update({
                "requested_ip": requested_host_ip,
                "active_vlan": vlan_id,
                "vlan_changed": bool(vlan_changed),
            })
            try:
                self._validate_record(record)
                # The journal is committed before either mutation. Removing a
                # planned-but-not-yet-added address during recovery is harmless.
                self._write_records([record])
            except (OSError, TypeError, ValueError) as exc:
                return False, f"could not create network recovery journal: {exc}"

            # Realtek and similar Windows drivers expose their VLAN setting in
            # the adapter class registry. Updating it directly and asking
            # netsh to reload the adapter avoids the several-second cold start
            # of the PowerShell NetAdapter module. The PowerShell path below
            # remains as a compatibility fallback for other drivers.
            fast_success, _ = self._apply_fast(record, requested_host_ip, vlan_id)
            if fast_success:
                return True, (
                    f"已临时设置 {requested_host_ip}/24、VLAN {vlan_id}；"
                    "程序退出时将恢复启动前的网卡配置"
                )

            keyword = _powershell_string(record["vlan_keyword"])
            vlan_setup = ""
            if vlan_changed:
                priority_keyword = record.get("priority_keyword", "")
                enable_priority = ""
                if priority_keyword and vlan_id > 0:
                    enable_priority = (
                        "Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword "
                        f"{_powershell_string(priority_keyword)} -RegistryValue 3 -NoRestart\n"
                    )
                vlan_setup = rf"""
$property = Get-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword {keyword} -ErrorAction Stop
$currentVlan = [int](@($property.RegistryValue)[0])
{enable_priority}
if ($currentVlan -ne {vlan_id}) {{
  Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword {keyword} -RegistryValue {vlan_id} -NoRestart
}}
Restart-NetAdapter -Name $adapter.Name -Confirm:$false
$deadline = (Get-Date).AddSeconds(15)
do {{ Start-Sleep -Milliseconds 250; $adapter = Get-NetAdapter -Name $adapter.Name }}
while ($adapter.Status -ne 'Up' -and (Get-Date) -lt $deadline)
if ($adapter.Status -ne 'Up') {{ throw 'network adapter did not return to Up state' }}
"""
            host = _powershell_string(requested_host_ip)
            script = rf"""
$ErrorActionPreference = 'Stop'
$adapter = Get-NetAdapter | Where-Object {{ $_.InterfaceGuid.Trim('{{}}') -eq '{guid}' }} | Select-Object -First 1
if ($null -eq $adapter) {{ throw 'network adapter was not found' }}
{vlan_setup}
$existing = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
  Where-Object {{ $_.IPAddress -eq {host} }}
if ($null -eq $existing) {{
  New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress {host} -PrefixLength 24 | Out-Null
}}
"""
            success, output = self._run_powershell(script, timeout=40)
            if not success:
                restored, restore_message = self._restore_records([record])
                suffix = f"; rollback: {restore_message}" if restore_message else ""
                if not restored:
                    suffix = f"; automatic rollback failed: {restore_message}"
                return False, f"could not configure NIC VLAN/IP: {output}{suffix}"
            return True, (
                f"已临时设置 {requested_host_ip}/24、VLAN {vlan_id}；"
                "程序退出时将恢复启动前的网卡配置"
            )

    def restore_all(self) -> tuple[bool, str]:
        if sys.platform != "win32":
            return True, ""
        with self._lock:
            records = self._load_records()
            if not records:
                return True, ""
            return self._restore_records(records)
