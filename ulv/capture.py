"""Optional libpcap/Npcap layer-2 LiDAR discovery and packet input."""

from __future__ import annotations

import ctypes
import ctypes.util
from dataclasses import dataclass
import hashlib
import ipaddress
import socket
import struct
import sys
import threading
import time
from typing import Callable

from .detection import Detection, detect_packet, normalize_lidar_payload
from .registry import Device, DeviceRegistry


PCAP_ERRBUF_SIZE = 256
PCAP_IF_LOOPBACK = 0x1
DLT_EN10MB = 1
VLAN_ETHERTYPES = {0x8100, 0x88A8, 0x9100}


# Modern 32-bit musl (including ImmortalWrt/OpenWrt on MT7621) uses a 64-bit
# time_t even though C long is 32-bit.  Using c_long for tv_sec shifts caplen
# and length inside pcap_pkthdr, which can turn a normal packet into an invalid
# multi-gigabyte ctypes read and crash the interpreter.  Npcap keeps the
# Windows timeval ABI with 32-bit longs.
PcapTimeT = ctypes.c_long if sys.platform == "win32" else getattr(ctypes, "c_time_t", ctypes.c_long)


class TimeVal(ctypes.Structure):
    _fields_ = [("tv_sec", PcapTimeT), ("tv_usec", ctypes.c_long)]


class PcapPacketHeader(ctypes.Structure):
    _fields_ = [
        ("ts", TimeVal),
        ("caplen", ctypes.c_uint32),
        ("length", ctypes.c_uint32),
    ]


class PcapInterface(ctypes.Structure):
    pass


PcapInterface._fields_ = [
    ("next", ctypes.POINTER(PcapInterface)),
    ("name", ctypes.c_char_p),
    ("description", ctypes.c_char_p),
    ("addresses", ctypes.c_void_p),
    ("flags", ctypes.c_uint32),
]


class BpfProgram(ctypes.Structure):
    _fields_ = [("bf_len", ctypes.c_uint), ("bf_insns", ctypes.c_void_p)]


@dataclass(frozen=True, slots=True)
class CapturedUdp:
    source_mac: str
    source_ip: str
    destination_ip: str
    source_port: int
    destination_port: int
    vlan_ids: tuple[int, ...]
    vlan_tags: tuple[tuple[int, int], ...]
    payload: bytes


@dataclass(frozen=True, slots=True)
class CapturedArp:
    source_mac: str
    destination_mac: str
    operation: int
    sender_ip: str
    target_ip: str
    vlan_ids: tuple[int, ...]
    vlan_tags: tuple[tuple[int, int], ...]


def _mac_text(value: bytes) -> str:
    return ":".join(f"{octet:02x}" for octet in value)


def interface_mac(interface_name: str) -> bytes | None:
    """Resolve an Npcap device GUID to its Windows adapter MAC, read-only."""
    if sys.platform != "win32":
        return None
    class AdapterAddress(ctypes.Structure):
        pass
    AdapterAddress._fields_ = [
        ("Length", ctypes.c_ulong), ("IfIndex", ctypes.c_ulong),
        ("Next", ctypes.POINTER(AdapterAddress)), ("AdapterName", ctypes.c_char_p),
        ("FirstUnicastAddress", ctypes.c_void_p), ("FirstAnycastAddress", ctypes.c_void_p),
        ("FirstMulticastAddress", ctypes.c_void_p), ("FirstDnsServerAddress", ctypes.c_void_p),
        ("DnsSuffix", ctypes.c_wchar_p), ("Description", ctypes.c_wchar_p),
        ("FriendlyName", ctypes.c_wchar_p), ("PhysicalAddress", ctypes.c_ubyte * 8),
        ("PhysicalAddressLength", ctypes.c_ulong),
    ]
    size = ctypes.c_ulong(15_000)
    for _attempt in range(2):
        buffer = ctypes.create_string_buffer(size.value)
        result = ctypes.windll.iphlpapi.GetAdaptersAddresses(0, 0, None, buffer, ctypes.byref(size))
        if result == 111:
            continue
        if result != 0:
            return None
        target = interface_name.upper()
        current = ctypes.cast(buffer, ctypes.POINTER(AdapterAddress))
        while current:
            item = current.contents
            name = item.AdapterName.decode(errors="replace").upper() if item.AdapterName else ""
            if name and name in target and item.PhysicalAddressLength == 6:
                return bytes(item.PhysicalAddress[:6])
            current = item.Next
        return None
    return None


def _ethernet_header(frame: bytes) -> tuple[str, str, int, int, tuple[tuple[int, int], ...]] | None:
    if len(frame) < 14:
        return None
    destination_mac = _mac_text(frame[0:6])
    source_mac = _mac_text(frame[6:12])
    ether_type = struct.unpack_from("!H", frame, 12)[0]
    offset = 14
    tags: list[tuple[int, int]] = []
    while ether_type in VLAN_ETHERTYPES:
        if len(frame) < offset + 4:
            return None
        tci, next_type = struct.unpack_from("!HH", frame, offset)
        tags.append((ether_type, tci))
        ether_type = next_type
        offset += 4
    return source_mac, destination_mac, ether_type, offset, tuple(tags)


def parse_ethernet_udp(frame: bytes) -> CapturedUdp | None:
    """Parse Ethernet/802.1Q/QinQ + IPv4 + UDP without external packages."""
    header = _ethernet_header(frame)
    if header is None:
        return None
    source_mac, _destination_mac, ether_type, offset, tags = header
    if ether_type != 0x0800 or len(frame) < offset + 20:
        return None
    version_ihl = frame[offset]
    if version_ihl >> 4 != 4:
        return None
    ihl = (version_ihl & 0x0F) * 4
    if ihl < 20 or len(frame) < offset + ihl + 8 or frame[offset + 9] != 17:
        return None
    flags_fragment = struct.unpack_from("!H", frame, offset + 6)[0]
    if flags_fragment & 0x1FFF:
        return None
    source_ip = socket.inet_ntoa(frame[offset + 12:offset + 16])
    destination_ip = socket.inet_ntoa(frame[offset + 16:offset + 20])
    udp_offset = offset + ihl
    source_port, destination_port, udp_length = struct.unpack_from("!HHH", frame, udp_offset)
    if udp_length < 8:
        return None
    payload_end = min(len(frame), udp_offset + udp_length)
    return CapturedUdp(
        source_mac,
        source_ip,
        destination_ip,
        source_port,
        destination_port,
        tuple(tci & 0x0FFF for _tpid, tci in tags),
        tags,
        frame[udp_offset + 8:payload_end],
    )


def parse_ethernet_arp(frame: bytes) -> CapturedArp | None:
    """Parse Ethernet ARP requests/replies, retaining 802.1Q/QinQ tags."""
    header = _ethernet_header(frame)
    if header is None:
        return None
    source_mac, destination_mac, ether_type, offset, tags = header
    if ether_type != 0x0806 or len(frame) < offset + 28:
        return None
    hardware_type, protocol_type, hardware_length, protocol_length, operation = struct.unpack_from(
        "!HHBBH", frame, offset
    )
    if (hardware_type, protocol_type, hardware_length, protocol_length) != (1, 0x0800, 6, 4):
        return None
    sender_mac = _mac_text(frame[offset + 8:offset + 14])
    # Reject malformed frames whose Ethernet and ARP source disagree.
    if sender_mac != source_mac:
        return None
    return CapturedArp(
        source_mac=source_mac,
        destination_mac=destination_mac,
        operation=operation,
        sender_ip=socket.inet_ntoa(frame[offset + 14:offset + 18]),
        target_ip=socket.inet_ntoa(frame[offset + 24:offset + 28]),
        vlan_ids=tuple(tci & 0x0FFF for _tpid, tci in tags),
        vlan_tags=tags,
    )


def build_arp_reply(request: CapturedArp, responder_mac: bytes) -> bytes:
    """Build a VLAN-preserving ARP reply for a captured request."""
    if len(responder_mac) != 6 or request.operation != 1:
        raise ValueError("ARP reply requires a request and a six-byte MAC")
    requester_mac = bytes.fromhex(request.source_mac.replace(":", ""))
    ethernet = requester_mac + responder_mac
    if request.vlan_tags:
        for tpid, tci in request.vlan_tags:
            ethernet += struct.pack("!HH", tpid, tci)
        # The final tag currently contains its original TPID/TCI only; append
        # the encapsulated payload type after the stack.
        ethernet += struct.pack("!H", 0x0806)
    else:
        ethernet += struct.pack("!H", 0x0806)
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1,
        0x0800,
        6,
        4,
        2,
        responder_mac,
        socket.inet_aton(request.target_ip),
        requester_mac,
        socket.inet_aton(request.sender_ip),
    )
    # libpcap injection is not guaranteed to pad runt Ethernet frames. Always
    # meet the 60-byte minimum (without FCS) for strict automotive NICs.
    return (ethernet + arp).ljust(60, b"\x00")


class PcapLibrary:
    def __init__(self) -> None:
        candidates: list[str] = []
        if sys.platform == "win32":
            candidates.extend([
                r"C:\Windows\System32\Npcap\wpcap.dll",
                "wpcap.dll",
            ])
        else:
            found = ctypes.util.find_library("pcap")
            if found:
                candidates.append(found)
            candidates.extend(["libpcap.so.1", "libpcap.so", "libpcap.A.dylib"])
        last_error: OSError | None = None
        for candidate in dict.fromkeys(candidates):
            try:
                self.library = ctypes.CDLL(candidate)
                self.path = candidate
                break
            except OSError as exc:
                last_error = exc
        else:
            raise OSError("Npcap/libpcap not found") from last_error
        lib = self.library
        lib.pcap_findalldevs.argtypes = [
            ctypes.POINTER(ctypes.POINTER(PcapInterface)),
            ctypes.c_char_p,
        ]
        lib.pcap_findalldevs.restype = ctypes.c_int
        lib.pcap_freealldevs.argtypes = [ctypes.POINTER(PcapInterface)]
        lib.pcap_open_live.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        lib.pcap_open_live.restype = ctypes.c_void_p
        lib.pcap_datalink.argtypes = [ctypes.c_void_p]
        lib.pcap_datalink.restype = ctypes.c_int
        lib.pcap_next_ex.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(PcapPacketHeader)),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
        ]
        lib.pcap_next_ex.restype = ctypes.c_int
        lib.pcap_close.argtypes = [ctypes.c_void_p]
        lib.pcap_geterr.argtypes = [ctypes.c_void_p]
        lib.pcap_geterr.restype = ctypes.c_char_p
        lib.pcap_compile.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(BpfProgram),
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_uint32,
        ]
        lib.pcap_compile.restype = ctypes.c_int
        lib.pcap_setfilter.argtypes = [ctypes.c_void_p, ctypes.POINTER(BpfProgram)]
        lib.pcap_setfilter.restype = ctypes.c_int
        lib.pcap_freecode.argtypes = [ctypes.POINTER(BpfProgram)]
        lib.pcap_sendpacket.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int]
        lib.pcap_sendpacket.restype = ctypes.c_int

    def interfaces(self) -> list[tuple[bytes, str, int]]:
        head = ctypes.POINTER(PcapInterface)()
        error = ctypes.create_string_buffer(PCAP_ERRBUF_SIZE)
        if self.library.pcap_findalldevs(ctypes.byref(head), error) != 0:
            raise OSError(error.value.decode(errors="replace"))
        result: list[tuple[bytes, str, int]] = []
        try:
            current = head
            while current:
                item = current.contents
                if item.name and not (item.flags & PCAP_IF_LOOPBACK):
                    description = item.description.decode(errors="replace") if item.description else "network interface"
                    result.append((bytes(item.name), description, int(item.flags)))
                current = item.next
        finally:
            self.library.pcap_freealldevs(head)
        return result

    def open(self, name: bytes, expression: bytes | None = None, timeout_ms: int = 250) -> tuple[int, str | None]:
        error = ctypes.create_string_buffer(PCAP_ERRBUF_SIZE)
        handle = self.library.pcap_open_live(name, 65535, 1, timeout_ms, error)
        if not handle:
            return 0, error.value.decode(errors="replace")
        if self.library.pcap_datalink(handle) != DLT_EN10MB:
            self.library.pcap_close(handle)
            return 0, "unsupported non-Ethernet capture interface"
        program = BpfProgram()
        expression = expression or b"arp or udp or (vlan and (arp or udp)) or (vlan and vlan and (arp or udp))"
        if self.library.pcap_compile(handle, ctypes.byref(program), expression, 1, 0xFFFFFFFF) != 0:
            message = self.library.pcap_geterr(handle)
            self.library.pcap_close(handle)
            return 0, message.decode(errors="replace") if message else "invalid capture filter"
        try:
            if self.library.pcap_setfilter(handle, ctypes.byref(program)) != 0:
                message = self.library.pcap_geterr(handle)
                self.library.pcap_close(handle)
                return 0, message.decode(errors="replace") if message else "capture filter failed"
        finally:
            self.library.pcap_freecode(ctypes.byref(program))
        return int(handle), None

    def send(self, handle: int, frame: bytes) -> bool:
        data = (ctypes.c_ubyte * len(frame)).from_buffer_copy(frame)
        return self.library.pcap_sendpacket(handle, data, len(frame)) == 0


class PacketCapture:
    def __init__(
        self,
        registry: DeviceRegistry,
        packet_callback: Callable[..., None],
        source_networks: tuple[str, ...] = (),
        arp_assist_enabled: bool = True,
        local_addresses: tuple[str, ...] = (),
        raw_packet_callback: Callable[[bytes, int, dict], None] | None = None,
        network_adapt_callback: Callable[[dict], bool] | None = None,
        arp_network_adapt_callback: Callable[[dict], bool] | None = None,
    ) -> None:
        self.registry = registry
        self.packet_callback = packet_callback
        self.source_networks = tuple(ipaddress.ip_network(item) for item in source_networks)
        self.arp_assist_enabled = arp_assist_enabled
        self.local_addresses = tuple(local_addresses)
        self.raw_packet_callback = raw_packet_callback
        self.network_adapt_callback = network_adapt_callback
        self.arp_network_adapt_callback = arp_network_adapt_callback
        # A /24 guard is intentionally conservative for automatic ARP. It
        # prevents this process from answering for ordinary peers on the
        # host's existing LAN while still covering the common LiDAR setup.
        self._local_guard_networks = tuple(
            ipaddress.ip_network(f"{item}/24", strict=False)
            for item in self.local_addresses
            if not ipaddress.ip_address(item).is_loopback
        )
        self.library: PcapLibrary | None = None
        self.error: str | None = None
        self.active = False
        self.packet_count = 0
        self.accepted_packet_count = 0
        self._stop = threading.Event()
        self._handles: list[int] = []
        self._threads: list[threading.Thread] = []
        self._interfaces: list[dict] = []
        self._flows: dict[tuple, dict] = {}
        self._flow_states: dict[tuple, dict] = {}
        self._flow_lock = threading.Lock()
        self._dedupe: dict[bytes, int] = {}
        self._detection_cache: dict[tuple, object] = {}
        self._device_cache: dict[tuple[str, str, str], Device] = {}
        self._arp_requests = 0
        self._arp_replies = 0
        self._arp_candidates: dict[tuple, dict] = {}
        self._processing_suspended = False
        self._arp_adapt_attempts: set[tuple] = set()

    def start(self) -> bool:
        try:
            self.library = PcapLibrary()
            interfaces = self.library.interfaces()
        except OSError as exc:
            self.error = str(exc)
            return False
        for name, description, flags in interfaces:
            handle, error = self.library.open(name)
            public_name = name.decode(errors="replace")
            self._interfaces.append({
                "name": public_name,
                "description": description,
                "flags": flags,
                "state": "capturing" if handle else "error",
                "error": error,
            })
            if not handle:
                continue
            self._handles.append(handle)
            thread = threading.Thread(
                target=self._capture_loop,
                args=(handle, description, public_name),
                name=f"pcap-{len(self._threads)}",
                daemon=True,
            )
            self._threads.append(thread)
            thread.start()
        self.active = bool(self._handles)
        if not self.active:
            self.error = "no Ethernet capture interface could be opened; administrator/capture permission may be required"
        return self.active

    def _device_for(self, packet: CapturedUdp, payload: bytes, detection: Detection) -> tuple[Device, bool]:
        identity = packet.source_mac.lower() if packet.source_mac else packet.source_ip
        key = (detection.vendor.lower(), identity, packet.source_ip)
        device = self._device_cache.get(key)
        if device is None or device.ip != packet.source_ip:
            device = self.registry.observe(
                packet.source_ip, packet.source_port, packet.destination_port,
                len(payload), detection, source_mac=packet.source_mac,
            )
            self._device_cache[key] = device
            return device, False
        return device, True

    @staticmethod
    def _wired_interface(description: str) -> bool:
        """Exclude Wi-Fi, WAN and virtual adapters from active ARP replies."""
        lowered = description.casefold()
        excluded = (
            "wi-fi", "wifi", "wireless", "wlan", "wan miniport", "virtual",
            "hyper-v", "vmware", "bluetooth", "tunnel", "wintun", "loopback",
            "vpn", "tap", "npcap loopback",
        )
        return not any(token in lowered for token in excluded)

    def _arp_is_safe(self, request: CapturedArp, interface: str) -> tuple[bool, str]:
        if not self.arp_assist_enabled:
            return False, "disabled"
        if request.operation != 1 or request.destination_mac != "ff:ff:ff:ff:ff:ff":
            return False, "not-request"
        if not self._wired_interface(interface):
            return False, "non-wired-interface"
        try:
            sender = ipaddress.ip_address(request.sender_ip)
            target = ipaddress.ip_address(request.target_ip)
        except ValueError:
            return False, "invalid-address"
        if sender.version != 4 or target.version != 4:
            return False, "not-ipv4"
        if sender.is_unspecified or sender.is_multicast or sender.is_loopback:
            return False, "invalid-sender"
        if target.is_unspecified or target.is_multicast or target.is_loopback:
            return False, "invalid-target"
        if not (sender.is_private or sender.is_link_local):
            return False, "public-sender"
        if not (target.is_private or target.is_link_local):
            return False, "public-target"
        if request.target_ip in self.local_addresses:
            return False, "owned-by-host"
        if self.source_networks and not any(sender in network for network in self.source_networks):
            return False, "source-filter"
        if any(sender in network or target in network for network in self._local_guard_networks):
            return False, "host-lan"
        # Avoid ever impersonating the conventional gateway address.
        if int(str(target).split(".")[-1]) in (0, 1, 255):
            return False, "gateway-or-network"
        return True, "candidate"

    @staticmethod
    def _virtual_mac(interface_name: str) -> bytes:
        # A stable locally-administered unicast address avoids modifying the
        # OS network configuration. Promiscuous capture receives the UDP
        # frames that the LiDAR subsequently sends to this address.
        return interface_mac(interface_name) or (
            b"\x02" + hashlib.blake2s(interface_name.encode(), digest_size=5).digest()
        )

    def _handle_arp(
        self,
        handle: int,
        interface: str,
        interface_name: str,
        request: CapturedArp,
        now_ns: int,
    ) -> None:
        self._arp_requests += 1
        safe, reason = self._arp_is_safe(request, interface)
        key = (
            interface_name,
            request.source_mac,
            request.sender_ip,
            request.target_ip,
            request.vlan_ids,
        )
        with self._flow_lock:
            candidate = self._arp_candidates.get(key)
            if candidate is None:
                candidate = {
                    "interface": interface,
                    "source_mac": request.source_mac,
                    "source_ip": request.sender_ip,
                    "requested_ip": request.target_ip,
                    "vlan_ids": list(request.vlan_ids),
                    "requests": 0,
                    "replies": 0,
                    "eligible": safe,
                    "reason": reason,
                    "first_seen_ns": now_ns,
                    "last_seen_ns": now_ns,
                    "last_reply_ns": 0,
                }
                self._arp_candidates[key] = candidate
            candidate["requests"] += 1
            candidate["last_seen_ns"] = now_ns
            candidate["eligible"] = safe
            candidate["reason"] = reason
            request_count = candidate["requests"]
            last_reply_ns = candidate["last_reply_ns"]
        # Require repeated requests. A normal one-off LAN ARP is never enough
        # to activate the raw LiDAR path; repeat at most once per second.
        if request_count >= 3 and self.arp_network_adapt_callback is not None:
            self._adapt_arp_target(request, interface, interface_name, key)
        if not safe or request_count < 2 or now_ns - last_reply_ns < 1_000_000_000:
            return
        responder_mac = self._virtual_mac(interface_name)
        reply = build_arp_reply(request, responder_mac)
        assert self.library is not None
        sent = self.library.send(handle, reply)
        if sent:
            with self._flow_lock:
                candidate["replies"] += 1
                candidate["last_reply_ns"] = now_ns
                candidate["responder_mac"] = _mac_text(responder_mac)
            self._arp_replies += 1

    def _adapt_arp_target(self, request: CapturedArp, interface: str,
                          interface_name: str, key: tuple) -> None:
        """Bootstrap a known LiDAR before unresolved ARP suppresses all UDP."""
        if key in self._arp_adapt_attempts or not self.arp_assist_enabled:
            return
        try:
            sender = ipaddress.IPv4Address(request.sender_ip)
            target = ipaddress.IPv4Address(request.target_ip)
        except ipaddress.AddressValueError:
            return
        if (request.operation != 1 or request.destination_mac != "ff:ff:ff:ff:ff:ff"
                or not self._wired_interface(interface)
                or request.sender_ip in self.local_addresses
                or request.target_ip in self.local_addresses
                or sender == target or not (sender.is_private or sender.is_link_local)
                or not (target.is_private or target.is_link_local)
                or sender.is_loopback or target.is_loopback or sender.is_unspecified
                or target not in ipaddress.ip_network(f"{sender}/24", strict=False)
                or (int(target) & 255) in (0, 1, 255)):
            return
        if self.source_networks and not any(sender in network for network in self.source_networks):
            return
        # Previously decoded devices are trusted by their MAC. For first
        # contact, the supported RoboSense OUI includes the observed Bpearl.
        # Unknown ARP alone is insufficient evidence to change host addresses.
        mac = request.source_mac.lower()
        device = next((d for (_vendor, identity), d in self._device_cache.items()
                       if identity == mac), None)
        vendor = device.vendor if device else ("RoboSense" if mac.startswith("40:2c:76:") else "")
        if not vendor:
            return
        self._arp_adapt_attempts.add(key)
        self.arp_network_adapt_callback({
            "interface": interface, "interface_name": interface_name,
            "source_ip": request.sender_ip, "destination_ip": request.target_ip,
            "source_mac": mac, "vlan_ids": list(request.vlan_ids),
            "vendor": vendor, "trigger": "arp",
        })

    def _capture_loop(self, handle: int, interface: str, interface_name: str) -> None:
        assert self.library is not None
        lib = self.library.library
        registry_pending: dict[int, list] = {}
        local_flows: dict[tuple, tuple[dict, dict]] = {}

        def flush_registry() -> None:
            for device, count, byte_count, seen_ns in registry_pending.values():
                self.registry.accumulate(device, count, byte_count, seen_ns)
            registry_pending.clear()

        while not self._stop.is_set():
            header = ctypes.POINTER(PcapPacketHeader)()
            data = ctypes.POINTER(ctypes.c_ubyte)()
            result = lib.pcap_next_ex(handle, ctypes.byref(header), ctypes.byref(data))
            if result == 0:
                flush_registry()
                continue
            if result < 0 or not header or not data:
                flush_registry()
                return
            captured = ctypes.string_at(data, header.contents.caplen)
            self.packet_count += 1
            now_ns = time.time_ns()
            # UDP is overwhelmingly the hot path (M2 is about 12,600 pps).
            # Trying it first avoids parsing every UDP Ethernet/VLAN header a
            # second time merely to prove that it is not ARP.
            packet = parse_ethernet_udp(captured)
            if packet is None:
                arp = parse_ethernet_arp(captured)
                if arp is not None:
                    if self.raw_packet_callback is not None:
                        self.raw_packet_callback(captured, now_ns, {
                            "kind": "arp",
                            "interface": interface,
                            "interface_name": interface_name,
                            "source_ip": arp.sender_ip,
                            "destination_ip": arp.target_ip,
                            "vlan_ids": list(arp.vlan_ids),
                        })
                    if arp.operation == 1 and arp.destination_mac == "ff:ff:ff:ff:ff:ff":
                        self._handle_arp(handle, interface, interface_name, arp, now_ns)
                continue
            if not packet.payload:
                continue
            if self.source_networks:
                source_address = ipaddress.ip_address(packet.source_ip)
                if not any(source_address in network for network in self.source_networks):
                    continue
            payload = normalize_lidar_payload(packet.payload)
            detection_key = (
                packet.source_mac,
                packet.source_port,
                packet.destination_port,
                len(payload),
                payload[:8],
            )
            detection = self._detection_cache.get(detection_key)
            if detection is None:
                detection = detect_packet(payload, packet.destination_port)
                if detection is not None:
                    self._detection_cache[detection_key] = detection
            if detection is None:
                continue

            key = (
                packet.source_ip,
                packet.destination_ip,
                packet.source_port,
                packet.destination_port,
                packet.vlan_ids,
            )
            cached_flow = local_flows.get(key)
            new_flow = False
            if cached_flow is None:
                with self._flow_lock:
                    flow = self._flows.get(key)
                    if flow is None:
                        flow = {
                            "interface": interface,
                            "interface_name": interface_name,
                            "source_mac": packet.source_mac,
                            "vendor": detection.vendor,
                            "model": detection.model,
                            "source_ip": packet.source_ip,
                            "destination_ip": packet.destination_ip,
                            "source_port": packet.source_port,
                            "destination_port": packet.destination_port,
                            "vlan_ids": list(packet.vlan_ids),
                            "vlan_tags": [list(tag) for tag in packet.vlan_tags],
                            "packets": 0,
                            "bytes": 0,
                            "last_seen_ns": now_ns,
                        }
                        self._flows[key] = flow
                        self._flow_states[key] = {"owner": interface_name, "contested": False}
                        new_flow = True
                    state = self._flow_states[key]
                    if state["owner"] != interface_name:
                        state["contested"] = True
                    cached_flow = (flow, state)
                    local_flows[key] = cached_flow
            else:
                flow, state = cached_flow

            if new_flow and self.network_adapt_callback is not None:
                # The callback must return quickly; Application moves the
                # privileged adapter restart onto its own worker thread.
                self._processing_suspended = bool(self.network_adapt_callback(dict(flow)))
            if self._processing_suspended:
                # Avoid initializing a vendor SDK instance while the old IP/
                # VLAN is still active. AT128 initialization otherwise waits
                # for an expected-to-fail PTC attempt before capture can stop.
                continue

            # Hash-based de-duplication is only needed when the same network
            # flow is visible through more than one capture interface. On the
            # normal directly connected NIC it was consuming CPU for every
            # packet without ever finding a duplicate.
            if state["contested"]:
                fingerprint = hashlib.blake2s(
                    socket.inet_aton(packet.source_ip)
                    + socket.inet_aton(packet.destination_ip)
                    + struct.pack("!HHH", packet.source_port, packet.destination_port, len(payload))
                    + payload[:16]
                    + payload[-16:],
                    digest_size=12,
                ).digest()
                with self._flow_lock:
                    previous = self._dedupe.get(fingerprint, 0)
                    if now_ns - previous < 5_000_000:
                        continue
                    self._dedupe[fingerprint] = now_ns
                    if len(self._dedupe) > 8192:
                        cutoff = now_ns - 1_000_000_000
                        self._dedupe = {
                            item_key: value for item_key, value in self._dedupe.items() if value >= cutoff
                        }

            # Only creation and cross-interface de-duplication need the flow
            # lock. Status counters are approximate telemetry and updating
            # them under the GIL avoids a lock acquisition at LiDAR line rate.
            flow["packets"] += 1
            flow["bytes"] += len(payload)
            flow["last_seen_ns"] = now_ns
            self.accepted_packet_count += 1
            device, cached = self._device_for(packet, payload, detection)
            if cached:
                pending = registry_pending.get(device.id)
                if pending is None:
                    pending = [device, 0, 0, now_ns]
                    registry_pending[device.id] = pending
                pending[1] += 1
                pending[2] += len(payload)
                pending[3] = now_ns
                if pending[1] >= 64:
                    self.registry.accumulate(device, pending[1], pending[2], pending[3])
                    registry_pending.pop(device.id, None)
            timestamp_ns = int(header.contents.ts.tv_sec) * 1_000_000_000 + int(header.contents.ts.tv_usec) * 1000
            if self.raw_packet_callback is not None:
                self.raw_packet_callback(captured, timestamp_ns or now_ns, {
                    "kind": "lidar_udp",
                    "interface": interface,
                    "interface_name": interface_name,
                    "source_ip": packet.source_ip,
                    "destination_ip": packet.destination_ip,
                    "source_port": packet.source_port,
                    "destination_port": packet.destination_port,
                    "vlan_ids": list(packet.vlan_ids),
                    "vendor": detection.vendor,
                    "model": detection.model,
                    "payload": packet.payload,
                })
            self.packet_callback(
                device,
                payload,
                timestamp_ns or now_ns,
                packet.source_port,
                packet.destination_port,
            )
        flush_registry()

    def status(self) -> dict:
        now_ns = time.time_ns()
        with self._flow_lock:
            flows = []
            for flow in self._flows.values():
                public = dict(flow)
                public["last_seen_seconds"] = round((now_ns - public.pop("last_seen_ns")) / 1_000_000_000, 2)
                flows.append(public)
            arp_candidates = []
            for candidate in self._arp_candidates.values():
                public = dict(candidate)
                public["first_seen_seconds"] = round((now_ns - public.pop("first_seen_ns")) / 1_000_000_000, 2)
                public["last_seen_seconds"] = round((now_ns - public.pop("last_seen_ns")) / 1_000_000_000, 2)
                public.pop("last_reply_ns", None)
                arp_candidates.append(public)
        return {
            "active": self.active,
            "backend": "Npcap/libpcap" if self.library else "socket fallback",
            "library": self.library.path if self.library else None,
            "error": self.error,
            "packets": self.packet_count,
            "accepted_packets": self.accepted_packet_count,
            "interfaces": list(self._interfaces),
            "flows": sorted(flows, key=lambda item: item["last_seen_seconds"]),
            "arp_assist": {
                "enabled": self.arp_assist_enabled,
                "requests": self._arp_requests,
                "replies": self._arp_replies,
                "candidates": sorted(arp_candidates, key=lambda item: item["last_seen_seconds"]),
            },
        }

    def stop(self) -> None:
        self._stop.set()
        for thread in self._threads:
            thread.join(timeout=1)
        if self.library is not None:
            for handle in self._handles:
                self.library.library.pcap_close(handle)
        self._handles.clear()
        self._threads.clear()
        self.active = False
