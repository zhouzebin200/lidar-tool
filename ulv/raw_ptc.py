"""Read-only PTC transport over captured Ethernet/VLAN frames.

This is a narrow TCP client for a directly attached LiDAR.  It is used only
after a live point-cloud flow has supplied the exact interface, source MAC,
source IPv4 address and VLAN stack.  It does not alter host addresses, routes,
adapter VLAN settings or firewall state.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
import hashlib
import ipaddress
import random
import socket
import struct
import time

from .capture import PcapLibrary, PcapPacketHeader, _ethernet_header, interface_mac


TCP_FIN = 0x01
TCP_SYN = 0x02
TCP_RST = 0x04
TCP_PSH = 0x08
TCP_ACK = 0x10


def internet_checksum(value: bytes) -> int:
    """Return the RFC 1071 one's-complement checksum."""
    if len(value) & 1:
        value += b"\x00"
    words = struct.unpack(f"!{len(value) // 2}H", value)
    total = sum(words)
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def _mac_bytes(value: str) -> bytes:
    raw = bytes.fromhex(value.replace(":", "").replace("-", ""))
    if len(raw) != 6:
        raise ValueError("无效的雷达 MAC 地址")
    return raw


def _vlan_ethernet(destination: bytes, source: bytes, vlan_tags: tuple[tuple[int, int], ...], ether_type: int) -> bytes:
    header = destination + source
    if not vlan_tags:
        return header + struct.pack("!H", ether_type)
    for tpid, tci in vlan_tags:
        header += struct.pack("!HH", tpid, tci)
    return header + struct.pack("!H", ether_type)


def build_arp_request(
    source_mac: bytes,
    source_ip: str,
    target_ip: str,
    vlan_tags: tuple[tuple[int, int], ...],
) -> bytes:
    ethernet = _vlan_ethernet(b"\xff" * 6, source_mac, vlan_tags, 0x0806)
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1, 0x0800, 6, 4, 1,
        source_mac, socket.inet_aton(source_ip), bytes(6), socket.inet_aton(target_ip),
    )
    return (ethernet + arp).ljust(60, b"\x00")


def build_tcp_frame(
    *, source_mac: bytes, destination_mac: bytes,
    source_ip: str, destination_ip: str,
    source_port: int, destination_port: int,
    sequence: int, acknowledgement: int, flags: int,
    payload: bytes = b"", vlan_tags: tuple[tuple[int, int], ...] = (),
    identification: int = 0, options: bytes = b"",
) -> bytes:
    source_ip_raw = socket.inet_aton(source_ip)
    destination_ip_raw = socket.inet_aton(destination_ip)
    if len(options) % 4 or len(options) > 40:
        raise ValueError("TCP options must be 32-bit aligned and at most 40 bytes")
    tcp = struct.pack(
        "!HHIIBBHHH",
        source_port, destination_port, sequence & 0xFFFFFFFF, acknowledgement & 0xFFFFFFFF,
        (5 + len(options) // 4) << 4, flags, 64240, 0, 0,
    ) + options + payload
    pseudo = source_ip_raw + destination_ip_raw + struct.pack("!BBH", 0, 6, len(tcp))
    checksum = internet_checksum(pseudo + tcp)
    tcp = tcp[:16] + struct.pack("!H", checksum) + tcp[18:]
    total_length = 20 + len(tcp)
    ip = struct.pack(
        "!BBHHHBBH4s4s",
        0x45, 0, total_length, identification & 0xFFFF, 0x4000, 64, 6, 0,
        source_ip_raw, destination_ip_raw,
    )
    ip = ip[:10] + struct.pack("!H", internet_checksum(ip)) + ip[12:]
    return _vlan_ethernet(destination_mac, source_mac, vlan_tags, 0x0800) + ip + tcp


@dataclass(frozen=True, slots=True)
class CapturedTcp:
    source_mac: str
    destination_mac: str
    source_ip: str
    destination_ip: str
    source_port: int
    destination_port: int
    sequence: int
    acknowledgement: int
    flags: int
    payload: bytes
    vlan_tags: tuple[tuple[int, int], ...]


def parse_ethernet_tcp(frame: bytes) -> CapturedTcp | None:
    header = _ethernet_header(frame)
    if header is None:
        return None
    source_mac, destination_mac, ether_type, offset, tags = header
    if ether_type != 0x0800 or len(frame) < offset + 40:
        return None
    version_ihl = frame[offset]
    ihl = (version_ihl & 0x0F) * 4
    if version_ihl >> 4 != 4 or ihl < 20 or len(frame) < offset + ihl + 20 or frame[offset + 9] != 6:
        return None
    total_length = struct.unpack_from("!H", frame, offset + 2)[0]
    tcp_offset = offset + ihl
    source_port, destination_port, sequence, acknowledgement = struct.unpack_from("!HHII", frame, tcp_offset)
    data_offset = (frame[tcp_offset + 12] >> 4) * 4
    if data_offset < 20 or len(frame) < tcp_offset + data_offset:
        return None
    payload_end = min(len(frame), offset + total_length)
    return CapturedTcp(
        source_mac, destination_mac,
        socket.inet_ntoa(frame[offset + 12:offset + 16]),
        socket.inet_ntoa(frame[offset + 16:offset + 20]),
        source_port, destination_port, sequence, acknowledgement,
        frame[tcp_offset + 13], frame[tcp_offset + data_offset:payload_end], tags,
    )


class RawPtcSocket:
    """Socket-like minimal TCP stream transported through Npcap."""

    def __init__(
        self, *, library: PcapLibrary, interface_name: str,
        lidar_ip: str, lidar_mac: str, port: int,
        vlan_tags: tuple[tuple[int, int], ...], timeout: float = 2.0,
        virtual_ip: str | None = None,
    ) -> None:
        address = ipaddress.ip_address(lidar_ip)
        if address.version != 4 or not (address.is_private or address.is_link_local):
            raise ValueError("二层 PTC 仅允许直接连接的私有 IPv4 雷达")
        self.library = library
        self.interface_name = interface_name
        self.lidar_ip = lidar_ip
        self.lidar_mac = _mac_bytes(lidar_mac)
        self.port = int(port)
        self.vlan_tags = tuple(vlan_tags)
        self.timeout = timeout
        self.source_mac = interface_mac(interface_name) or (
            b"\x02" + hashlib.blake2s(interface_name.encode(), digest_size=5).digest()
        )
        self.source_ip = virtual_ip or self._candidate_ip(lidar_ip)
        self.source_port = random.randint(49152, 65534)
        self.sequence = random.getrandbits(32)
        self.remote_sequence = 0
        self._buffer = bytearray()
        self._out_of_order: dict[int, bytes] = {}
        self._identification = random.getrandbits(16)
        self._closed = False
        ip = lidar_ip.encode("ascii")
        expression = (
            b"arp or (vlan and arp) or (vlan and vlan and arp) or "
            b"(tcp and host " + ip + b" and port " + str(port).encode("ascii") + b") or "
            b"(vlan and tcp and host " + ip + b" and port " + str(port).encode("ascii") + b") or "
            b"(vlan and vlan and tcp and host " + ip + b" and port " + str(port).encode("ascii") + b")"
        )
        self.handle, error = library.open(interface_name.encode(), expression=expression, timeout_ms=50)
        if not self.handle:
            raise OSError(f"无法打开点云所在网卡的二层 PTC 通道: {error}")
        try:
            self._connect()
        except Exception:
            self.close(abort=True)
            raise

    @staticmethod
    def _candidate_ip(lidar_ip: str) -> str:
        # /24 is the common direct-LiDAR control subnet.  This address exists
        # only in emitted frames and is never assigned to the Windows adapter.
        network = ipaddress.ip_network(f"{lidar_ip}/24", strict=False)
        preferred = ipaddress.ip_address(int(network.network_address) + 100)
        if preferred == ipaddress.ip_address(lidar_ip):
            preferred = ipaddress.ip_address(int(network.network_address) + 101)
        return str(preferred)

    def _send_frame(self, flags: int, payload: bytes = b"", *, sequence: int | None = None) -> None:
        self._identification = (self._identification + 1) & 0xFFFF
        options = b"\x02\x04\x05\xb4\x01\x03\x03\x08\x01\x01\x04\x02" if flags & TCP_SYN else b""
        frame = build_tcp_frame(
            source_mac=self.source_mac, destination_mac=self.lidar_mac,
            source_ip=self.source_ip, destination_ip=self.lidar_ip,
            source_port=self.source_port, destination_port=self.port,
            sequence=self.sequence if sequence is None else sequence,
            acknowledgement=self.remote_sequence, flags=flags, payload=payload,
            vlan_tags=self.vlan_tags, identification=self._identification, options=options,
        )
        if not self.library.send(self.handle, frame):
            raise OSError("Npcap 未能发送二层 PTC 报文")

    def _next(self, deadline: float) -> CapturedTcp | None:
        lib = self.library.library
        while time.monotonic() < deadline:
            header = ctypes.POINTER(PcapPacketHeader)()
            data = ctypes.POINTER(ctypes.c_ubyte)()
            result = lib.pcap_next_ex(self.handle, ctypes.byref(header), ctypes.byref(data))
            if result < 0:
                raise OSError("Npcap 二层 PTC 捕获中断")
            if result == 0 or not header or not data:
                continue
            packet = parse_ethernet_tcp(ctypes.string_at(data, header.contents.caplen))
            if packet is None:
                continue
            if (packet.source_ip, packet.destination_ip, packet.source_port, packet.destination_port) != (
                self.lidar_ip, self.source_ip, self.port, self.source_port,
            ):
                continue
            if packet.vlan_tags != self.vlan_tags:
                continue
            return packet
        return None

    def _connect(self) -> None:
        # Teach the LiDAR the ephemeral source IP/MAC without changing the OS.
        arp = build_arp_request(self.source_mac, self.source_ip, self.lidar_ip, self.vlan_tags)
        self.library.send(self.handle, arp)
        initial = self.sequence
        for _attempt in range(3):
            self._send_frame(TCP_SYN, sequence=initial)
            deadline = time.monotonic() + self.timeout
            while True:
                packet = self._next(deadline)
                if packet is None:
                    break
                if packet.flags & TCP_RST:
                    raise ConnectionRefusedError(f"雷达拒绝二层 PTC 端口 {self.port}")
                if packet.flags & TCP_SYN and packet.flags & TCP_ACK and packet.acknowledgement == (initial + 1) & 0xFFFFFFFF:
                    self.sequence = (initial + 1) & 0xFFFFFFFF
                    self.remote_sequence = (packet.sequence + 1) & 0xFFFFFFFF
                    self._send_frame(TCP_ACK)
                    return
        raise TimeoutError(f"雷达未响应 VLAN 二层 TCP/{self.port} 握手")

    def _accept(self, packet: CapturedTcp) -> None:
        if packet.flags & TCP_RST:
            raise ConnectionResetError("雷达重置了二层 PTC 连接")
        if packet.payload:
            if packet.sequence == self.remote_sequence:
                self._buffer.extend(packet.payload)
                self.remote_sequence = (self.remote_sequence + len(packet.payload)) & 0xFFFFFFFF
                while self.remote_sequence in self._out_of_order:
                    following = self._out_of_order.pop(self.remote_sequence)
                    self._buffer.extend(following)
                    self.remote_sequence = (self.remote_sequence + len(following)) & 0xFFFFFFFF
            elif packet.sequence not in self._out_of_order:
                # Retain a future segment; an old retransmission is simply ACKed.
                delta = (packet.sequence - self.remote_sequence) & 0xFFFFFFFF
                if delta < 0x80000000:
                    self._out_of_order[packet.sequence] = packet.payload
            self._send_frame(TCP_ACK)
        if packet.flags & TCP_FIN:
            self.remote_sequence = (max(self.remote_sequence, packet.sequence + len(packet.payload)) + 1) & 0xFFFFFFFF
            self._send_frame(TCP_ACK)

    def sendall(self, value: bytes) -> None:
        if self._closed:
            raise OSError("二层 PTC 连接已关闭")
        offset = 0
        while offset < len(value):
            chunk = value[offset:offset + 1200]
            sent_sequence = self.sequence
            target_ack = (sent_sequence + len(chunk)) & 0xFFFFFFFF
            # Incoming response data may be piggybacked on the ACK.  ACK that
            # data using the post-send sequence number.
            self.sequence = target_ack
            acknowledged = False
            for _attempt in range(3):
                self._send_frame(TCP_ACK | TCP_PSH, chunk, sequence=sent_sequence)
                deadline = time.monotonic() + self.timeout
                while True:
                    packet = self._next(deadline)
                    if packet is None:
                        break
                    self._accept(packet)
                    if ((packet.acknowledgement - target_ack) & 0xFFFFFFFF) < 0x80000000:
                        acknowledged = True
                        break
                if acknowledged:
                    break
            if not acknowledged:
                raise TimeoutError("雷达未确认二层 PTC 请求")
            offset += len(chunk)

    def recv(self, size: int) -> bytes:
        deadline = time.monotonic() + self.timeout
        while not self._buffer:
            packet = self._next(deadline)
            if packet is None:
                raise TimeoutError("等待二层 PTC 响应超时")
            self._accept(packet)
        value = bytes(self._buffer[:size])
        del self._buffer[:size]
        return value

    def close(self, abort: bool = False) -> None:
        if self._closed:
            return
        if self.handle:
            if not abort and self.remote_sequence:
                try:
                    self._send_frame(TCP_FIN | TCP_ACK)
                except OSError:
                    pass
            self.library.library.pcap_close(self.handle)
            self.handle = 0
        self._closed = True


def flow_vlan_tags(flow: dict) -> tuple[tuple[int, int], ...]:
    raw = flow.get("vlan_tags")
    if isinstance(raw, list) and raw:
        # Preserve the discovered VLAN stack and IDs.  Control traffic uses
        # best-effort priority (PCP 0); point-cloud streams often use PCP 5.
        return tuple((int(item[0]), int(item[1]) & 0x0FFF) for item in raw if isinstance(item, (list, tuple)) and len(item) == 2)
    return tuple((0x8100, int(vlan) & 0x0FFF) for vlan in flow.get("vlan_ids", []))
