import socket
import struct
import unittest
from dataclasses import replace

from ulv.capture import PacketCapture, build_arp_reply, parse_ethernet_arp, parse_ethernet_udp
from ulv.detection import detect_packet
from ulv.registry import DeviceRegistry


def ethernet_udp(payload: bytes, vlans: tuple[int, ...] = ()) -> bytes:
    ethernet = bytes.fromhex("00112233445566778899aabb")
    if vlans:
        ethernet += struct.pack("!H", 0x88A8 if len(vlans) > 1 else 0x8100)
        for index, vlan in enumerate(vlans):
            next_type = 0x8100 if index + 1 < len(vlans) else 0x0800
            ethernet += struct.pack("!HH", vlan, next_type)
    else:
        ethernet += struct.pack("!H", 0x0800)
    udp_length = 8 + len(payload)
    ip_header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45, 0, 20 + udp_length, 1, 0, 64, 17, 0,
        socket.inet_aton("192.168.1.201"),
        socket.inet_aton("192.168.1.102"),
    )
    udp_header = struct.pack("!HHHH", 10000, 34567, udp_length, 0)
    return ethernet + ip_header + udp_header + payload


def ethernet_arp(vlans: tuple[int, ...] = ()) -> bytes:
    requester = bytes.fromhex("66778899aabb")
    ethernet = bytes.fromhex("ffffffffffff") + requester
    if vlans:
        ethernet += struct.pack("!H", 0x88A8 if len(vlans) > 1 else 0x8100)
        for index, vlan in enumerate(vlans):
            next_type = 0x8100 if index + 1 < len(vlans) else 0x0806
            ethernet += struct.pack("!HH", vlan, next_type)
    else:
        ethernet += struct.pack("!H", 0x0806)
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1, 0x0800, 6, 4, 1,
        requester, socket.inet_aton("10.10.10.201"),
        bytes(6), socket.inet_aton("10.10.10.100"),
    )
    return ethernet + arp


class CaptureParserTests(unittest.TestCase):
    def test_extracts_arbitrary_udp_destination(self):
        packet = parse_ethernet_udp(ethernet_udp(b"lidar"))
        self.assertEqual(packet.source_mac, "66:77:88:99:aa:bb")
        self.assertEqual(packet.source_ip, "192.168.1.201")
        self.assertEqual(packet.destination_ip, "192.168.1.102")
        self.assertEqual(packet.source_port, 10000)
        self.assertEqual(packet.destination_port, 34567)
        self.assertEqual(packet.payload, b"lidar")
        self.assertEqual(packet.vlan_ids, ())

    def test_extracts_8021q_vlan(self):
        packet = parse_ethernet_udp(ethernet_udp(b"points", (321,)))
        self.assertEqual(packet.vlan_ids, (321,))
        self.assertEqual(packet.vlan_tags, ((0x8100, 321),))
        self.assertEqual(packet.destination_port, 34567)

    def test_extracts_qinq_vlan_stack(self):
        packet = parse_ethernet_udp(ethernet_udp(b"points", (100, 200)))
        self.assertEqual(packet.vlan_ids, (100, 200))

    def test_parses_arp_request_and_preserves_vlan_in_reply(self):
        request = parse_ethernet_arp(ethernet_arp((100, 200)))
        self.assertIsNotNone(request)
        self.assertEqual(request.source_mac, "66:77:88:99:aa:bb")
        self.assertEqual(request.sender_ip, "10.10.10.201")
        self.assertEqual(request.target_ip, "10.10.10.100")
        self.assertEqual(request.vlan_ids, (100, 200))

        reply = build_arp_reply(request, bytes.fromhex("021122334455"))
        self.assertGreaterEqual(len(reply), 60)
        parsed = parse_ethernet_arp(reply)
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed.operation, 2)
        self.assertEqual(parsed.source_mac, "02:11:22:33:44:55")
        self.assertEqual(parsed.destination_mac, "66:77:88:99:aa:bb")
        self.assertEqual(parsed.sender_ip, "10.10.10.100")
        self.assertEqual(parsed.target_ip, "10.10.10.201")
        self.assertEqual(parsed.vlan_ids, (100, 200))

    def test_arp_assist_accepts_only_cross_subnet_wired_request(self):
        request = parse_ethernet_arp(ethernet_arp())
        capture = PacketCapture(
            DeviceRegistry(),
            lambda *_args: None,
            arp_assist_enabled=True,
            local_addresses=("192.168.1.102",),
        )
        self.assertEqual(capture._arp_is_safe(request, "Realtek PCIe GbE Controller"), (True, "candidate"))
        self.assertEqual(
            capture._arp_is_safe(request, "Intel Wi-Fi Adapter"),
            (False, "non-wired-interface"),
        )

    def test_same_mac_new_source_ip_refreshes_cached_device(self):
        registry = DeviceRegistry()
        capture = PacketCapture(registry, lambda *_args: None)
        payload = bytes.fromhex("ee ff 03 02") + bytes(80)
        detection = detect_packet(payload, 2368)
        first_packet = parse_ethernet_udp(ethernet_udp(payload))
        first, cached = capture._device_for(first_packet, payload, detection)
        self.assertFalse(cached)
        self.assertTrue(capture._device_for(first_packet, payload, detection)[1])

        next_packet = replace(first_packet, source_ip="192.168.3.101")
        second, cached = capture._device_for(next_packet, payload, detection)
        self.assertFalse(cached)
        self.assertEqual(first.id, second.id)
        self.assertEqual(second.ip, "192.168.3.101")
        self.assertEqual(registry.list_public()[0]["ip"], "192.168.3.101")
        returned, cached = capture._device_for(first_packet, payload, detection)
        self.assertFalse(cached)
        self.assertEqual(returned.ip, "192.168.1.201")


if __name__ == "__main__":
    unittest.main()
