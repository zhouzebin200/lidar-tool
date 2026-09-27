import socket
import struct
import unittest

from ulv.raw_ptc import build_arp_request, build_tcp_frame, internet_checksum, parse_ethernet_tcp


class RawPtcFrameTests(unittest.TestCase):
    def test_tcp_frame_round_trip_preserves_vlan_and_payload(self):
        frame = build_tcp_frame(
            source_mac=bytes.fromhex("021122334455"),
            destination_mac=bytes.fromhex("cacababe0206"),
            source_ip="172.16.4.100",
            destination_ip="172.16.4.31",
            source_port=52000,
            destination_port=9347,
            sequence=1234,
            acknowledgement=5678,
            flags=0x18,
            payload=b"Gt\x07\x00\x00\x00\x00\x00",
            vlan_tags=((0x8100, 4),),
            identification=42,
        )
        packet = parse_ethernet_tcp(frame)
        self.assertIsNotNone(packet)
        self.assertEqual(packet.source_ip, "172.16.4.100")
        self.assertEqual(packet.destination_ip, "172.16.4.31")
        self.assertEqual(packet.destination_port, 9347)
        self.assertEqual(packet.sequence, 1234)
        self.assertEqual(packet.acknowledgement, 5678)
        self.assertEqual(packet.vlan_tags, ((0x8100, 4),))
        self.assertEqual(packet.payload, b"Gt\x07\x00\x00\x00\x00\x00")

        ethernet_length = 18
        ip_header = frame[ethernet_length:ethernet_length + 20]
        self.assertEqual(internet_checksum(ip_header), 0)
        tcp = frame[ethernet_length + 20:]
        pseudo = socket.inet_aton("172.16.4.100") + socket.inet_aton("172.16.4.31")
        pseudo += struct.pack("!BBH", 0, 6, len(tcp))
        self.assertEqual(internet_checksum(pseudo + tcp), 0)

    def test_arp_request_uses_same_vlan(self):
        frame = build_arp_request(
            bytes.fromhex("021122334455"), "172.16.4.100", "172.16.4.31",
            ((0x88A8, 100), (0x8100, 4)),
        )
        self.assertGreaterEqual(len(frame), 60)
        self.assertEqual(struct.unpack_from("!H", frame, 12)[0], 0x88A8)
        self.assertEqual(struct.unpack_from("!H", frame, 16)[0], 0x8100)
        self.assertEqual(struct.unpack_from("!H", frame, 20)[0], 0x0806)


if __name__ == "__main__":
    unittest.main()
