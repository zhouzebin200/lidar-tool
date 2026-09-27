import ctypes
import math
from pathlib import Path
import threading
import time
import unittest

from ulv.native_plugins import CPoint, NativePluginManager
from ulv.protocol import HEADER, decode_header
from ulv.registry import Device


ROOT = Path(__file__).resolve().parents[1]
PLUGIN_FILES = tuple(
    path
    for extension in ("*.dll", "*.so", "*.dylib")
    for path in (ROOT / "plugins").glob(extension)
)


def velodyne_packet(start_azimuth: int, product_id: int, banks: tuple[bytes, ...]) -> bytes:
    raw = bytearray(1206)
    for block in range(12):
        offset = block * 100
        raw[offset:offset + 2] = banks[block % len(banks)]
        azimuth = (start_azimuth + (block // len(banks)) * 20) % 36000
        raw[offset + 2:offset + 4] = azimuth.to_bytes(2, "little")
        for scan in range(32):
            sample = offset + 4 + scan * 3
            raw[sample:sample + 2] = (1000 + scan).to_bytes(2, "little")
            raw[sample + 2] = 128
    raw[1204] = 0x37
    raw[1205] = product_id
    return bytes(raw)


def huawei_l306_packet(azimuth_degrees: int, lut_index: int = 0, counter_byte: int = 4) -> bytes:
    raw = bytearray(1021)
    raw[4:6] = bytes((3, counter_byte))
    raw[13] = 1
    raw[23] = lut_index
    raw[25:27] = (0).to_bytes(2, "big")
    raw[53:55] = b"\xff\xdd"
    raw[55:57] = (azimuth_degrees * 256).to_bytes(2, "big")
    for ring in range(192):
        offset = 57 + ring * 5
        raw[offset:offset + 2] = (1000).to_bytes(2, "big")
        raw[offset + 4] = 77
    return bytes(raw)


@unittest.skipUnless(PLUGIN_FILES, "native plugins are not built on this machine")
class NativePluginTests(unittest.TestCase):
    def test_vendor_plugins_load_through_stable_abi(self) -> None:
        manager = NativePluginManager(ROOT / "plugins", lambda _frame: None)
        try:
            self.assertEqual(manager.errors, [])
            vendors = {plugin.vendor for plugin in manager.plugins}
            self.assertIn("Hesai", vendors)
            self.assertIn("Huawei", vendors)
            self.assertIn("RoboSense", vendors)
            self.assertIn("Velodyne", vendors)
            self.assertIn("Seyond", vendors)
        finally:
            manager.close()

    def test_huawei_l306_uses_demo_packet_layout_scale_and_calibration(self) -> None:
        captured: list[bytes] = []
        received = threading.Event()

        def emit(frame: bytes) -> None:
            captured.append(frame)
            received.set()

        manager = NativePluginManager(ROOT / "plugins", emit)
        device = Device(
            id=306, key="huawei:l306", vendor="Huawei", model="L306",
            ip="192.168.1.201", source_mac="", source_port=58005,
            destination_port=2368, confidence=1.0, reason="test", packets=19,
            bytes=19399, first_seen_ns=1, last_seen_ns=2,
        )
        try:
            for _ in range(18):
                manager.feed(device, huawei_l306_packet(100), time.time_ns(), 58005, 2368)
            manager.feed(device, huawei_l306_packet(10), time.time_ns(), 58005, 2368)
            self.assertTrue(received.wait(4))
            _, _, point_count, _ = decode_header(captured[0])
            self.assertEqual(point_count, 18 * 192)
            points = (CPoint * point_count).from_buffer_copy(captured[0][HEADER.size:])
            first = points[0]
            self.assertAlmostEqual(math.sqrt(first.x**2 + first.y**2 + first.z**2), 4.0, places=4)
            self.assertAlmostEqual(first.intensity, 77.0, places=4)
            self.assertEqual(first.ring, 0)
            elevation = math.degrees(math.atan2(first.z, math.hypot(first.x, first.y)))
            self.assertAlmostEqual(elevation, 7.62246875, places=4)
        finally:
            manager.close()

    def test_velodyne_vlp16_decodes_complete_revolution(self) -> None:
        captured: list[bytes] = []
        received = threading.Event()

        def emit(frame: bytes) -> None:
            captured.append(frame)
            received.set()

        manager = NativePluginManager(
            ROOT / "plugins",
            emit,
            default_correction_directory=ROOT / "corrections" / "hesai",
        )
        device = Device(
            id=73, key="velodyne:test", vendor="Velodyne", model="VLP-16",
            ip="192.168.1.201", source_mac="", source_port=2368,
            destination_port=2368, confidence=0.999, reason="test", packets=2,
            bytes=2412, first_seen_ns=1, last_seen_ns=2,
        )

        try:
            manager.feed(device, velodyne_packet(35000, 0x22, (b"\xff\xee",)), time.time_ns(), 2368, 2368)
            manager.feed(device, velodyne_packet(100, 0x22, (b"\xff\xee",)), time.time_ns(), 2368, 2368)
            self.assertTrue(received.wait(4))
            self.assertEqual(decode_header(captured[0])[:3], (73, 0, 384))
            plugin = next(item for item in manager.plugins if item.vendor == "Velodyne")
            self.assertEqual(plugin.last_model, "VLP-16")
            self.assertEqual(plugin.last_ring_range, [0, 15])
            self.assertGreater(plugin.last_bounds["max"][2] - plugin.last_bounds["min"][2], 0.5)
        finally:
            manager.close()

    def test_all_bundled_velodyne_models_load_calibration_and_decode(self) -> None:
        captured: dict[int, bytes] = {}
        received = threading.Event()

        models = (
            (101, "VLP-16", 0x22, (b"\xff\xee",), 16),
            (102, "VLP-16 Hi-Res", 0x24, (b"\xff\xee",), 16),
            (103, "VLP-32C", 0x28, (b"\xff\xee",), 32),
            (104, "HDL-32E", 0x21, (b"\xff\xee",), 32),
            (105, "HDL-64E S3", 0x40, (b"\xff\xee", b"\xff\xdd"), 64),
            (106, "VLS-128", 0xA1, (b"\xff\xee", b"\xff\xdd", b"\xff\xcc", b"\xff\xbb"), 128),
        )

        def emit(frame: bytes) -> None:
            device_id, _, _, _ = decode_header(frame)
            captured[device_id] = frame
            if len(captured) == len(models):
                received.set()

        manager = NativePluginManager(
            ROOT / "plugins", emit,
            default_correction_directory=ROOT / "corrections" / "hesai",
        )
        try:
            for device_id, model, product_id, banks, _ in models:
                device = Device(
                    id=device_id, key=f"velodyne:{device_id}", vendor="Velodyne", model=model,
                    ip=f"192.168.1.{device_id}", source_mac="", source_port=2368,
                    destination_port=2368, confidence=0.999, reason="test", packets=2,
                    bytes=2412, first_seen_ns=1, last_seen_ns=2,
                )
                manager.feed(device, velodyne_packet(35000, product_id, banks), time.time_ns(), 2368, 2368)
                manager.feed(device, velodyne_packet(100, product_id, banks), time.time_ns(), 2368, 2368)
            self.assertTrue(received.wait(6), f"missing frames: {set(m[0] for m in models) - set(captured)}")
            for device_id, _, _, _, rings in models:
                frame = captured[device_id]
                _, _, point_count, _ = decode_header(frame)
                self.assertEqual(point_count, 384)
                points = (CPoint * point_count).from_buffer_copy(frame[HEADER.size:])
                self.assertEqual(min(point.ring for point in points), 0)
                self.assertEqual(max(point.ring for point in points), rings - 1)
        finally:
            manager.close()

    def test_native_frame_worker_serializes_points(self) -> None:
        captured: list[bytes] = []
        received = threading.Event()

        def emit(frame: bytes) -> None:
            captured.append(frame)
            received.set()

        manager = NativePluginManager(ROOT / "plugins", emit)
        try:
            plugin = next(item for item in manager.plugins if item.vendor == "Hesai")
            source = (CPoint * 2)(
                CPoint(1.0, 2.0, 3.0, 0.5, 1, 2, 0),
                CPoint(-1.0, -2.0, -3.0, 0.8, 2, 1, 0),
            )
            raw = ctypes.string_at(source, ctypes.sizeof(source))
            plugin._frame_queue.put((7, 9, 123, 2, raw))
            self.assertTrue(received.wait(2))
            self.assertEqual(decode_header(captured[0]), (7, 9, 2, 123))
            self.assertEqual(plugin.last_nonzero_points, 2)
            self.assertEqual(plugin.last_ring_range, [1, 2])
        finally:
            manager.close()

    def test_hesai_pandarqt_fallback_correction_resolved(self) -> None:
        manager = NativePluginManager(
            ROOT / "plugins",
            lambda _frame: None,
            default_correction_directory=ROOT / "corrections" / "hesai",
        )
        try:
            device = Device(
                id=88, key="hesai:test_qt", vendor="Hesai", model="PandarQT",
                ip="192.168.1.201", source_mac="", source_port=10000,
                destination_port=2368, confidence=1.0, reason="test", packets=1,
                bytes=1072, first_seen_ns=1, last_seen_ns=2,
            )
            plugin = next(item for item in manager.plugins if item.vendor == "Hesai")
            instance = plugin._instance_for(88, "PandarQT", "192.168.1.201", "hesai:test_qt")
            self.assertIsNotNone(instance)
        finally:
            manager.close()


if __name__ == "__main__":
    unittest.main()
