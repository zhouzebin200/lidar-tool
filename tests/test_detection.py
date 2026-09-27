import unittest
from pathlib import Path
import tempfile
import threading
import time

from ulv.detection import ROBOSENSE_DIFOP, detect_packet, normalize_lidar_payload
from ulv.registry import DeviceRegistry
from ulv.server import Application


class DetectionTests(unittest.TestCase):
    @staticmethod
    def velodyne_packet(product_id: int, banks: tuple[bytes, ...] = (b"\xff\xee",)) -> bytes:
        packet = bytearray(1206)
        for block in range(12):
            offset = block * 100
            packet[offset:offset + 2] = banks[block % len(banks)]
            packet[offset + 2:offset + 4] = (block * 100).to_bytes(2, "little")
        packet[1205] = product_id
        return bytes(packet)

    def test_hesai_protocol_detection(self):
        result = detect_packet(bytes.fromhex("ee ff 04 07") + bytes(100), 2368)
        self.assertIsNotNone(result)
        self.assertEqual(result.vendor, "Hesai")
        self.assertEqual(result.model, "ATX")
        self.assertGreater(result.confidence, 0.9)

    def test_huawei_l306_precedes_generic_hesai_port_fallback(self):
        for counter_prefix in (b"\x03\x02", b"\x03\x03", b"\x03\x04", b"\x7f\xa5"):
            packet = bytearray(1021)
            packet[4:6] = counter_prefix
            packet[53:55] = b"\xff\xdd"
            result = detect_packet(bytes(packet), 2368)
            self.assertIsNotNone(result)
            self.assertEqual(result.vendor, "Huawei")
            self.assertEqual(result.model, "L306")
            self.assertGreater(result.confidence, 0.99)

    def test_at128_short_status_packet_is_not_point_cloud(self):
        packet = bytes.fromhex("ee ff 04 03") + bytes(95)
        self.assertIsNone(detect_packet(packet, 2368))

    def test_seyond_header_identifies_model_on_arbitrary_port(self):
        packet = bytearray(70)
        packet[0:2] = (0x176A).to_bytes(2, "little")
        packet[2:4] = bytes((2, 1))
        packet[10:14] = len(packet).to_bytes(4, "little")
        packet[15] = 5
        result = detect_packet(bytes(packet), 45123)
        self.assertIsNotNone(result)
        self.assertEqual(result.vendor, "Seyond")
        self.assertEqual(result.model, "Robin-E1X / ELITE")
        self.assertGreater(result.confidence, 0.99)

    def test_seyond_header_rejects_wrong_declared_size(self):
        packet = bytearray(70)
        packet[0:2] = (0x176A).to_bytes(2, "little")
        packet[2:4] = bytes((2, 1))
        packet[10:14] = (999).to_bytes(4, "little")
        packet[15] = 0
        self.assertIsNone(detect_packet(bytes(packet), 45123))

    def test_robosense_m1_detection(self):
        packet = bytes.fromhex("55 aa 5a a5") + bytes(1210 - 4)
        result = detect_packet(packet, 6699)
        self.assertIsNotNone(result)
        self.assertEqual(result.vendor, "RoboSense")
        self.assertEqual(result.model, "RSM1")

    def test_robosense_m2_transport_envelope_is_removed(self):
        inner = bytes.fromhex("55 aa 5a a5") + bytes(1342 - 4)
        wrapped = bytes.fromhex("00 07 05 00") + len(inner).to_bytes(4, "big") + inner
        self.assertEqual(len(wrapped), 1350)
        self.assertEqual(normalize_lidar_payload(wrapped), inner)
        result = detect_packet(wrapped, 30097)
        self.assertEqual(result.vendor, "RoboSense")
        self.assertEqual(result.model, "RSM2")

    def test_robosense_emx_sequence_length_envelope_is_removed(self):
        inner = bytes.fromhex("55 aa 5a a5") + bytes(808)
        packet = (167).to_bytes(4, "big") + len(inner).to_bytes(4, "big") + inner
        self.assertEqual(len(packet), 820)
        self.assertEqual(normalize_lidar_payload(packet), inner)
        result = detect_packet(packet, 57000)
        self.assertIsNotNone(result)
        self.assertEqual(result.vendor, "RoboSense")
        self.assertEqual(result.model, "RSEMX")

    def test_robosense_emx_msop_and_periodic_difop_share_device(self):
        msop_inner = bytes.fromhex("55 aa 5a a5") + bytes(808)
        difop_inner = bytes.fromhex("a5 ff 00 5a") + bytes(736)
        msop = (167).to_bytes(4, "big") + len(msop_inner).to_bytes(4, "big") + msop_inner
        difop = (168).to_bytes(4, "big") + len(difop_inner).to_bytes(4, "big") + difop_inner
        self.assertEqual(normalize_lidar_payload(difop), difop_inner)
        msop_detection = detect_packet(msop, 57000)
        difop_detection = detect_packet(difop, 56999)
        self.assertEqual(difop_detection.model, "model pending MSOP")
        registry = DeviceRegistry()
        first = registry.observe(
            "10.206.0.12", 57000, 57000, 812, msop_detection,
            source_mac="5a:45:45:00:00:0c",
        )
        second = registry.observe(
            "10.206.0.12", 56999, 56999, 740, difop_detection,
            source_mac="5a:45:45:00:00:0c",
        )
        self.assertEqual(first.id, second.id)

    def test_robosense_m2_wrapped_difop_identifier_is_normalized(self):
        inner = ROBOSENSE_DIFOP[::-1] + bytes(256 - len(ROBOSENSE_DIFOP))
        wrapped = bytes.fromhex("00 07 05 01") + len(inner).to_bytes(4, "big") + inner
        normalized = normalize_lidar_payload(wrapped)
        self.assertEqual(len(normalized), 256)
        self.assertTrue(normalized.startswith(ROBOSENSE_DIFOP))

    def test_transport_envelope_with_wrong_length_is_not_removed(self):
        inner = bytes.fromhex("55 aa 5a a5") + bytes(32)
        wrapped = bytes.fromhex("00 07 05 00 00 00 05 3e") + inner
        self.assertIs(normalize_lidar_payload(wrapped), wrapped)

    def test_velodyne_vlp16_product_id_detection(self):
        result = detect_packet(self.velodyne_packet(0x22), 2368)
        self.assertIsNotNone(result)
        self.assertEqual(result.vendor, "Velodyne")
        self.assertEqual(result.model, "VLP-16")
        self.assertGreater(result.confidence, 0.99)

    def test_velodyne_vls128_four_bank_detection_without_product_id(self):
        result = detect_packet(
            self.velodyne_packet(0x00, (b"\xff\xee", b"\xff\xdd", b"\xff\xcc", b"\xff\xbb")),
            45000,
        )
        self.assertEqual(result.vendor, "Velodyne")
        self.assertEqual(result.model, "VLS-128")

    def test_velodyne_position_packet_detection(self):
        result = detect_packet(bytes(512), 8308)
        self.assertEqual(result.vendor, "Velodyne")
        self.assertEqual(result.model, "model pending data packet")

    def test_robosense_airy_lite_detection(self):
        packet = bytes.fromhex("5a ff 55 aa") + bytes(1280 - 4)
        result = detect_packet(packet, 42000)
        self.assertEqual(result.model, "RSAIRYLITE_ETH")

    def test_robosense_airy_detection_uses_lidar_type(self):
        packet = bytearray(1248)
        packet[:4] = bytes.fromhex("55 aa 05 5a")
        packet[31] = 0x31
        packet[32] = 0x02
        packet[42:44] = bytes.fromhex("ff ee")
        result = detect_packet(bytes(packet), 6699)
        self.assertEqual(result.model, "RSAIRY")
        self.assertEqual(result.confidence, 0.999)
        self.assertIn("96-channel", result.reason)
        self.assertGreater(result.confidence, 0.95)

    def test_ruby_plus_128_uses_block_layout_not_shared_magic(self):
        packet = bytearray(1248)
        packet[:4] = bytes.fromhex("55 aa 05 5a")
        packet[30] = 0x04
        packet[80] = packet[468] = packet[856] = 0xFE
        result = detect_packet(bytes(packet), 6699)
        self.assertEqual(result.model, "RSP128")
        self.assertGreater(result.confidence, 0.99)

    def test_ruby_80_uses_four_block_layout(self):
        packet = bytearray(1248)
        packet[:4] = bytes.fromhex("55 aa 05 5a")
        packet[30] = 0x03
        packet[80] = packet[324] = packet[568] = packet[812] = 0xFE
        result = detect_packet(bytes(packet), 6699)
        self.assertEqual(result.model, "RS80")

    def test_unknown_custom_port_is_ignored(self):
        self.assertIsNone(detect_packet(bytes(100), 40200))

    def test_registry_keeps_stable_device_id(self):
        registry = DeviceRegistry()
        detection = detect_packet(bytes.fromhex("ee ff 03 02") + bytes(80), 2368)
        first = registry.observe("192.168.1.201", 55000, 2368, 84, detection)
        second = registry.observe("192.168.1.201", 55000, 2368, 84, detection)
        self.assertEqual(first.id, second.id)
        self.assertEqual(second.packets, 2)

    def test_registry_public_list_contains_only_online_devices(self):
        registry = DeviceRegistry()
        detection = detect_packet(bytes.fromhex("ee ff 03 02") + bytes(80), 2368)
        device = registry.observe("192.168.1.201", 55000, 2368, 84, detection)
        self.assertEqual(len(registry.list_public()), 1)
        device.last_seen_ns -= 4_000_000_000
        self.assertEqual(registry.list_public(), [])

    def test_registry_tracks_same_mac_across_ip_subnets(self):
        registry = DeviceRegistry()
        detection = detect_packet(bytes.fromhex("ee ff 03 02") + bytes(80), 2368)
        first = registry.observe(
            "192.168.1.201", 10000, 2368, 84, detection,
            source_mac="ec:9f:0d:00:52:80",
        )
        second = registry.observe(
            "192.168.2.201", 10000, 2368, 84, detection,
            source_mac="ec:9f:0d:00:52:80",
        )
        self.assertEqual(first.id, second.id)
        self.assertEqual(second.ip, "192.168.2.201")
        self.assertEqual(len(registry.list_public()), 1)

    def test_registry_groups_robosense_msop_and_difop(self):
        registry = DeviceRegistry()
        msop = detect_packet(bytes.fromhex("55 aa 05 0a 5a a5 50 a0") + bytes(1240), 6699)
        difop = detect_packet(bytes.fromhex("a5 ff 00 5a 11 11 55 55") + bytes(1240), 7788)
        self.assertIsNotNone(msop)
        self.assertIsNotNone(difop)
        first = registry.observe("192.168.1.201", 6699, 6699, 1248, msop)
        second = registry.observe("192.168.1.201", 7788, 7788, 1248, difop)
        self.assertEqual(first.id, second.id)
        self.assertEqual(second.packets, 2)
        self.assertEqual(len(registry.list_public()), 1)

    def test_registry_accepts_sdk_confirmed_model(self):
        registry = DeviceRegistry()
        detection = detect_packet(bytes.fromhex("55 aa 05 0a 5a a5 50 a0") + bytes(1240), 6699)
        device = registry.observe("192.168.1.201", 6699, 6699, 1248, detection)
        registry.update_model(device.id, "RoboSense", "RS128")
        public = registry.list_public()[0]
        self.assertEqual(public["model"], "RS128")
        self.assertEqual(public["confidence"], 1.0)

    def test_config_validation_and_cidr_normalization(self):
        listen_ip, ports, networks = Application.validate_config({
            "listen_ip": "192.168.1.100",
            "udp_ports": [2368, 6699, 2368],
            "source_networks": ["192.168.1.201", "10.0.0.25/24"],
        })
        self.assertEqual(listen_ip, "192.168.1.100")
        self.assertEqual(ports, (2368, 6699))
        self.assertEqual(networks, ("192.168.1.201/32", "10.0.0.0/24"))

    def test_config_rejects_invalid_port(self):
        with self.assertRaises(ValueError):
            Application.validate_config({"listen_ip": "0.0.0.0", "udp_ports": [70000]})

    def test_automatic_network_mode_uses_all_interfaces_and_sdk_ports(self):
        resolved = Application.resolve_config({
            "auto_discovery": True,
            "listen_ip": "192.168.1.102",
            "udp_ports": [34567],
            "source_networks": [],
        })
        automatic, manual_ip, manual_ports, effective_ip, effective_ports, _ = resolved
        self.assertTrue(automatic)
        self.assertEqual(manual_ip, "192.168.1.102")
        self.assertEqual(manual_ports, (34567,))
        self.assertEqual(effective_ip, "0.0.0.0")
        self.assertEqual(effective_ports, (2368, 6699, 7788, 7766, 6688, 8010, 10110, 8308))

    def test_last_browser_disconnect_stops_service_after_grace_period(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = Application("127.0.0.1", (34567,), (), root / "runtime.json", root / "plugins")
            stopped = threading.Event()
            app.attach_shutdown(stopped.set)
            app.browser_connected()
            app.browser_disconnected(0.02)
            self.assertTrue(stopped.wait(0.5))
            app.stop()

    def test_browser_reconnect_cancels_idle_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = Application("127.0.0.1", (34567,), (), root / "runtime.json", root / "plugins")
            stopped = threading.Event()
            app.attach_shutdown(stopped.set)
            app.browser_connected()
            app.browser_disconnected(0.08)
            time.sleep(0.01)
            app.browser_connected()
            self.assertFalse(stopped.wait(0.12))
            app.stop()

    def test_start_does_not_block_ui_during_network_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = Application(
                "127.0.0.1", (34567,), (), root / "runtime.json", root / "plugins",
                auto_discovery=False, auto_arp_assist=False,
                shutdown_when_browser_closes=False,
            )
            recovery_finished = threading.Event()
            inputs_started = threading.Event()

            def slow_recovery():
                time.sleep(0.25)
                recovery_finished.set()
                return True, "restored"

            app.network_manager.recover_pending = slow_recovery
            app._start_inputs_locked = inputs_started.set
            app.ptp.start_if_enabled = lambda: None
            started = time.monotonic()
            try:
                app.start()
                self.assertLess(time.monotonic() - started, 0.1)
                self.assertFalse(inputs_started.is_set())
                self.assertTrue(recovery_finished.wait(1))
                self.assertTrue(inputs_started.wait(1))
            finally:
                app.stop()


if __name__ == "__main__":
    unittest.main()
