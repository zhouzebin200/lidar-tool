import socket
from pathlib import Path
import tempfile
import time
import unittest

from ulv.capture import parse_ethernet_udp
from ulv.gateway import LidarGateway, PcapRecorder, synthetic_ethernet_udp
from ulv.pcap_replay import inspect_pcap


class GatewayTests(unittest.TestCase):
    def test_synthetic_socket_packet_is_replayable_ethernet(self):
        payload = bytes.fromhex("eeff0403") + bytes(120)
        frame = synthetic_ethernet_udp("192.168.2.201", "192.168.2.100", 10000, 2368, payload)
        packet = parse_ethernet_udp(frame)
        self.assertIsNotNone(packet)
        self.assertEqual(packet.source_ip, "192.168.2.201")
        self.assertEqual(packet.destination_port, 2368)
        self.assertEqual(packet.payload, payload)

    def test_forward_preserves_original_destination_port_when_zero(self):
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(1)
        port = receiver.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            gateway = LidarGateway(Path(directory) / "gateway.json")
            gateway.configure({"enabled": True, "target_ip": "127.0.0.1", "target_port": port})
            gateway.forward(b"lidar", "192.168.1.201", 10000, 2368)
            self.assertEqual(receiver.recvfrom(64)[0], b"lidar")
            self.assertEqual(gateway.status()["forwarding"]["packets"], 1)
            gateway.close()
        receiver.close()

    def test_pcap_recorder_writes_classic_ethernet_file(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = PcapRecorder()
            recorder.start(directory, "AT128 test", 64, 64)
            frame = synthetic_ethernet_udp("10.0.0.201", "10.0.0.100", 10000, 2368, b"points")
            recorder.enqueue(time.time_ns(), frame)
            deadline = time.monotonic() + 1
            while recorder.status()["packets"] < 1 and time.monotonic() < deadline:
                time.sleep(0.01)
            status = recorder.stop()
            self.assertEqual(status["packets"], 1)
            path = Path(status["current_file"])
            self.assertTrue(path.is_file())
            self.assertEqual(inspect_pcap(path).records, 1)

    def test_gateway_rejects_invalid_target(self):
        with tempfile.TemporaryDirectory() as directory:
            gateway = LidarGateway(Path(directory) / "gateway.json")
            with self.assertRaisesRegex(ValueError, "目标 IP"):
                gateway.configure({"enabled": True, "target_ip": "not-an-ip", "target_port": 2368})
            gateway.close()


if __name__ == "__main__":
    unittest.main()
