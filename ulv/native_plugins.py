"""ctypes host for optional C++ vendor decoder plugins."""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
from queue import Empty, Full, Queue
import sys
import threading
from typing import Callable

from .protocol import HEADER, MAGIC, POINT, VERSION
from .registry import Device


def default_calibration_cache_directory() -> Path:
    """Return a persistent, per-user cache outside a frozen app's temp dir."""
    if os.name == "nt":
        root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        return root / "UniversalLidarViewer" / "calibration"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Caches" / "UniversalLidarViewer" / "calibration"
    root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return root / "universal-lidar-viewer" / "calibration"


class CPoint(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("z", ctypes.c_float),
        ("intensity", ctypes.c_float),
        ("ring", ctypes.c_uint16),
        ("return_type", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
    ]


class CFrame(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("device_id", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("vendor", ctypes.c_char_p),
        ("model", ctypes.c_char_p),
        ("points", ctypes.POINTER(CPoint)),
        ("point_count", ctypes.c_size_t),
    ]


class CUdpPacket(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("source_ip", ctypes.c_char_p),
        ("source_port", ctypes.c_uint16),
        ("destination_port", ctypes.c_uint16),
        ("receive_timestamp_ns", ctypes.c_uint64),
        ("payload", ctypes.POINTER(ctypes.c_uint8)),
        ("payload_size", ctypes.c_size_t),
    ]


FrameCallback = ctypes.CFUNCTYPE(None, ctypes.POINTER(CFrame), ctypes.c_void_p)


class LoadedPlugin:
    def __init__(
        self,
        path: Path,
        emit: Callable[[bytes], None],
        identify: Callable[[int, str, str], None] | None = None,
        calibration_cache_directory: Path | None = None,
        correction_file: Path | None = None,
        default_correction_directory: Path | None = None,
    ) -> None:
        self.path = path
        self.library = ctypes.CDLL(str(path))
        self.library.ulv_plugin_abi_version.restype = ctypes.c_uint32
        if self.library.ulv_plugin_abi_version() != 2:
            raise RuntimeError("unsupported plugin ABI")
        self.library.ulv_plugin_manifest_json.restype = ctypes.c_char_p
        self.manifest = json.loads(self.library.ulv_plugin_manifest_json().decode("utf-8"))
        self.library.ulv_plugin_create.argtypes = [ctypes.c_char_p, FrameCallback, ctypes.c_void_p]
        self.library.ulv_plugin_create.restype = ctypes.c_void_p
        self.library.ulv_plugin_feed_udp.argtypes = [ctypes.c_void_p, ctypes.POINTER(CUdpPacket)]
        self.library.ulv_plugin_feed_udp.restype = ctypes.c_int
        try:
            self._feed_batch = self.library.ulv_plugin_feed_udp_batch
            self._feed_batch.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(CUdpPacket),
                ctypes.c_size_t,
            ]
            self._feed_batch.restype = ctypes.c_int
        except AttributeError:
            self._feed_batch = None
        try:
            self._feed_payload_batch = self.library.ulv_plugin_feed_udp_payload_batch
            self._feed_payload_batch.argtypes = [
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_uint16,
                ctypes.c_uint16,
                ctypes.c_uint64,
                ctypes.POINTER(ctypes.c_uint8),
                ctypes.c_size_t,
                ctypes.c_size_t,
            ]
            self._feed_payload_batch.restype = ctypes.c_int
        except AttributeError:
            self._feed_payload_batch = None
        self.library.ulv_plugin_destroy.argtypes = [ctypes.c_void_p]
        try:
            self._instance_status = self.library.ulv_plugin_instance_status_json
            self._instance_status.argtypes = [ctypes.c_void_p]
            self._instance_status.restype = ctypes.c_char_p
        except AttributeError:
            self._instance_status = None
        self._emit = emit
        self._identify = identify
        self._callback = FrameCallback(self._on_frame)
        self._instances: dict[int, ctypes.c_void_p] = {}
        self._instance_ips: dict[int, str] = {}
        self._calibration_cache_directory = calibration_cache_directory
        self._correction_file = correction_file
        self._default_correction_directory = default_correction_directory
        self._lock = threading.Lock()
        self.feed_calls = 0
        self.frames = 0
        self.points = 0
        self.last_feed_result: int | None = None
        self.last_model = ""
        self.last_frame_timestamp_ns = 0
        self.last_nonzero_points = 0
        self.last_bounds: dict[str, list[float]] | None = None
        self.last_ring_range: list[int] | None = None
        self.dropped_frames = 0
        self.dropped_packets = 0
        self.decode_errors = 0
        self.last_decode_error = ""
        self._closing = threading.Event()
        self._packet_queue: Queue[
            tuple[int, str, str, str, str, int, int, bytes, int] | None
        ] = Queue(maxsize=65536)
        self._packet_worker = threading.Thread(
            target=self._process_packets,
            name=f"decoder-{self.vendor.lower()}",
            daemon=True,
        )
        self._frame_queue: Queue[tuple[int, int, int, int, bytes] | None] = Queue(maxsize=2)
        self._frame_worker = threading.Thread(target=self._process_frames, daemon=True)
        self._packet_worker.start()
        self._frame_worker.start()

    @property
    def vendor(self) -> str:
        return str(self.manifest.get("vendor", ""))

    def _on_frame(self, frame_pointer: ctypes.POINTER(CFrame), _user_data: int) -> None:
        frame = frame_pointer.contents
        self.frames += 1
        self.points = int(frame.point_count)
        self.last_model = frame.model.decode("utf-8", errors="replace") if frame.model else ""
        if self._identify is not None:
            self._identify(int(frame.device_id), self.vendor, self.last_model)
        self.last_frame_timestamp_ns = int(frame.timestamp_ns)
        point_bytes = ctypes.string_at(frame.points, ctypes.sizeof(CPoint) * frame.point_count)
        item = (
            int(frame.device_id),
            int(frame.sequence),
            int(frame.timestamp_ns),
            int(frame.point_count),
            point_bytes,
        )
        try:
            self._frame_queue.put_nowait(item)
        except Full:
            try:
                self._frame_queue.get_nowait()
            except Empty:
                pass
            self.dropped_frames += 1
            self._frame_queue.put_nowait(item)

    def _process_frames(self) -> None:
        while True:
            item = self._frame_queue.get()
            if item is None:
                return
            device_id, sequence, timestamp_ns, point_count, point_bytes = item
            point_array = (CPoint * point_count).from_buffer_copy(point_bytes)
            payload = bytearray(HEADER.size + POINT.size * point_count)
            HEADER.pack_into(
                payload,
                0,
                MAGIC,
                VERSION,
                0,
                device_id,
                sequence,
                point_count,
                timestamp_ns,
            )
            payload[HEADER.size:] = point_bytes
            minimum = [float("inf"), float("inf"), float("inf")]
            maximum = [float("-inf"), float("-inf"), float("-inf")]
            minimum_ring = 65535
            maximum_ring = 0
            sample_stride = max(1, point_count // 8192)
            for index in range(0, point_count, sample_stride):
                source = point_array[index]
                coordinates = (source.x, source.y, source.z)
                for axis, value in enumerate(coordinates):
                    minimum[axis] = min(minimum[axis], value)
                    maximum[axis] = max(maximum[axis], value)
                minimum_ring = min(minimum_ring, source.ring)
                maximum_ring = max(maximum_ring, source.ring)
            self.last_nonzero_points = point_count
            self.last_bounds = {"min": minimum, "max": maximum} if point_count else None
            self.last_ring_range = [minimum_ring, maximum_ring] if point_count else None
            self._emit(bytes(payload))

    def feed(
        self,
        device: Device,
        data: bytes,
        receive_timestamp_ns: int,
        source_port: int,
        destination_port: int,
    ) -> None:
        """Queue a datagram without blocking the UDP receive thread."""
        if self._closing.is_set():
            return
        item = (
            device.id,
            device.model,
            device.ip,
            device.vendor,
            device.source_mac or device.key,
            source_port,
            destination_port,
            data,
            receive_timestamp_ns,
        )
        try:
            self._packet_queue.put_nowait(item)
        except Full:
            try:
                self._packet_queue.get_nowait()
            except Empty:
                pass
            self.dropped_packets += 1
            try:
                self._packet_queue.put_nowait(item)
            except Full:
                self.dropped_packets += 1

    def _process_packets(self) -> None:
        while True:
            item = self._packet_queue.get()
            if item is None:
                return
            items = [item]
            stop_after_batch = False
            if self._feed_batch is not None:
                while len(items) < 512:
                    try:
                        following = self._packet_queue.get_nowait()
                    except Empty:
                        break
                    if following is None:
                        stop_after_batch = True
                        break
                    items.append(following)
            try:
                if self._feed_batch is None:
                    self._feed_now(*item[:3], item[4], *item[5:])
                else:
                    start = 0
                    while start < len(items):
                        device_id, _, device_ip = items[start][:3]
                        end = start + 1
                        while (end < len(items) and items[end][0] == device_id
                               and items[end][2] == device_ip):
                            end += 1
                        self._feed_batch_now(items[start:end])
                        start = end
            except (OSError, ValueError) as exc:
                self.decode_errors += 1
                self.last_decode_error = str(exc)
            if stop_after_batch:
                return

    def _instance_for(
        self,
        device_id: int,
        model: str,
        device_ip: str,
        device_identity: str,
    ) -> ctypes.c_void_p | None:
        """Return one decoder instance, recreating it after an IP change."""
        with self._lock:
            instance = self._instances.get(device_id)
            if instance is not None and self._instance_ips.get(device_id) != device_ip:
                self.library.ulv_plugin_destroy(instance)
                self._instances.pop(device_id, None)
                self._instance_ips.pop(device_id, None)
                instance = None
            if instance is not None:
                return instance
            config_values: dict[str, object] = {
                "device_id": device_id,
                "model": model,
                "device_ip": device_ip,
                "ptc_port": 9347,
            }
            if self._correction_file is not None:
                config_values["correction_file"] = self._correction_file.as_posix()
            if self.vendor.casefold() == "hesai" and self._default_correction_directory is not None:
                fallback_names = {
                    "AT128": "AT128E2X.dat",
                    "AT128E2X": "AT128E2X.dat",
                    "AT128E3X": "AT128E3X.dat",
                    "AT128P": "AT128E3X.dat",
                    "PandarQT": "PandarQT.csv",
                    "PandarQT64": "PandarQT.csv",
                    "QT64": "PandarQT.csv",
                }
                fallback_name = fallback_names.get(model)
                if fallback_name:
                    fallback = self._default_correction_directory / fallback_name
                    if fallback.is_file():
                        config_values["fallback_correction_file"] = fallback.as_posix()
            if self.vendor.casefold() == "velodyne" and self._default_correction_directory is not None:
                velodyne_directory = self._default_correction_directory.parent / "velodyne"
                if velodyne_directory.is_dir():
                    config_values["default_correction_directory"] = velodyne_directory.as_posix()
            if self.vendor.casefold() == "seyond" and self._default_correction_directory is not None:
                seyond_fallbacks = {
                    "Robin-W": "Robin-W.bin",
                }
                fallback_name = seyond_fallbacks.get(model)
                if fallback_name:
                    fallback = self._default_correction_directory.parent / "seyond" / fallback_name
                    if fallback.is_file():
                        config_values["fallback_anglehv_file"] = fallback.as_posix()
            if self._calibration_cache_directory is not None:
                digest = hashlib.blake2s(device_identity.encode(), digest_size=10).hexdigest()
                cache_file = self._calibration_cache_directory / f"{self.vendor.lower()}-{digest}.bin"
                # The native adapter's intentionally tiny JSON reader does
                # not unescape backslashes. Forward slashes work on Windows.
                config_values["correction_cache_file"] = cache_file.as_posix()
            config = json.dumps(config_values).encode("utf-8")
            instance = self.library.ulv_plugin_create(config, self._callback, None)
            if not instance:
                return None
            self._instances[device_id] = instance
            self._instance_ips[device_id] = device_ip
            return instance

    def _feed_now(
        self,
        device_id: int,
        model: str,
        device_ip: str,
        device_identity: str,
        source_port: int,
        destination_port: int,
        data: bytes,
        receive_timestamp_ns: int,
    ) -> None:
        instance = self._instance_for(device_id, model, device_ip, device_identity)
        if instance is None:
            return
        payload = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        source_ip = device_ip.encode("ascii")
        packet = CUdpPacket(
            ctypes.sizeof(CUdpPacket),
            source_ip,
            source_port,
            destination_port,
            receive_timestamp_ns,
            payload,
            len(data),
        )
        self.feed_calls += 1
        self.last_feed_result = self.library.ulv_plugin_feed_udp(instance, ctypes.byref(packet))

    def _feed_batch_now(self, items: list[tuple]) -> None:
        if not items:
            return
        device_id, model, device_ip, _vendor, device_identity = items[0][:5]
        instance = self._instance_for(device_id, model, device_ip, device_identity)
        if instance is None or (self._feed_batch is None and self._feed_payload_batch is None):
            return
        if self._feed_payload_batch is not None:
            start = 0
            while start < len(items):
                first = items[start]
                source_port, destination_port, first_data = first[5], first[6], first[7]
                end = start + 1
                while (end < len(items) and items[end][5] == source_port
                       and items[end][6] == destination_port
                       and len(items[end][7]) == len(first_data)):
                    end += 1
                blob = b"".join(item[7] for item in items[start:end])
                payloads = (ctypes.c_uint8 * len(blob)).from_buffer_copy(blob)
                self.feed_calls += end - start
                self.last_feed_result = self._feed_payload_batch(
                    instance,
                    device_ip.encode("ascii"),
                    source_port,
                    destination_port,
                    first[8],
                    payloads,
                    len(first_data),
                    end - start,
                )
                start = end
            return
        payload_buffers = []
        source_ips = []
        packets = (CUdpPacket * len(items))()
        for index, item in enumerate(items):
            (_device_id, _model, item_ip, _vendor, _identity, source_port,
             destination_port, data, receive_timestamp_ns) = item
            payload = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
            source_ip = item_ip.encode("ascii")
            payload_buffers.append(payload)
            source_ips.append(source_ip)
            packets[index] = CUdpPacket(
                ctypes.sizeof(CUdpPacket),
                source_ip,
                source_port,
                destination_port,
                receive_timestamp_ns,
                payload,
                len(data),
            )
        self.feed_calls += len(items)
        self.last_feed_result = self._feed_batch(instance, packets, len(items))

    def status(self) -> dict:
        instance_statuses = []
        if self._instance_status is not None:
            with self._lock:
                instances = tuple(self._instances.items())
                instance_ips = dict(self._instance_ips)
            for device_id, instance in instances:
                try:
                    raw_status = self._instance_status(instance)
                    decoder_status = json.loads(raw_status.decode("utf-8")) if raw_status else {}
                except (OSError, ValueError, UnicodeDecodeError):
                    decoder_status = {"state": "unknown", "label": "标定状态未知"}
                instance_statuses.append({
                    "device_id": device_id,
                    "device_ip": instance_ips.get(device_id, ""),
                    "calibration": decoder_status,
                })
        return {
            **self.manifest,
            "feed_calls": self.feed_calls,
            "frames": self.frames,
            "last_points": self.points,
            "last_model": self.last_model,
            "last_nonzero_points": self.last_nonzero_points,
            "last_bounds": self.last_bounds,
            "last_ring_range": self.last_ring_range,
            "dropped_frames": self.dropped_frames,
            "dropped_packets": self.dropped_packets,
            "decode_errors": self.decode_errors,
            "last_decode_error": self.last_decode_error,
            "packet_queue_depth": self._packet_queue.qsize(),
            "last_feed_result": self.last_feed_result,
            "last_frame_timestamp_ns": self.last_frame_timestamp_ns,
            "instances": instance_statuses,
        }

    def close(self) -> None:
        self._closing.set()
        while True:
            try:
                self._packet_queue.get_nowait()
            except Empty:
                break
        self._packet_queue.put_nowait(None)
        self._packet_worker.join(timeout=5)
        with self._lock:
            instances = tuple(self._instances.values())
            self._instances.clear()
            self._instance_ips.clear()
        for instance in instances:
            self.library.ulv_plugin_destroy(instance)
        while True:
            try:
                self._frame_queue.get_nowait()
            except Empty:
                break
        self._frame_queue.put_nowait(None)
        self._frame_worker.join(timeout=3)


class NativePluginManager:
    def __init__(
        self,
        directory: Path,
        emit: Callable[[bytes], None],
        identify: Callable[[int, str, str], None] | None = None,
        calibration_cache_directory: Path | None = None,
        correction_file: Path | None = None,
        default_correction_directory: Path | None = None,
        authorize: Callable[[], bool] | None = None,
    ) -> None:
        self.authorize = authorize
        original_emit = emit

        def authorized_emit(frame: bytes) -> None:
            if authorize is None or authorize():
                original_emit(frame)

        self.plugins: list[LoadedPlugin] = []
        self.errors: list[str] = []
        cache_directory = calibration_cache_directory or default_calibration_cache_directory()
        try:
            cache_directory.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            self.errors.append(f"calibration cache unavailable at {cache_directory}: {exc}")
            cache_directory = None
        patterns = ("*.dll", "*.so", "*.dylib")
        candidates = [path for pattern in patterns for path in directory.glob(pattern)] if directory.is_dir() else []
        if not candidates:
            self.errors.append(f"no decoder plugins found in {directory}")
        for path in candidates:
            try:
                self.plugins.append(LoadedPlugin(
                    path, authorized_emit, identify, cache_directory,
                    correction_file if "hesai" in path.name.casefold() else None,
                    default_correction_directory,
                ))
            except (OSError, RuntimeError, ValueError, AttributeError) as exc:
                self.errors.append(f"{path.name}: {exc}")

    def feed(
        self,
        device: Device,
        data: bytes,
        receive_timestamp_ns: int,
        source_port: int,
        destination_port: int,
    ) -> None:
        if self.authorize is not None and not self.authorize():
            return
        for plugin in self.plugins:
            if plugin.vendor.lower() == device.vendor.lower():
                plugin.feed(
                    device,
                    data,
                    receive_timestamp_ns,
                    source_port,
                    destination_port,
                )

    def status(self) -> dict:
        return {
            "loaded": [plugin.status() for plugin in self.plugins],
            "errors": list(self.errors),
        }

    def close(self) -> None:
        for plugin in self.plugins:
            plugin.close()
