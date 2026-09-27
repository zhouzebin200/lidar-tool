"""Small linuxptp supervisor used by the OpenWrt web gateway."""

from __future__ import annotations

from collections import deque
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time


def network_interfaces() -> list[dict]:
    result: list[dict] = []
    for _index, name in socket.if_nameindex():
        if name == "lo":
            continue
        sysfs = Path("/sys/class/net") / name
        phc = ""
        try:
            ptp_root = sysfs / "device" / "ptp"
            devices = sorted(ptp_root.iterdir()) if ptp_root.is_dir() else []
            if devices:
                phc = f"/dev/{devices[0].name}"
        except OSError:
            pass
        state = "unknown"
        try:
            state = (sysfs / "operstate").read_text(encoding="ascii").strip()
        except OSError:
            pass
        result.append({"name": name, "state": state, "phc": phc, "hardware_timestamping": bool(phc)})
    return result


class PtpController:
    """Configure and supervise ptp4l/phc2sys without invoking a shell."""

    def __init__(self, config_path: Path, runtime_directory: Path | None = None) -> None:
        self.config_path = config_path
        self.runtime_directory = runtime_directory or Path("/var/run/ulv")
        self._config = {
            "enabled": False,
            "interface": "",
            "role": "master",
            "transport": "L2",
            "timestamping": "auto",
            "domain": 0,
            "priority1": 128,
            "delay_mechanism": "E2E",
        }
        self._ptp4l: subprocess.Popen | None = None
        self._phc2sys: subprocess.Popen | None = None
        self._logs: deque[str] = deque(maxlen=80)
        self._lock = threading.Lock()
        self._started_ns = 0
        self._error = ""
        self._effective_timestamping = ""
        self._load()

    def _load(self) -> None:
        try:
            self._config = self.validate(json.loads(self.config_path.read_text(encoding="utf-8")))
        except (OSError, ValueError, json.JSONDecodeError):
            pass

    @staticmethod
    def validate(value: dict) -> dict:
        enabled = bool(value.get("enabled", False))
        interface = str(value.get("interface", "")).strip()
        names = {item[1] for item in socket.if_nameindex()}
        if enabled and interface not in names:
            raise ValueError("请选择有效的 PTP 网口")
        role = str(value.get("role", "master"))
        if role not in ("master", "client"):
            raise ValueError("PTP 模式必须是主时钟或从时钟")
        transport = str(value.get("transport", "L2"))
        if transport not in ("L2", "UDPv4"):
            raise ValueError("PTP 传输类型必须是 L2 或 UDPv4")
        timestamping = str(value.get("timestamping", "auto"))
        if timestamping not in ("auto", "hardware", "software"):
            raise ValueError("PTP 时间戳模式无效")
        delay = str(value.get("delay_mechanism", "E2E"))
        if delay not in ("E2E", "P2P", "Auto"):
            raise ValueError("PTP 延迟机制无效")
        try:
            domain = int(value.get("domain", 0))
            priority = int(value.get("priority1", 128))
        except (TypeError, ValueError) as exc:
            raise ValueError("PTP Domain 和优先级必须是整数") from exc
        if domain < 0 or domain > 127:
            raise ValueError("PTP Domain 必须在 0 到 127 之间")
        if priority < 0 or priority > 255:
            raise ValueError("PTP Priority1 必须在 0 到 255 之间")
        return {
            "enabled": enabled,
            "interface": interface,
            "role": role,
            "transport": transport,
            "timestamping": timestamping,
            "domain": domain,
            "priority1": priority,
            "delay_mechanism": delay,
        }

    def configure(self, value: dict, persist: bool = True) -> dict:
        config = self.validate(value)
        self.stop()
        self._config = config
        if persist:
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.config_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            temporary.replace(self.config_path)
        if config["enabled"]:
            self.start()
        return self.status()

    def start_if_enabled(self) -> None:
        if self._config["enabled"]:
            try:
                self.start()
            except (OSError, ValueError) as exc:
                self._error = str(exc)

    def _has_phc(self, interface: str) -> bool:
        return any(item["name"] == interface and item["hardware_timestamping"] for item in network_interfaces())

    def _reader(self, process: subprocess.Popen, label: str) -> None:
        if process.stdout is None:
            return
        for line in process.stdout:
            text = line.rstrip()
            if text:
                with self._lock:
                    self._logs.append(f"{label}: {text}")

    def start(self) -> None:
        self.stop()
        config = self._config
        if not config["enabled"]:
            return
        ptp4l = shutil.which("ptp4l")
        if not ptp4l:
            raise OSError("未安装 ptp4l；OpenWrt 请先安装 linuxptp")
        interface = config["interface"]
        if interface not in {item[1] for item in socket.if_nameindex()}:
            raise ValueError("配置的 PTP 网口当前不存在")
        has_phc = self._has_phc(interface)
        timestamping = config["timestamping"]
        if timestamping == "hardware" and not has_phc:
            raise ValueError("所选网口没有可用 PHC，无法启用硬件时间戳")
        effective = "hardware" if timestamping == "hardware" or (timestamping == "auto" and has_phc) else "software"
        self.runtime_directory.mkdir(parents=True, exist_ok=True)
        runtime_config = self.runtime_directory / "ptp4l.conf"
        role_option = "serverOnly 1" if config["role"] == "master" else "clientOnly 1"
        runtime_config.write_text(
            "[global]\n"
            f"domainNumber {config['domain']}\n"
            f"priority1 {config['priority1']}\n"
            f"network_transport {config['transport']}\n"
            f"delay_mechanism {config['delay_mechanism']}\n"
            f"{role_option}\n"
            "logging_level 6\n"
            "use_syslog 0\n"
            "verbose 1\n",
            encoding="ascii",
        )
        command = [ptp4l, "-i", interface, "-f", str(runtime_config), "-m", "-H" if effective == "hardware" else "-S"]
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        self._error = ""
        self._logs.clear()
        self._ptp4l = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            bufsize=1, creationflags=flags,
        )
        threading.Thread(target=self._reader, args=(self._ptp4l, "ptp4l"), daemon=True).start()
        self._effective_timestamping = effective
        self._started_ns = time.time_ns()
        # Software timestamping already disciplines/uses CLOCK_REALTIME.  A
        # separate PHC synchronizer is required only for hardware timestamping.
        if effective == "hardware":
            phc2sys = shutil.which("phc2sys")
            if not phc2sys:
                self.stop()
                raise OSError("硬件时间戳需要 phc2sys；请安装完整 linuxptp 包")
            phc_command = [phc2sys, "-a", "-m", "-r"]
            if config["role"] == "master":
                phc_command.append("-r")
            self._phc2sys = subprocess.Popen(
                phc_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                bufsize=1, creationflags=flags,
            )
            threading.Thread(target=self._reader, args=(self._phc2sys, "phc2sys"), daemon=True).start()
        time.sleep(0.08)
        if self._ptp4l.poll() is not None:
            error = "ptp4l 启动失败"
            if self._logs:
                error += "：" + self._logs[-1]
            self.stop()
            raise OSError(error)

    @staticmethod
    def _terminate(process: subprocess.Popen | None) -> None:
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)

    def stop(self) -> dict:
        self._terminate(self._phc2sys)
        self._terminate(self._ptp4l)
        self._phc2sys = None
        self._ptp4l = None
        self._effective_timestamping = ""
        return self.status()

    def status(self) -> dict:
        ptp_code = self._ptp4l.poll() if self._ptp4l else None
        phc_code = self._phc2sys.poll() if self._phc2sys else None
        if self._ptp4l and ptp_code is not None and not self._error:
            self._error = f"ptp4l 已退出，代码 {ptp_code}"
        active = bool(self._ptp4l and ptp_code is None)
        return {
            "available": bool(shutil.which("ptp4l")) and sys.platform.startswith("linux"),
            "config": dict(self._config),
            "active": active,
            "state": "running" if active else "error" if self._error else "stopped",
            "error": self._error,
            "effective_timestamping": self._effective_timestamping,
            "uptime_seconds": round((time.time_ns() - self._started_ns) / 1e9, 1) if active else 0,
            "ptp4l_pid": self._ptp4l.pid if active else None,
            "phc2sys_pid": self._phc2sys.pid if self._phc2sys and phc_code is None else None,
            "interfaces": network_interfaces(),
            "logs": list(self._logs)[-30:],
        }

    def close(self) -> None:
        self.stop()
