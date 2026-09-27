"""Classic Ethernet PCAP validation and real-time LiDAR replay."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct
import threading
import time
from typing import Callable, Iterator

from .capture import parse_ethernet_udp
from .detection import detect_packet, normalize_lidar_payload
from .registry import DeviceRegistry


PCAP_MAGICS = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1),
    b"\xa1\xb2\x3c\x4d": (">", 1),
}
MAX_CAPTURED_PACKET = 16 * 1024 * 1024


@dataclass(frozen=True, slots=True)
class PcapInfo:
    records: int
    first_timestamp_ns: int
    last_timestamp_ns: int

    @property
    def duration_ns(self) -> int:
        return max(0, self.last_timestamp_ns - self.first_timestamp_ns)


def pcap_packets(path: Path) -> Iterator[tuple[int, bytes, int]]:
    """Yield timestamp, Ethernet frame and one-based record index."""
    with path.open("rb") as source:
        header = source.read(24)
        if len(header) != 24 or header[:4] not in PCAP_MAGICS:
            raise ValueError("仅支持经典 Ethernet PCAP；当前文件不是受支持的 PCAP 格式")
        byte_order, fraction_ns = PCAP_MAGICS[header[:4]]
        link_type = struct.unpack_from(f"{byte_order}I", header, 20)[0]
        if link_type != 1:
            raise ValueError(f"PCAP 链路类型为 {link_type}，当前只支持 Ethernet 类型 1")
        record = struct.Struct(f"{byte_order}IIII")
        index = 0
        while True:
            raw_record = source.read(record.size)
            if not raw_record:
                return
            if len(raw_record) != record.size:
                raise ValueError("PCAP 记录头不完整")
            seconds, fraction, captured_size, _wire_size = record.unpack(raw_record)
            if captured_size > MAX_CAPTURED_PACKET:
                raise ValueError(f"PCAP 单包长度异常：{captured_size} 字节")
            frame = source.read(captured_size)
            if len(frame) != captured_size:
                raise ValueError("PCAP 数据包不完整")
            index += 1
            yield seconds * 1_000_000_000 + fraction * fraction_ns, frame, index


def inspect_pcap(path: Path) -> PcapInfo:
    records = 0
    first_timestamp_ns = last_timestamp_ns = 0
    for timestamp_ns, _frame, records in pcap_packets(path):
        if records == 1:
            first_timestamp_ns = timestamp_ns
        last_timestamp_ns = timestamp_ns
    if records == 0:
        raise ValueError("PCAP 中没有数据包")
    return PcapInfo(records, first_timestamp_ns, last_timestamp_ns)


class PcapReplay:
    def __init__(
        self,
        registry: DeviceRegistry,
        packet_callback: Callable[..., None],
    ) -> None:
        self.registry = registry
        self.packet_callback = packet_callback
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._path: Path | None = None
        self._delete_file = False
        self._name = ""
        self._loop = True
        self._info: PcapInfo | None = None
        self._state = "idle"
        self._error = ""
        self._record = 0
        self._accepted = 0
        self._loops = 0

    def start(self, path: Path, display_name: str, loop: bool = True, delete_file: bool = False) -> dict:
        info = inspect_pcap(path)
        self.stop()
        with self._lock:
            self._path = path
            self._delete_file = delete_file
            self._name = display_name or path.name
            self._loop = loop
            self._info = info
            self._state = "playing"
            self._error = ""
            self._record = 0
            self._accepted = 0
            self._loops = 0
            self._stop.clear()
            self._thread = threading.Thread(target=self._run, name="pcap-replay", daemon=True)
            self._thread.start()
        return self.status()

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                first_timestamp_ns: int | None = None
                loop_started_ns = time.monotonic_ns()
                for timestamp_ns, frame, record_index in pcap_packets(self._path):
                    if self._stop.is_set():
                        return
                    if first_timestamp_ns is None:
                        first_timestamp_ns = timestamp_ns
                    target_ns = loop_started_ns + max(0, timestamp_ns - first_timestamp_ns)
                    wait_seconds = (target_ns - time.monotonic_ns()) / 1_000_000_000
                    if wait_seconds > 0 and self._stop.wait(wait_seconds):
                        return
                    with self._lock:
                        self._record = record_index
                    packet = parse_ethernet_udp(frame)
                    if packet is None or not packet.payload:
                        continue
                    payload = normalize_lidar_payload(packet.payload)
                    detection = detect_packet(payload, packet.destination_port)
                    if detection is None:
                        continue
                    device = self.registry.observe(
                        packet.source_ip,
                        packet.source_port,
                        packet.destination_port,
                        len(payload),
                        detection,
                        source_mac=packet.source_mac,
                    )
                    self.packet_callback(
                        device,
                        payload,
                        timestamp_ns,
                        packet.source_port,
                        packet.destination_port,
                    )
                    with self._lock:
                        self._accepted += 1
                with self._lock:
                    self._loops += 1
                    repeat = self._loop
                if not repeat:
                    with self._lock:
                        self._state = "completed"
                    return
        except (OSError, ValueError) as exc:
            with self._lock:
                self._state = "error"
                self._error = str(exc)
        finally:
            if self._stop.is_set():
                with self._lock:
                    self._state = "stopped"

    def status(self) -> dict:
        with self._lock:
            info = self._info
            return {
                "state": self._state,
                "active": self._state == "playing",
                "name": self._name,
                "loop": self._loop,
                "record": self._record,
                "records": info.records if info else 0,
                "progress": round(self._record / info.records, 4) if info and info.records else 0,
                "duration_seconds": round(info.duration_ns / 1_000_000_000, 3) if info else 0,
                "accepted_packets": self._accepted,
                "completed_loops": self._loops,
                "error": self._error,
            }

    def stop(self) -> dict:
        self._stop.set()
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=3)
        with self._lock:
            path = self._path
            delete_file = self._delete_file
            self._thread = None
            self._path = None
            self._delete_file = False
            if self._state == "playing":
                self._state = "stopped"
        if delete_file and path is not None:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        return self.status()
