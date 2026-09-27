"""Thread-safe registry of observed LiDAR UDP flows."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import threading
import time

from .detection import Detection


@dataclass(slots=True)
class Device:
    id: int
    key: str
    vendor: str
    model: str
    ip: str
    source_mac: str
    source_port: int
    destination_port: int
    confidence: float
    reason: str
    packets: int
    bytes: int
    first_seen_ns: int
    last_seen_ns: int
    state: str = "online"

    def public(self, now_ns: int) -> dict:
        result = asdict(self)
        age_seconds = max(0.0, (now_ns - self.last_seen_ns) / 1_000_000_000)
        result["last_seen_seconds"] = round(age_seconds, 2)
        result["state"] = "online" if age_seconds < 3 else "idle" if age_seconds < 15 else "offline"
        result.pop("key", None)
        result.pop("first_seen_ns", None)
        result.pop("last_seen_ns", None)
        return result


class DeviceRegistry:
    def __init__(self) -> None:
        self._devices: dict[str, Device] = {}
        self._lock = threading.Lock()

    @staticmethod
    def stable_id(key: str) -> int:
        return int.from_bytes(hashlib.blake2s(key.encode(), digest_size=4).digest(), "little")

    def observe(
        self,
        source_ip: str,
        source_port: int,
        destination_port: int,
        packet_size: int,
        detection: Detection,
        source_mac: str = "",
    ) -> Device:
        # One physical lidar may emit MSOP, DIFOP, IMU and status traffic on
        # different UDP ports. Group those flows so a vendor SDK instance sees
        # all packets required for calibration and frame decoding.
        identity = source_mac.lower() if source_mac else source_ip
        key = f"{detection.vendor.lower()}:{identity}"
        now = time.time_ns()
        with self._lock:
            device = self._devices.get(key)
            if device is None:
                device = Device(
                    id=self.stable_id(key),
                    key=key,
                    vendor=detection.vendor,
                    model=detection.model,
                    ip=source_ip,
                    source_mac=source_mac.lower(),
                    source_port=source_port,
                    destination_port=destination_port,
                    confidence=detection.confidence,
                    reason=detection.reason,
                    packets=0,
                    bytes=0,
                    first_seen_ns=now,
                    last_seen_ns=now,
                )
                self._devices[key] = device
            else:
                # A hardware MAC remains stable when the lidar is moved to a
                # different IPv4 subnet. Keep its decoder/cache identity but
                # immediately use the newly observed address for PTC access.
                device.ip = source_ip
                if source_mac:
                    device.source_mac = source_mac.lower()
                if detection.confidence > device.confidence:
                    device.vendor = detection.vendor
                    device.model = detection.model
                    device.confidence = detection.confidence
                    device.reason = detection.reason
            device.packets += 1
            device.bytes += packet_size
            device.last_seen_ns = now
            return device

    def accumulate(self, device: Device, packet_count: int, byte_count: int, seen_ns: int) -> None:
        """Apply cached hot-path counters without repeating device discovery."""
        if packet_count <= 0:
            return
        with self._lock:
            device.packets += packet_count
            device.bytes += byte_count
            device.last_seen_ns = max(device.last_seen_ns, seen_ns)

    def list_public(self) -> list[dict]:
        now = time.time_ns()
        with self._lock:
            # The UI is an operational view, not a discovery history. Remove
            # a lidar from the public device list as soon as it has missed the
            # three-second online window; keep the internal object so its
            # stable decoder/calibration identity can be reused on reconnect.
            public = [device.public(now) for device in self._devices.values()]
            return [device for device in public if device["state"] == "online"]

    def update_model(self, device_id: int, vendor: str, model: str) -> None:
        """Replace a packet-family guess with the SDK's proven decoder model."""
        if not model:
            return
        with self._lock:
            for device in self._devices.values():
                if device.id == device_id:
                    device.vendor = vendor or device.vendor
                    device.model = model
                    device.confidence = 1.0
                    device.reason = "confirmed by vendor SDK point-cloud decoder"
                    return
