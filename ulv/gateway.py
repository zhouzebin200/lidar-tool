"""LiDAR UDP forwarding and removable-storage PCAP recording.

The gateway deliberately operates on detected LiDAR traffic instead of fixed
vendor ports.  This keeps forwarding/recording useful for MSOP, DIFOP and
future packet layouts while avoiding a full LAN packet recorder.
"""

from __future__ import annotations

from collections import deque
import ipaddress
import json
import os
from pathlib import Path
from queue import Empty, Full, Queue
import re
import shutil
import socket
import struct
import sys
import threading
import time
from typing import Iterable


PCAP_GLOBAL_HEADER = struct.Struct("<IHHIIII")
PCAP_RECORD_HEADER = struct.Struct("<IIII")
PCAP_MAGIC = 0xA1B2C3D4
ETHERNET_LINKTYPE = 1
_SAFE_PREFIX = re.compile(r"[^A-Za-z0-9._-]+")


def _unescape_mount(value: str) -> str:
    return value.replace("\\040", " ").replace("\\011", "\t").replace("\\134", "\\")


def storage_candidates() -> list[dict]:
    """Return writable removable/external mount points suitable for PCAPs."""
    candidates: list[tuple[str, str, str]] = []
    if sys.platform.startswith("linux"):
        try:
            for line in Path("/proc/mounts").read_text(encoding="utf-8", errors="replace").splitlines():
                fields = line.split()
                if len(fields) < 4:
                    continue
                device, mountpoint, fs_type, options = fields[:4]
                mountpoint = _unescape_mount(mountpoint)
                # OpenWrt mountd/block-mount normally uses one of these roots.
                # Also accept /overlay mounted from a USB block device, but do
                # not offer internal pseudo filesystems or the root filesystem.
                external_path = mountpoint.startswith(("/mnt/", "/media/", "/tmp/mount", "/tmp/run/mount"))
                block_device = device.startswith(("/dev/sd", "/dev/mmcblk", "/dev/nvme")) and mountpoint != "/"
                if (external_path or block_device) and "rw" in options.split(","):
                    candidates.append((device, mountpoint, fs_type))
        except OSError:
            pass
    elif os.name == "nt":
        # Useful for developing/testing the same web workflow on Windows.
        for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
            root = f"{letter}:\\"
            if os.path.isdir(root):
                candidates.append((root, root, "windows-volume"))

    result: list[dict] = []
    seen: set[str] = set()
    for device, mountpoint, fs_type in candidates:
        try:
            resolved = str(Path(mountpoint).resolve())
            if resolved in seen or not os.access(resolved, os.W_OK):
                continue
            usage = shutil.disk_usage(resolved)
        except OSError:
            continue
        seen.add(resolved)
        result.append({
            "device": device,
            "mountpoint": resolved,
            "filesystem": fs_type,
            "total_bytes": usage.total,
            "free_bytes": usage.free,
        })
    return sorted(result, key=lambda item: item["mountpoint"])


def system_snapshot() -> dict:
    """Small dependency-free system summary for the OpenWrt dashboard."""
    total_kb = available_kb = 0
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            key, raw = line.split(":", 1)
            value = int(raw.strip().split()[0])
            if key == "MemTotal":
                total_kb = value
            elif key == "MemAvailable":
                available_kb = value
    except (OSError, ValueError, IndexError):
        pass
    try:
        load = [round(item, 2) for item in os.getloadavg()]
    except (AttributeError, OSError):
        load = []
    uptime_seconds = 0.0
    try:
        uptime_seconds = float(Path("/proc/uptime").read_text(encoding="ascii").split()[0])
    except (OSError, ValueError, IndexError):
        pass
    try:
        root = shutil.disk_usage("/")
        root_storage = {"total_bytes": root.total, "free_bytes": root.free}
    except OSError:
        root_storage = {"total_bytes": 0, "free_bytes": 0}
    hostname = socket.gethostname()
    machine = ""
    release = ""
    if hasattr(os, "uname"):
        uname = os.uname()
        machine, release = uname.machine, uname.release
    return {
        "hostname": hostname,
        "machine": machine,
        "kernel": release,
        "load_average": load,
        "uptime_seconds": round(uptime_seconds, 1),
        "memory_total_bytes": total_kb * 1024,
        "memory_available_bytes": available_kb * 1024,
        "root_storage": root_storage,
    }


def _path_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def validate_storage_directory(value: str, candidates: Iterable[dict] | None = None) -> Path:
    if not value.strip():
        raise ValueError("请选择 USB 存储设备")
    path = Path(value).expanduser().resolve()
    available = list(storage_candidates() if candidates is None else candidates)
    if sys.platform.startswith("linux"):
        roots = [Path(item["mountpoint"]).resolve() for item in available]
        if not any(_path_within(path, root) for root in roots):
            raise ValueError("录包目录必须位于当前已挂载的 USB/外部存储设备中")
    path.mkdir(parents=True, exist_ok=True)
    if not os.access(path, os.W_OK):
        raise ValueError("录包目录不可写")
    return path


class PcapRecorder:
    """Bounded asynchronous classic-PCAP writer with file rotation."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._queue: Queue[tuple[int, bytes] | None] = Queue(maxsize=8192)
        self._thread: threading.Thread | None = None
        self._file = None
        self._directory: Path | None = None
        self._prefix = "lidar"
        self._split_bytes = 1024 * 1024 * 1024
        self._min_free_bytes = 256 * 1024 * 1024
        self._part = 0
        self._current_bytes = 0
        self._started_ns = 0
        self._active = False
        self._error = ""
        self._current_file = ""
        self._files: deque[str] = deque(maxlen=32)
        self._packets = 0
        self._bytes = 0
        self._dropped = 0

    def start(self, directory: str, prefix: str, split_mb: int, min_free_mb: int = 256) -> dict:
        self.stop()
        target = validate_storage_directory(directory)
        cleaned = _SAFE_PREFIX.sub("-", prefix.strip()).strip(".-")[:64] or "lidar"
        split_mb = int(split_mb)
        min_free_mb = int(min_free_mb)
        if split_mb < 64 or split_mb > 16384:
            raise ValueError("单个 PCAP 大小必须在 64 到 16384 MB 之间")
        if min_free_mb < 64:
            raise ValueError("USB 最低保留空间不能小于 64 MB")
        with self._lock:
            self._queue = Queue(maxsize=8192)
            self._directory = target
            self._prefix = cleaned
            self._split_bytes = split_mb * 1024 * 1024
            self._min_free_bytes = min_free_mb * 1024 * 1024
            self._part = 0
            self._current_bytes = 0
            self._started_ns = time.time_ns()
            self._active = True
            self._error = ""
            self._current_file = ""
            self._files.clear()
            self._packets = self._bytes = self._dropped = 0
            queue = self._queue
            self._thread = threading.Thread(
                target=self._writer_loop, args=(queue,), name="pcap-usb-writer", daemon=True
            )
            self._thread.start()
        return self.status()

    @property
    def active(self) -> bool:
        return self._active

    def enqueue(self, timestamp_ns: int, frame: bytes) -> None:
        if not self._active or not frame:
            return
        try:
            self._queue.put_nowait((timestamp_ns, frame))
        except Full:
            self._dropped += 1

    def _open_part(self) -> None:
        assert self._directory is not None
        if self._file is not None:
            self._file.flush()
            os.fsync(self._file.fileno())
            self._file.close()
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(self._started_ns / 1_000_000_000))
        while True:
            self._part += 1
            filename = f"{self._prefix}-{stamp}-part{self._part:03d}.pcap"
            path = self._directory / filename
            if not path.exists():
                break
        self._file = path.open("xb")
        self._file.write(PCAP_GLOBAL_HEADER.pack(PCAP_MAGIC, 2, 4, 0, 0, 65535, ETHERNET_LINKTYPE))
        self._current_bytes = PCAP_GLOBAL_HEADER.size
        self._current_file = str(path)
        self._files.append(str(path))

    def _writer_loop(self, queue: Queue[tuple[int, bytes] | None]) -> None:
        try:
            self._open_part()
            checks = 0
            while True:
                try:
                    item = queue.get(timeout=0.5)
                except Empty:
                    if not self._active:
                        break
                    continue
                if item is None:
                    break
                timestamp_ns, frame = item
                record_size = PCAP_RECORD_HEADER.size + len(frame)
                if self._current_bytes + record_size > self._split_bytes:
                    self._open_part()
                seconds, nanoseconds = divmod(max(0, timestamp_ns), 1_000_000_000)
                microseconds = nanoseconds // 1000
                self._file.write(PCAP_RECORD_HEADER.pack(seconds, microseconds, len(frame), len(frame)))
                self._file.write(frame)
                self._current_bytes += record_size
                self._packets += 1
                self._bytes += len(frame)
                checks += 1
                if checks >= 256:
                    checks = 0
                    assert self._directory is not None
                    if shutil.disk_usage(self._directory).free < self._min_free_bytes:
                        raise OSError("USB 可用空间低于保留值，录包已自动停止")
        except (OSError, ValueError) as exc:
            self._error = str(exc)
        finally:
            if self._file is not None:
                try:
                    self._file.flush()
                    os.fsync(self._file.fileno())
                    self._file.close()
                except OSError as exc:
                    self._error = self._error or str(exc)
                self._file = None
            self._active = False

    def stop(self) -> dict:
        thread = self._thread
        if thread is not None:
            self._active = False
            try:
                self._queue.put_nowait(None)
            except Full:
                # Make room for the stop marker without blocking shutdown.
                try:
                    self._queue.get_nowait()
                except Empty:
                    pass
                try:
                    self._queue.put_nowait(None)
                except Full:
                    pass
            thread.join(timeout=8)
            self._thread = None
        return self.status()

    def status(self) -> dict:
        directory = str(self._directory) if self._directory else ""
        free_bytes = 0
        if directory:
            try:
                free_bytes = shutil.disk_usage(directory).free
            except OSError:
                pass
        return {
            "active": self._active,
            "state": "recording" if self._active else "error" if self._error else "idle",
            "error": self._error,
            "directory": directory,
            "current_file": self._current_file,
            "files": list(self._files),
            "packets": self._packets,
            "bytes": self._bytes,
            "dropped_packets": self._dropped,
            "queued_packets": self._queue.qsize(),
            "free_bytes": free_bytes,
            "split_bytes": self._split_bytes,
        }


def _ipv4_checksum(header: bytes) -> int:
    if len(header) % 2:
        header += b"\x00"
    total = sum(struct.unpack(f"!{len(header) // 2}H", header))
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def synthetic_ethernet_udp(source_ip: str, destination_ip: str, source_port: int,
                           destination_port: int, payload: bytes) -> bytes:
    """Build a replayable Ethernet/IP/UDP frame for socket-fallback capture."""
    source = socket.inet_aton(source_ip)
    destination = socket.inet_aton(destination_ip if destination_ip != "0.0.0.0" else "127.0.0.1")
    udp_length = 8 + len(payload)
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_length, 0, 0, 64, 17, 0, source, destination)
    checksum = _ipv4_checksum(header)
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_length, 0, 0, 64, 17, checksum, source, destination)
    udp = struct.pack("!HHHH", source_port, destination_port, udp_length, 0)
    return bytes.fromhex("0200000000020200000000010800") + header + udp + payload


class LidarGateway:
    """Persistent forwarding policy plus PCAP recorder telemetry."""

    def __init__(self, config_path: Path, local_addresses: Iterable[str] = ()) -> None:
        self.config_path = config_path
        self.local_addresses = set(local_addresses)
        self.recorder = PcapRecorder()
        self._lock = threading.Lock()
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setblocking(False)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._config = {
            "enabled": False,
            "target_ip": "",
            "target_port": 0,
            "source_ports": [],
        }
        self._forwarded_packets = 0
        self._forwarded_bytes = 0
        self._dropped_packets = 0
        self._last_error = ""
        self._last_forward_ns = 0
        self._load()

    def _load(self) -> None:
        try:
            value = json.loads(self.config_path.read_text(encoding="utf-8"))
            self.configure(value, persist=False)
        except (OSError, ValueError, json.JSONDecodeError):
            pass

    @staticmethod
    def validate(value: dict) -> dict:
        enabled = bool(value.get("enabled", False))
        target_ip = str(value.get("target_ip", "")).strip()
        if enabled and not target_ip:
            raise ValueError("启用转发前必须填写目标 IP")
        if target_ip:
            try:
                address = ipaddress.ip_address(target_ip)
            except ValueError as exc:
                raise ValueError("转发目标 IP 无效") from exc
            if address.version != 4 or address.is_unspecified:
                raise ValueError("转发目标必须是有效的 IPv4 地址")
        try:
            target_port = int(value.get("target_port", 0))
        except (TypeError, ValueError) as exc:
            raise ValueError("转发目标端口必须是整数") from exc
        if target_port < 0 or target_port > 65535:
            raise ValueError("转发目标端口必须是 0 到 65535；0 表示保持原目标端口")
        raw_ports = value.get("source_ports", [])
        if not isinstance(raw_ports, list):
            raise ValueError("源端口过滤必须是数组")
        try:
            ports = list(dict.fromkeys(int(item) for item in raw_ports))
        except (TypeError, ValueError) as exc:
            raise ValueError("源端口过滤必须是整数") from exc
        if any(item < 1 or item > 65535 for item in ports):
            raise ValueError("源端口过滤必须在 1 到 65535 之间")
        return {"enabled": enabled, "target_ip": target_ip, "target_port": target_port, "source_ports": ports}

    def configure(self, value: dict, persist: bool = True) -> dict:
        config = self.validate(value)
        with self._lock:
            self._config = config
            self._last_error = ""
        if persist:
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.config_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            temporary.replace(self.config_path)
        return self.status()

    def forward(self, payload: bytes, source_ip: str, source_port: int, destination_port: int) -> None:
        config = self._config
        if not config["enabled"] or source_ip in self.local_addresses:
            return
        if config["source_ports"] and destination_port not in config["source_ports"]:
            return
        target_port = config["target_port"] or destination_port
        try:
            sent = self._socket.sendto(payload, (config["target_ip"], target_port))
            self._forwarded_packets += 1
            self._forwarded_bytes += sent
            self._last_forward_ns = time.time_ns()
        except (BlockingIOError, OSError) as exc:
            self._dropped_packets += 1
            self._last_error = str(exc)

    def capture_frame(self, frame: bytes, timestamp_ns: int, metadata: dict | None = None) -> None:
        if metadata and metadata.get("source_ip") in self.local_addresses:
            return
        self.recorder.enqueue(timestamp_ns, frame)

    def capture_socket_udp(self, source_ip: str, destination_ip: str, source_port: int,
                           destination_port: int, payload: bytes, timestamp_ns: int) -> None:
        if self.recorder.active:
            self.recorder.enqueue(timestamp_ns, synthetic_ethernet_udp(
                source_ip, destination_ip, source_port, destination_port, payload
            ))

    def handle_socket_udp(self, source_ip: str, destination_ip: str, source_port: int,
                          destination_port: int, payload: bytes, timestamp_ns: int) -> None:
        self.forward(payload, source_ip, source_port, destination_port)
        self.capture_socket_udp(
            source_ip, destination_ip, source_port, destination_port, payload, timestamp_ns
        )

    def storage(self) -> list[dict]:
        return storage_candidates()

    def start_recording(self, value: dict) -> dict:
        return self.recorder.start(
            str(value.get("directory", "")),
            str(value.get("prefix", "lidar")),
            int(value.get("split_mb", 1024)),
            int(value.get("min_free_mb", 256)),
        )

    def status(self) -> dict:
        config = dict(self._config)
        return {
            "config": config,
            "forwarding": {
                "active": bool(config["enabled"] and config["target_ip"]),
                "packets": self._forwarded_packets,
                "bytes": self._forwarded_bytes,
                "dropped_packets": self._dropped_packets,
                "last_error": self._last_error,
                "last_forward_seconds": round((time.time_ns() - self._last_forward_ns) / 1e9, 2)
                if self._last_forward_ns else None,
            },
            "recording": self.recorder.status(),
        }

    def close(self) -> None:
        self.recorder.stop()
        self._socket.close()
