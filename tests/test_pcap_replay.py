from pathlib import Path
import socket
import struct
import tempfile
import time
import unittest

from ulv.pcap_replay import PcapReplay, inspect_pcap
from ulv.registry import DeviceRegistry


def ethernet_udp(payload: bytes) -> bytes:
    ethernet = bytes.fromhex("00112233445566778899aabb0800")
    udp_length = 8 + len(payload)
    ip_header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45, 0, 20 + udp_length, 1, 0, 64, 17, 0,
        socket.inet_aton("172.16.4.31"),
        socket.inet_aton("225.0.4.21"),
    )
    udp_header = struct.pack("!HHHH", 10000, 55889, udp_length, 0)
    return ethernet + ip_header + udp_header + payload


def write_pcap(path: Path, frames: list[tuple[int, bytes]]) -> None:
    content = bytearray(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
    for timestamp_us, frame in frames:
        content += struct.pack("<IIII", timestamp_us // 1_000_000, timestamp_us % 1_000_000, len(frame), len(frame))
        content += frame
    path.write_bytes(content)


class PcapReplayTests(unittest.TestCase):
    def test_inspects_and_replays_lidar_packet(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.pcap"
            payload = bytes.fromhex("ee ff 04 03") + bytes(1114)
            frame = ethernet_udp(payload)
            write_pcap(path, [(1_000_000, frame), (1_001_000, frame)])
            info = inspect_pcap(path)
            self.assertEqual(info.records, 2)
            self.assertEqual(info.duration_ns, 1_000_000)

            registry = DeviceRegistry()
            received: list[bytes] = []
            replay = PcapReplay(registry, lambda _device, data, *_args: received.append(data))
            replay.start(path, "test.pcap", loop=False)
            deadline = time.monotonic() + 1
            while replay.status()["active"] and time.monotonic() < deadline:
                time.sleep(0.005)
            status = replay.status()
            self.assertEqual(status["state"], "completed")
            self.assertEqual(status["accepted_packets"], 2)
            self.assertEqual(len(received), 2)
            self.assertEqual(registry.list_public()[0]["model"], "AT128")
            replay.stop()

    def test_rejects_pcapng(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.pcapng"
            path.write_bytes(bytes.fromhex("0a0d0d0a") + bytes(28))
            with self.assertRaisesRegex(ValueError, "经典 Ethernet PCAP"):
                inspect_pcap(path)


if __name__ == "__main__":
    unittest.main()
