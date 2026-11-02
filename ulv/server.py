"""Zero-dependency HTTP, WebSocket, discovery, and LiDAR service."""

from __future__ import annotations

import argparse
import base64
from collections import Counter
import hashlib
import json
import ipaddress
import mimetypes
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse
from urllib.request import urlopen
try:
    import webbrowser
except ImportError:  # OpenWrt intentionally removes this desktop-only module.
    webbrowser = None

from . import __version__
from .capture import PacketCapture
from .detection import detect_packet, normalize_lidar_payload
from .gateway import LidarGateway, system_snapshot
from .hesai_ptc import HesaiPtcClient
from .lidar_web_scraper import LidarWebScraper
from .native_plugins import NativePluginManager
from .network_bootstrap import (
    MacOSNetworkBootstrapManager,
    WindowsNetworkBootstrapManager,
    ensure_macos_admin,
    ensure_windows_admin,
)
from .pcap_replay import PcapReplay
from .ptp import PtpController
from .raw_ptc import RawPtcSocket, flow_vlan_tags
from .registry import DeviceRegistry


WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
# Standard online ports from Hesai SDK 2.0, RoboSense rs_driver and Velodyne.
# Binding to all of them lets a directly connected lidar work immediately.
AUTO_UDP_PORTS = (2368, 6699, 7788, 7766, 6688, 8010, 10110, 8308)
DEFAULT_UDP_PORTS = AUTO_UDP_PORTS


def local_ipv4_addresses() -> list[str]:
    """Best-effort, dependency-free list of host IPv4 addresses."""
    addresses: set[str] = set()
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.add(item[4][0])
    except OSError:
        pass
    # UDP connect performs only a route lookup; it does not transmit data.
    for target in ("192.0.2.1", "10.255.255.255"):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect((target, 9))
            addresses.add(probe.getsockname()[0])
        except OSError:
            pass
        finally:
            probe.close()
    addresses.discard("0.0.0.0")
    return sorted(addresses, key=lambda value: tuple(int(part) for part in value.split(".")))


def websocket_frame(payload: bytes, opcode: int = 2) -> bytes:
    first = 0x80 | opcode
    length = len(payload)
    if length < 126:
        return bytes((first, length)) + payload
    if length <= 0xFFFF:
        return bytes((first, 126)) + struct.pack("!H", length) + payload
    return bytes((first, 127)) + struct.pack("!Q", length) + payload


class WebSocketClient:
    def __init__(self, connection: socket.socket) -> None:
        self.connection = connection
        self.lock = threading.Lock()
        self.alive = True
        self.sent_frames = 0
        self.replaced_frames = 0
        self._pending_frame: bytes | None = None
        self._cv = threading.Condition(self.lock)
        self._thread = threading.Thread(target=self._sender_loop, daemon=True)
        self._thread.start()

    def _sender_loop(self) -> None:
        while self.alive:
            with self.lock:
                while self.alive and self._pending_frame is None:
                    self._cv.wait()
                if not self.alive:
                    break
                frame = self._pending_frame
                self._pending_frame = None
            try:
                self.connection.sendall(frame)
                with self.lock:
                    self.sent_frames += 1
            except OSError:
                self.alive = False
                break

    def send(self, payload: bytes, opcode: int = 2) -> bool:
        if not self.alive:
            return False
        frame = websocket_frame(payload, opcode)
        with self.lock:
            if self._pending_frame is not None:
                self.replaced_frames += 1
            self._pending_frame = frame
            self._cv.notify()
        return True

    def status(self) -> dict:
        with self.lock:
            return {
                "sent_frames": self.sent_frames,
                "replaced_frames": self.replaced_frames,
                "alive": self.alive,
            }

    def close(self) -> None:
        with self.lock:
            self.alive = False
            self._cv.notify_all()
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.connection.close()
        except OSError:
            pass


class WebSocketHub:
    def __init__(self) -> None:
        self._clients: set[WebSocketClient] = set()
        self._lock = threading.Lock()
        self._last_binary: bytes | None = None

    def add(self, client: WebSocketClient) -> None:
        with self._lock:
            self._clients.add(client)
            # The first decoded frame can arrive before the browser finishes
            # loading, especially while the NIC is being reconfigured. Replay
            # it immediately so the UI does not wait for physical link return.
            if self._last_binary is not None:
                client.send(self._last_binary)

    def remove(self, client: WebSocketClient) -> None:
        with self._lock:
            self._clients.discard(client)
        client.close()

    def broadcast_binary(self, payload: bytes) -> None:
        with self._lock:
            self._last_binary = payload
            clients = tuple(self._clients)
        dead = [client for client in clients if not client.send(payload)]
        for client in dead:
            self.remove(client)

    @property
    def client_count(self) -> int:
        with self._lock:
            return len(self._clients)

    def close_all(self) -> None:
        with self._lock:
            clients = tuple(self._clients)
            self._clients.clear()
        for client in clients:
            client.close()


class UdpListener:
    def __init__(
        self,
        bind_ip: str,
        port: int,
        source_networks: tuple[str, ...],
        registry: DeviceRegistry,
        packet_callback: callable,
        socket_capture_callback: callable | None = None,
    ) -> None:
        self.bind_ip = bind_ip
        self.port = port
        self.source_networks = tuple(ipaddress.ip_network(item, strict=False) for item in source_networks)
        self.registry = registry
        self.packet_callback = packet_callback
        self.socket_capture_callback = socket_capture_callback
        self.error: str | None = None
        self.packet_count = 0
        self.accepted_packet_count = 0
        self.receive_buffer_bytes = 0
        self.packet_sizes: Counter[int] = Counter()
        self.at128_azimuth = 0
        self.at128_azimuth_jumps = 0
        self._at128_last_azimuth = 0
        self._stop = threading.Event()
        self._socket: socket.socket | None = None
        self._thread = threading.Thread(target=self._run, name=f"udp-{port}", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def _run(self) -> None:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            # AT128 can exceed 1,000 packets/s. Keep reception independent from
            # calibration/decode work and give short scheduler stalls room.
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
            self.receive_buffer_bytes = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
            sock.settimeout(0.5)
            sock.bind((self.bind_ip, self.port))
            self._socket = sock
        except OSError as exc:
            self.error = str(exc)
            return

        while not self._stop.is_set():
            try:
                data, (source_ip, source_port) = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            self.packet_count += 1
            self.packet_sizes[len(data)] += 1
            if len(data) >= 16 and data[:4] == b"\xee\xff\x04\x03":
                laser_count = data[6]
                block_count = data[7]
                last_block_offset = 12 + (3 + laser_count * 4) * max(0, block_count - 1)
                if last_block_offset + 2 <= len(data):
                    azimuth = int.from_bytes(data[last_block_offset:last_block_offset + 2], "little")
                    if self._at128_last_azimuth and abs(azimuth - self._at128_last_azimuth) > 1000:
                        self.at128_azimuth_jumps += 1
                    self._at128_last_azimuth = azimuth
                    self.at128_azimuth = azimuth
            if self.source_networks:
                source_address = ipaddress.ip_address(source_ip)
                if not any(source_address in network for network in self.source_networks):
                    continue
            raw_data = data
            data = normalize_lidar_payload(data)
            detection = detect_packet(data, self.port)
            if detection:
                self.accepted_packet_count += 1
                timestamp_ns = time.time_ns()
                device = self.registry.observe(
                    source_ip,
                    source_port,
                    self.port,
                    len(data),
                    detection,
                )
                if self.socket_capture_callback is not None:
                    self.socket_capture_callback(
                        source_ip, self.bind_ip, source_port, self.port, raw_data, timestamp_ns
                    )
                self.packet_callback(
                    device,
                    data,
                    timestamp_ns,
                    source_port,
                    self.port,
                )

    def status(self) -> dict:
        return {
            "bind_ip": self.bind_ip,
            "port": self.port,
            "state": "error" if self.error else "listening",
            "error": self.error,
            "packets": self.packet_count,
            "accepted_packets": self.accepted_packet_count,
            "receive_buffer_bytes": self.receive_buffer_bytes,
            "packet_sizes": dict(self.packet_sizes.most_common(8)),
            "at128_azimuth_centidegrees": self.at128_azimuth,
            "at128_azimuth_jumps": self.at128_azimuth_jumps,
        }

    def stop(self) -> None:
        self._stop.set()
        if self._socket:
            self._socket.close()
        self._thread.join(timeout=1)


class Application:
    def __init__(
        self,
        listen_ip: str,
        udp_ports: tuple[int, ...],
        source_networks: tuple[str, ...],
        config_path: Path,
        plugin_path: Path,
        correction_path: Path | None = None,
        auto_discovery: bool = False,
        auto_arp_assist: bool = True,
        auto_network_adapt: bool = True,
        manual_listen_ip: str | None = None,
        manual_udp_ports: tuple[int, ...] | None = None,
        gateway_only: bool = False,
        shutdown_when_browser_closes: bool = True,
        license_session=None,
    ) -> None:
        self.license_session = license_session
        self.started_ns = time.time_ns()
        self.registry = DeviceRegistry()
        self.hub = WebSocketHub()
        self.gateway_only = gateway_only
        addresses = tuple(local_ipv4_addresses())
        self.gateway = LidarGateway(config_path.with_name("gateway.json"), addresses)
        self.ptp = PtpController(config_path.with_name("ptp.json"))
        self.native_plugins = NativePluginManager(
            plugin_path,
            self.hub.broadcast_binary,
            self.registry.update_model,
            default_correction_directory=correction_path,
            authorize=license_session.valid if license_session is not None else None,
        )
        self.pcap_replay = PcapReplay(self.registry, self.native_plugins.feed)
        self.config_path = config_path
        self._listener_lock = threading.Lock()
        self.auto_discovery = auto_discovery
        self.auto_arp_assist = auto_arp_assist
        self.auto_network_adapt = auto_network_adapt
        self.manual_listen_ip = manual_listen_ip or listen_ip
        self.manual_udp_ports = manual_udp_ports or udp_ports
        self.listen_ip = listen_ip
        self.udp_ports = udp_ports
        self.source_networks = source_networks
        self.listeners: list[UdpListener] = []
        self.capture: PacketCapture | None = None
        self._shutdown_callback: callable | None = None
        self._idle_shutdown_timer: threading.Timer | None = None
        self._browser_seen = False
        self.shutdown_when_browser_closes = shutdown_when_browser_closes
        self._stopping = threading.Event()
        self._startup_thread: threading.Thread | None = None
        self._network_adapt_lock = threading.Lock()
        self._network_profiles_attempted: set[tuple[str, str, str, int]] = set()
        self._adapted_radars: set[str] = set()
        manager_type = MacOSNetworkBootstrapManager if sys.platform == "darwin" else WindowsNetworkBootstrapManager
        self.network_manager = manager_type(config_path.with_name("network-recovery.json"))
        self._network_adapt_thread: threading.Thread | None = None
        self.network_adapt_status = {
            "state": "idle", "interface": "", "radar_ip": "",
            "requested_host_ip": "", "vlan_id": None, "message": "等待点云数据流",
        }

    def _make_listeners(self) -> list[UdpListener]:
        return [
            UdpListener(
                self.listen_ip, port, self.source_networks, self.registry,
                self._handle_packet, self.gateway.handle_socket_udp,
            )
            for port in self.udp_ports
        ]

    def start(self) -> None:
        if self._startup_thread is not None and self._startup_thread.is_alive():
            return
        self.network_adapt_status["state"] = "starting"
        self.network_adapt_status["message"] = "正在检查并恢复上次保留的网卡配置"

        def start_inputs_after_recovery() -> None:
            recovered, message = self.network_manager.recover_pending()
            if message:
                self.network_adapt_status["state"] = "recovered" if recovered else "error"
                self.network_adapt_status["message"] = message
            else:
                self.network_adapt_status["state"] = "idle"
                self.network_adapt_status["message"] = "等待点云数据流"
            if self._stopping.is_set():
                return
            with self._listener_lock:
                if self._stopping.is_set():
                    return
                self._start_inputs_locked()
            if not self._stopping.is_set():
                self.ptp.start_if_enabled()

        self._startup_thread = threading.Thread(
            target=start_inputs_after_recovery,
            name="lidar-startup-network-recovery",
            daemon=True,
        )
        self._startup_thread.start()

    def _handle_packet(self, device: object, data: bytes, timestamp_ns: int,
                       source_port: int, destination_port: int) -> None:
        if not self.gateway_only:
            self.native_plugins.feed(device, data, timestamp_ns, source_port, destination_port)

    def _handle_raw_packet(self, frame: bytes, timestamp_ns: int, metadata: dict) -> None:
        self.gateway.capture_frame(frame, timestamp_ns, metadata)
        if metadata.get("kind") == "lidar_udp":
            self.gateway.forward(
                metadata["payload"], metadata["source_ip"],
                metadata["source_port"], metadata["destination_port"],
            )

    def attach_shutdown(self, callback: callable) -> None:
        self._shutdown_callback = callback

    def browser_connected(self) -> None:
        self._browser_seen = True
        timer = self._idle_shutdown_timer
        self._idle_shutdown_timer = None
        if timer is not None:
            timer.cancel()

    def browser_disconnected(self, grace_seconds: float = 3.0) -> None:
        if (not self.shutdown_when_browser_closes or self._stopping.is_set()
                or not self._browser_seen or self.hub.client_count):
            return
        timer = self._idle_shutdown_timer
        if timer is not None:
            timer.cancel()

        def shutdown_if_idle() -> None:
            if (not self._stopping.is_set() and self.hub.client_count == 0
                    and self._shutdown_callback is not None):
                self._shutdown_callback()

        timer = threading.Timer(grace_seconds, shutdown_if_idle)
        timer.daemon = True
        self._idle_shutdown_timer = timer
        timer.start()

    def _start_inputs_locked(self) -> None:
        self.listeners = []
        self.capture = None
        if self.auto_discovery:
            capture = PacketCapture(
                self.registry,
                self._handle_packet,
                self.source_networks,
                arp_assist_enabled=self.auto_arp_assist,
                local_addresses=tuple(local_ipv4_addresses()),
                raw_packet_callback=self._handle_raw_packet,
                network_adapt_callback=(self._request_network_adapt
                                        if sys.platform in ("win32", "darwin") and self.auto_network_adapt else None),
                arp_network_adapt_callback=(self._request_network_adapt
                                            if sys.platform == "darwin" and self.auto_network_adapt else None),
            )
            self.capture = capture
            if capture.start():
                return
        # Manual mode, or automatic layer-2 capture unavailable: use ordinary
        # UDP sockets. Automatic mode falls back to all SDK standard ports.
        self.listeners = self._make_listeners()
        for listener in self.listeners:
            listener.start()

    @staticmethod
    def _host_ip_for_flow(flow: dict) -> str:
        radar = ipaddress.ip_address(str(flow["source_ip"]))
        destination = ipaddress.ip_address(str(flow.get("destination_ip", "0.0.0.0")))
        network = ipaddress.ip_network(f"{radar}/24", strict=False)
        if (destination.version == 4 and not destination.is_multicast
                and not destination.is_unspecified and destination in network
                and destination != radar and destination != network.broadcast_address):
            return str(destination)
        candidate = ipaddress.ip_address(int(network.network_address) + 100)
        if candidate == radar:
            candidate = ipaddress.ip_address(int(network.network_address) + 101)
        return str(candidate)

    def _request_network_adapt(self, flow: dict) -> bool:
        """Schedule one privileged VLAN/IP adaptation for a detected LiDAR flow."""
        if self._stopping.is_set() or not PacketCapture._wired_interface(str(flow.get("interface", ""))):
            return False
        try:
            radar_address = ipaddress.ip_address(str(flow["source_ip"]))
            if not (radar_address.is_private or radar_address.is_link_local):
                return False
            radar_ip = str(radar_address)
            host_ip = self._host_ip_for_flow(flow)
            vlan_id = int((flow.get("vlan_ids") or [0])[-1])
            interface_name = str(flow["interface_name"])
            interface = str(flow["interface"])
        except (KeyError, TypeError, ValueError):
            return False
        if radar_ip in self._adapted_radars:
            started_ns = int(self.network_adapt_status.get("started_ns") or 0)
            if started_ns and not self.network_adapt_status.get("point_flow_restored_ns"):
                restored_ns = time.time_ns()
                self.network_adapt_status["point_flow_restored_ns"] = restored_ns
                self.network_adapt_status["point_flow_reload_ms"] = round(
                    (restored_ns - started_ns) / 1_000_000, 1
                )
            return False
        profile = (interface_name, radar_ip, host_ip, vlan_id)
        if profile in self._network_profiles_attempted:
            return False
        if not self._network_adapt_lock.acquire(blocking=False):
            return False
        self._network_profiles_attempted.add(profile)
        self.network_adapt_status = {
            "state": "configuring", "interface": interface,
            "radar_ip": radar_ip, "requested_host_ip": host_ip,
            "vlan_id": vlan_id,
            "started_ns": time.time_ns(),
            "message": f"已识别点云流，正在临时设置 {host_ip}/24、VLAN {vlan_id}",
        }
        baseline_frames = sum(
            int(plugin.frames) for plugin in self.native_plugins.plugins
            if plugin.vendor.casefold() == str(flow.get("vendor", "")).casefold()
        )

        def worker() -> None:
            try:
                # Let the already captured VLAN stream produce a visible frame
                # before the physical adapter briefly drops link. The decoder
                # uses a local calibration source and does not wait for PTC.
                frame_deadline = time.monotonic() + 0.45
                while time.monotonic() < frame_deadline and not self._stopping.is_set():
                    current_frames = sum(
                        int(plugin.frames) for plugin in self.native_plugins.plugins
                        if plugin.vendor.casefold() == str(flow.get("vendor", "")).casefold()
                    )
                    if current_frames > baseline_frames:
                        ready_ns = time.time_ns()
                        self.network_adapt_status["first_pointcloud_ns"] = ready_ns
                        self.network_adapt_status["first_pointcloud_ms"] = round(
                            (ready_ns - int(self.network_adapt_status["started_ns"])) / 1_000_000,
                            1,
                        )
                        break
                    time.sleep(0.01)
                with self._listener_lock:
                    self._stop_inputs_locked()
                    self.capture = None
                    self.listeners = []
                success, message = self.network_manager.apply(
                    interface_name, radar_ip, host_ip, vlan_id,
                )
                if success:
                    self._adapted_radars.add(radar_ip)
                completed_ns = time.time_ns()
                self.network_adapt_status["state"] = "completed" if success else "error"
                self.network_adapt_status["message"] = message
                self.network_adapt_status["completed_ns"] = completed_ns
                self.network_adapt_status["adapter_apply_ms"] = round(
                    (completed_ns - int(self.network_adapt_status["started_ns"])) / 1_000_000,
                    1,
                )
            finally:
                with self._listener_lock:
                    if not self._stopping.is_set():
                        self._start_inputs_locked()
                self._network_adapt_lock.release()

        self._network_adapt_thread = threading.Thread(target=worker, name="lidar-network-adapt", daemon=True)
        self._network_adapt_thread.start()
        return False

    def _stop_inputs_locked(self) -> None:
        if self.capture is not None:
            self.capture.stop()
        for listener in self.listeners:
            listener.stop()

    def config(self) -> dict:
        return {
            "auto_discovery": self.auto_discovery,
            "auto_arp_assist": self.auto_arp_assist,
            "auto_network_adapt": self.auto_network_adapt,
            "listen_ip": self.manual_listen_ip,
            "udp_ports": list(self.manual_udp_ports),
            "effective_listen_ip": self.listen_ip,
            "effective_udp_ports": list(self.udp_ports),
            "automatic_udp_ports": list(AUTO_UDP_PORTS),
            "local_ipv4_addresses": local_ipv4_addresses(),
            "vlan_mode": "layer-2 capture" if self.capture and self.capture.active else "operating-system interface",
            "source_networks": list(self.source_networks),
            "network_adapt": dict(self.network_adapt_status),
        }

    @staticmethod
    def validate_config(value: dict) -> tuple[str, tuple[int, ...], tuple[str, ...]]:
        listen_ip = str(value.get("listen_ip", "0.0.0.0")).strip()
        try:
            address = ipaddress.ip_address(listen_ip)
        except ValueError as exc:
            raise ValueError("监听 IP 不是有效的 IP 地址") from exc
        if address.version != 4:
            raise ValueError("当前版本仅支持 IPv4 监听地址")

        raw_ports = value.get("udp_ports", [])
        if not isinstance(raw_ports, list):
            raise ValueError("udp_ports 必须是数组")
        try:
            udp_ports = tuple(dict.fromkeys(int(port) for port in raw_ports))
        except (TypeError, ValueError) as exc:
            raise ValueError("监听端口必须是整数") from exc
        if not udp_ports or any(port < 1 or port > 65535 for port in udp_ports):
            raise ValueError("至少配置一个 1 到 65535 之间的 UDP 端口")

        raw_networks = value.get("source_networks", [])
        if not isinstance(raw_networks, list):
            raise ValueError("source_networks 必须是数组")
        source_networks: list[str] = []
        for item in raw_networks:
            text = str(item).strip()
            if not text:
                continue
            try:
                network = ipaddress.ip_network(text, strict=False)
            except ValueError as exc:
                raise ValueError(f"无效的雷达源 IP 或 CIDR：{text}") from exc
            if network.version != 4:
                raise ValueError("当前版本仅支持 IPv4 源地址过滤")
            source_networks.append(str(network))
        return listen_ip, udp_ports, tuple(dict.fromkeys(source_networks))

    @staticmethod
    def resolve_config(
        value: dict,
    ) -> tuple[bool, str, tuple[int, ...], str, tuple[int, ...], tuple[str, ...]]:
        manual_ip, manual_ports, source_networks = Application.validate_config(value)
        auto_discovery = bool(value.get("auto_discovery", False))
        effective_ip = "0.0.0.0" if auto_discovery else manual_ip
        effective_ports = AUTO_UDP_PORTS if auto_discovery else manual_ports
        return (
            auto_discovery,
            manual_ip,
            manual_ports,
            effective_ip,
            effective_ports,
            source_networks,
        )

    def reconfigure(self, value: dict) -> dict:
        (auto_discovery, manual_ip, manual_ports, listen_ip,
         udp_ports, source_networks) = self.resolve_config(value)
        with self._listener_lock:
            self._stop_inputs_locked()
            self.auto_discovery = auto_discovery
            self.auto_arp_assist = bool(value.get("auto_arp_assist", True))
            self.manual_listen_ip = manual_ip
            self.manual_udp_ports = manual_ports
            self.listen_ip = listen_ip
            self.udp_ports = udp_ports
            self.source_networks = source_networks
            self._start_inputs_locked()
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        self.config_path.write_text(
            json.dumps(self.config(), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        time.sleep(0.05)
        errors = [listener.status() for listener in self.listeners if listener.error]
        return {
            "config": self.config(),
            "listener_errors": errors,
            "capture": self.capture.status() if self.capture else None,
        }

    def status(self) -> dict:
        with self._listener_lock:
            listener_status = [listener.status() for listener in self.listeners]
            capture_status = self.capture.status() if self.capture else None
        return {
            "name": "Universal LiDAR Viewer",
            "version": __version__,
            "uptime_seconds": round((time.time_ns() - self.started_ns) / 1_000_000_000, 1),
            "websocket_clients": self.hub.client_count,
            "listeners": listener_status,
            "capture": capture_status,
            "config": self.config(),
            "native_plugins": self.native_plugins.status(),
            "pcap": self.pcap_replay.status(),
            "gateway": self.gateway.status(),
            "ptp": self.ptp.status(),
            "gateway_only": self.gateway_only,
            "network_adapt": dict(self.network_adapt_status),
            "capabilities": {
                "ptp_supported": sys.platform != "win32",
                "ptp_disabled_reason": "Windows 平台暂不支持硬件 PTP 主时钟控制" if sys.platform == "win32" else None,
            },
            "diagnostics": {
                "point_frame_seen": False,
                "nic_traffic_seen": bool(self.capture and self.capture.active),
                "radar_flow_seen": bool(self.registry.list_public()),
                "model_identified": bool(self.registry.list_public()),
                "calibration_ready": True,
            },
        }

    def support_bundle(self) -> dict:
        return {
            "schema": 1,
            "status": self.status(),
            "calibration_assets": [],
        }

    def query_hesai_ptc(self, target_ip: str, port: int = 9347) -> dict:
        """Use normal TCP first, then the VLAN/MAC learned from live point cloud."""
        deadline = time.monotonic() + 20.0
        while self.network_adapt_status.get("state") == "configuring" and time.monotonic() < deadline:
            time.sleep(0.2)
        client = HesaiPtcClient(timeout=2.0)
        direct = client.query_all(target_ip, port=port)
        if direct.get("success"):
            return direct

        capture = self.capture
        if capture is None or capture.library is None:
            return direct
        status = capture.status()
        candidates = [
            flow for flow in status.get("flows", [])
            if flow.get("source_ip") == target_ip
            and flow.get("source_mac")
            and flow.get("interface_name")
            and flow.get("last_seen_seconds", 999) <= 10
            and ("hesai" in str(flow.get("vendor", "")).casefold()
                 or "at128" in str(flow.get("model", "")).casefold())
        ]
        if not candidates:
            result = dict(direct)
            result["adaptive_fallback"] = {
                "attempted": False,
                "reason": "未找到与目标 IP 匹配的实时禾赛点云流",
            }
            return result
        flow = min(candidates, key=lambda item: item.get("last_seen_seconds", 999))
        tags = flow_vlan_tags(flow)
        try:
            raw = RawPtcSocket(
                library=capture.library,
                interface_name=str(flow["interface_name"]),
                lidar_ip=target_ip,
                lidar_mac=str(flow["source_mac"]),
                port=port,
                vlan_tags=tags,
                timeout=1.0,
            )
            connection = {
                "mode": "npcap_layer2",
                "target_ip": target_ip,
                "target_port": port,
                "target_mac": flow["source_mac"],
                "interface": flow.get("interface"),
                "interface_name": flow["interface_name"],
                "vlan_ids": list(flow.get("vlan_ids", [])),
                "virtual_source_ip": raw.source_ip,
                "changes_windows_network": False,
            }
            return client.query_connected(
                raw, ip=target_ip, port=port,
                source="hesai_ptc_layer2", connection=connection,
            )
        except Exception as exc:
            network = ipaddress.ip_network(f"{target_ip}/24", strict=False)
            suggested_ip = str(ipaddress.ip_address(int(network.network_address) + 100))
            if suggested_ip == target_ip:
                suggested_ip = str(ipaddress.ip_address(int(network.network_address) + 101))
            result = dict(direct)
            result["adaptive_fallback"] = {
                "attempted": True,
                "mode": "npcap_layer2",
                "interface": flow.get("interface"),
                "vlan_ids": list(flow.get("vlan_ids", [])),
                "error": str(exc),
                "layer2_response_received": False,
                "manual_profile": {
                    "host_ip": suggested_ip,
                    "prefix_length": 24,
                    "vlan_id": (flow.get("vlan_ids") or [0])[-1],
                    "ptc_port": port,
                },
                "changes_windows_network": False,
            }
            vlan = (flow.get("vlan_ids") or [0])[-1]
            result["error"] = (
                f"{direct.get('error', '普通 PTC 失败')}；二层自适应失败：{exc}。"
                f"已自动识别雷达 {target_ip}、VLAN {vlan}、PTC {port}；"
                f"网卡自动适配状态：{self.network_adapt_status.get('message', '未知')}。"
            )
            return result

    def import_pcap(self, stream: object, length: int, display_name: str, loop: bool) -> dict:
        if length < 24:
            raise ValueError("PCAP 文件为空或过小")
        if length > 4 * 1024 * 1024 * 1024:
            raise ValueError("PCAP 文件超过 4 GB 上限")
        safe_name = Path(display_name).name[:200] or "capture.pcap"
        temporary = tempfile.NamedTemporaryFile(prefix="ulv-pcap-", suffix=".pcap", delete=False)
        temporary_path = Path(temporary.name)
        try:
            remaining = length
            with temporary:
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError("PCAP 上传中断")
                    temporary.write(chunk)
                    remaining -= len(chunk)
            return self.pcap_replay.start(
                temporary_path,
                safe_name,
                loop=loop,
                delete_file=True,
            )
        except Exception:
            temporary.close()
            temporary_path.unlink(missing_ok=True)
            raise

    def stop(self) -> None:
        self._stopping.set()
        if self._idle_shutdown_timer is not None:
            self._idle_shutdown_timer.cancel()
            self._idle_shutdown_timer = None
        with self._listener_lock:
            self._stop_inputs_locked()
        worker = self._network_adapt_thread
        if worker is not None and worker is not threading.current_thread():
            worker.join()
        restored, message = self.network_manager.restore_all()
        if message:
            self.network_adapt_status["state"] = "restored" if restored else "error"
            self.network_adapt_status["message"] = message
        self.pcap_replay.stop()
        self.ptp.close()
        self.gateway.close()
        self.hub.close_all()
        self.native_plugins.close()


class ViewerHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    # Never let a second viewer silently split network traffic with this one.
    allow_reuse_address = False

    def __init__(self, address: tuple[str, int], app: Application, web_root: Path) -> None:
        self.app = app
        self.web_root = web_root.resolve()
        super().__init__(address, ViewerHandler)


class ViewerHandler(BaseHTTPRequestHandler):
    server: ViewerHTTPServer

    def log_message(self, format: str, *args: object) -> None:
        return

    def _json(self, value: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _lidar_target(self, default_port: int) -> tuple[str, int]:
        """Accept only literal local-network addresses for read-only device queries."""
        query = parse_qs(urlparse(self.path).query)
        target = query.get("ip", [""])[0].strip()
        if not target:
            devices = self.server.app.registry.list_public()
            target = str(devices[0].get("ip", "")) if devices else ""
        try:
            address = ipaddress.ip_address(target)
        except ValueError as exc:
            raise ValueError("雷达 IP 必须是有效的 IPv4 地址") from exc
        if address.version != 4 or not (address.is_private or address.is_link_local or address.is_loopback):
            raise ValueError("只允许查询本机测试网络中的雷达地址")
        raw_port = query.get("port", [str(default_port)])[0].strip()
        try:
            port = int(raw_port)
        except ValueError as exc:
            raise ValueError("端口必须是整数") from exc
        if not 1 <= port <= 65535:
            raise ValueError("端口超出有效范围")
        return str(address), port

    def do_GET(self) -> None:  # noqa: N802
        session = self.server.app.license_session
        if session is not None and not session.valid():
            self._json({"error": session.result.reason}, HTTPStatus.FORBIDDEN)
            return
        path = urlparse(self.path).path
        if path == "/api/status":
            self._json(self.server.app.status())
            return
        if path == "/api/devices":
            self._json({"devices": self.server.app.registry.list_public()})
            return
        if path == "/api/config":
            self._json(self.server.app.config())
            return
        if path == "/api/gateway":
            result = self.server.app.gateway.status()
            result["storage"] = self.server.app.gateway.storage()
            result["ptp"] = self.server.app.ptp.status()
            result["system"] = system_snapshot()
            self._json(result)
            return
        if path in ("/api/lidar-web", "/api/hesai-ptc"):
            try:
                target_ip, port = self._lidar_target(80 if path == "/api/lidar-web" else 9347)
                if path == "/api/lidar-web":
                    result = LidarWebScraper().scrape(target_ip, port=port)
                    if not result.get("success"):
                        result = self.server.app.query_hesai_ptc(target_ip, port=9347)
                else:
                    result = self.server.app.query_hesai_ptc(target_ip, port=port)
                self._json(result)
            except ValueError as exc:
                self._json({"success": False, "error": str(exc)}, HTTPStatus.BAD_REQUEST)
            except Exception as exc:
                self._json({"success": False, "error": f"读取雷达参数失败：{exc}"}, HTTPStatus.BAD_GATEWAY)
            return
        if path == "/ws/points":
            self._upgrade_websocket()
            return
        self._serve_static(path)

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        session = self.server.app.license_session
        if path != "/api/shutdown" and session is not None and not session.valid():
            self._json({"error": session.result.reason}, HTTPStatus.FORBIDDEN)
            return
        if path == "/api/shutdown":
            try:
                caller = ipaddress.ip_address(self.client_address[0])
            except ValueError:
                caller = None
            if caller is None or not caller.is_loopback:
                self._json({"error": "仅允许从本机安全退出程序"}, HTTPStatus.FORBIDDEN)
                return
            self._json({"success": True, "message": "正在退出并恢复网卡配置"})
            if self.server.app._shutdown_callback is not None:
                threading.Thread(target=self.server.app._shutdown_callback, daemon=True).start()
            return
        if path == "/api/config":
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 65536:
                    raise ValueError("配置请求内容为空或过大")
                value = json.loads(self.rfile.read(length).decode("utf-8"))
                result = self.server.app.reconfigure(value)
                self._json(result)
            except (ValueError, json.JSONDecodeError) as exc:
                self._json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return
        if path == "/api/pcap/replay":
            try:
                length = int(self.headers.get("Content-Length", "0"))
                filename = unquote(self.headers.get("X-PCAP-Filename", "capture.pcap"))
                loop = self.headers.get("X-PCAP-Loop", "1") != "0"
                result = self.server.app.import_pcap(self.rfile, length, filename, loop)
                self._json({"pcap": result})
            except (OSError, ValueError) as exc:
                self._json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return
        if path == "/api/pcap/stop":
            self._json({"pcap": self.server.app.pcap_replay.stop()})
            return
        if path == "/api/gateway/config":
            try:
                value = self._read_json_body()
                self._json(self.server.app.gateway.configure(value))
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                self._json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return
        if path == "/api/gateway/recording/start":
            try:
                value = self._read_json_body()
                self._json(self.server.app.gateway.start_recording(value))
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                self._json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return
        if path == "/api/gateway/recording/stop":
            self._json(self.server.app.gateway.recorder.stop())
            return
        if path == "/api/ptp/config":
            try:
                value = self._read_json_body()
                self._json(self.server.app.ptp.configure(value))
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                self._json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return
        self._json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def _read_json_body(self) -> dict:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 65536:
            raise ValueError("配置请求内容为空或过大")
        value = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("配置内容必须是对象")
        return value

    def _serve_static(self, request_path: str) -> None:
        if request_path == "/":
            relative = "gateway.html" if self.server.app.gateway_only else "index.html"
        else:
            relative = request_path.lstrip("/")
        candidate = (self.server.web_root / relative).resolve()
        try:
            candidate.relative_to(self.server.web_root)
        except ValueError:
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if not candidate.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        content = candidate.read_bytes()
        content_type = mimetypes.guess_type(candidate.name)[0] or "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(content)

    def _upgrade_websocket(self) -> None:
        if self.headers.get("Upgrade", "").lower() != "websocket":
            self.send_error(HTTPStatus.UPGRADE_REQUIRED)
            return
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(HTTPStatus.BAD_REQUEST)
            return
        accept = base64.b64encode(hashlib.sha1((key + WEBSOCKET_GUID).encode()).digest()).decode()
        self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()

        client = WebSocketClient(self.connection)
        self.server.app.browser_connected()
        self.server.app.hub.add(client)
        self.close_connection = True
        self.connection.settimeout(1.0)
        try:
            self._websocket_read_loop(client)
        finally:
            self.server.app.hub.remove(client)
            self.server.app.browser_disconnected()

    def _websocket_read_loop(self, client: WebSocketClient) -> None:
        while client.alive:
            try:
                header = self.connection.recv(2)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(header) < 2:
                return
            opcode = header[0] & 0x0F
            masked = bool(header[1] & 0x80)
            length = header[1] & 0x7F
            if length == 126:
                raw = self.connection.recv(2)
                if len(raw) != 2:
                    return
                length = struct.unpack("!H", raw)[0]
            elif length == 127:
                raw = self.connection.recv(8)
                if len(raw) != 8:
                    return
                length = struct.unpack("!Q", raw)[0]
            mask = self.connection.recv(4) if masked else b""
            payload = bytearray()
            while len(payload) < length:
                chunk = self.connection.recv(min(65536, length - len(payload)))
                if not chunk:
                    return
                payload.extend(chunk)
            if masked:
                for index in range(len(payload)):
                    payload[index] ^= mask[index % 4]
            if opcode == 0x8:
                return
            if opcode == 0x9:
                client.send(bytes(payload), opcode=0xA)


def parse_ports(value: str) -> tuple[int, ...]:
    ports = tuple(dict.fromkeys(int(item.strip()) for item in value.split(",") if item.strip()))
    if not ports or any(port < 1 or port > 65535 for port in ports):
        raise argparse.ArgumentTypeError("ports must be comma-separated values from 1 to 65535")
    return ports


def bundled_project_root() -> Path:
    bundle_root = getattr(sys, "_MEIPASS", None)
    return Path(bundle_root) if bundle_root else Path(__file__).resolve().parents[1]


def runtime_config_path(project_root: Path) -> Path:
    if not getattr(sys, "frozen", False) and "__compiled__" not in globals():
        return project_root / "config" / "runtime.json"
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        return base / "UniversalLidarViewer" / "runtime.json"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "UniversalLidarViewer" / "runtime.json"
    base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "universal-lidar-viewer" / "runtime.json"


def native_plugin_path(project_root: Path) -> Path:
    if getattr(sys, "frozen", False):
        external = Path(sys.executable).resolve().parent / "plugins"
        if external.is_dir() and any(
            path.suffix.lower() in (".dll", ".so", ".dylib") for path in external.iterdir()
        ):
            return external
        bundled = project_root / "plugins"
        if bundled.is_dir():
            return bundled
        return external
    return project_root / "plugins"


def existing_viewer(url: str) -> bool:
    """Return true only when the occupied HTTP port belongs to this app."""
    try:
        with urlopen(f"{url}/api/status", timeout=0.8) as response:
            status = json.loads(response.read().decode("utf-8"))
        return status.get("name") == "Universal LiDAR Viewer"
    except (OSError, ValueError, json.JSONDecodeError):
        return False


def main(license_session=None) -> int:
    if license_session is not None and not license_session.check(force=True).valid:
        return 3
    parser = argparse.ArgumentParser(description="Universal LiDAR Viewer MVP")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP bind address")
    parser.add_argument("--port", type=int, default=17777, help="HTTP port")
    parser.add_argument("--listen-ip", help="UDP bind address; overrides saved configuration")
    parser.add_argument("--udp-ports", type=parse_ports, help="comma-separated UDP ports; overrides saved configuration")
    parser.add_argument("--no-browser", action="store_true", help="do not open the browser")
    parser.add_argument("--offline-test", type=Path, help="decode one PCAP or a directory without network capture")
    parser.add_argument("--offline-report", type=Path, help="write the offline test JSON report")
    parser.add_argument("--offline-cache", type=Path, help="explicit calibration cache for offline test")
    parser.add_argument("--service-mode", action="store_true", help="keep running when all browser pages close")
    parser.add_argument("--no-elevation", action="store_true", help="disable automatic administrator authorization and host-network adaptation")
    parser.add_argument("--gateway-only", action="store_true", help="run discovery/forwarding/PCAP/PTP without decoder plugins")
    args = parser.parse_args()

    project_root = bundled_project_root()
    if args.offline_test is not None:
        from .offline_test import run_offline_test, write_report
        report = run_offline_test(
            args.offline_test,
            native_plugin_path(project_root),
            project_root / "corrections",
            args.offline_cache,
            authorize=license_session.valid if license_session is not None else None,
        )
        write_report(report, args.offline_report)
        return 0 if report["success"] else 4
    url_host = "127.0.0.1" if args.host == "0.0.0.0" else args.host
    default_url = f"http://{url_host}:{args.port}"
    if existing_viewer(default_url):
        print(f"Universal LiDAR Viewer is already running at {default_url}")
        if not args.no_browser and webbrowser is not None:
            webbrowser.open(default_url)
        return 0

    if not args.no_elevation:
        if not ensure_windows_admin() or not ensure_macos_admin():
            print("Administrator permission was declined; automatic packet capture/network adaptation is unavailable.")
    if not args.no_elevation and (project_root / 'vendor/npcap/npcap-1.90.exe').is_file():
        from .npcap_bootstrap import ensure_npcap
        ensure_npcap(project_root)
    config_path = runtime_config_path(project_root)
    saved_config: dict = {}
    if config_path.is_file():
        try:
            saved_config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            saved_config = {}
    command_line_network = args.listen_ip is not None or args.udp_ports is not None
    initial_config = {
        "auto_discovery": False if command_line_network else saved_config.get("auto_discovery", True),
        "auto_arp_assist": saved_config.get("auto_arp_assist", True),
        "listen_ip": args.listen_ip or saved_config.get("listen_ip", "0.0.0.0"),
        "udp_ports": list(args.udp_ports or tuple(saved_config.get("udp_ports", DEFAULT_UDP_PORTS))),
        "source_networks": saved_config.get("source_networks", []),
    }
    (auto_discovery, manual_ip, manual_ports, listen_ip,
     udp_ports, source_networks) = Application.resolve_config(initial_config)
    app = Application(
        listen_ip,
        udp_ports,
        source_networks,
        config_path,
        native_plugin_path(project_root),
        project_root / "corrections" / "hesai",
        auto_discovery=auto_discovery,
        auto_arp_assist=bool(initial_config["auto_arp_assist"]),
        auto_network_adapt=not args.no_elevation,
        manual_listen_ip=manual_ip,
        manual_udp_ports=manual_ports,
        gateway_only=args.gateway_only,
        shutdown_when_browser_closes=not args.service_mode,
        license_session=license_session,
    )
    url_host = "127.0.0.1" if args.host == "0.0.0.0" else args.host
    candidate_ports = [args.port]
    if args.port == 17777:
        candidate_ports.extend([18888, 27777, 37777])

    server = None
    last_error = None
    for port in candidate_ports:
        url = f"http://{url_host}:{port}"
        try:
            server = ViewerHTTPServer((args.host, port), app, project_root / "web")
            if port != args.port:
                print(f"[提示] 默认端口 {args.port} 被 Windows 保留或占用 (WinError 10013/10048)，已自动切换至可用端口 {port}")
            break
        except OSError as exc:
            last_error = exc
            if existing_viewer(url):
                app.stop()
                print(f"Universal LiDAR Viewer is already running at {url}")
                if not args.no_browser and webbrowser is not None:
                    webbrowser.open(url)
                return 0

    if server is None:
        app.stop()
        raise last_error
    app.attach_shutdown(server.shutdown)
    app.start()

    stopped = threading.Event()
    license_failed = threading.Event()

    def stop_server(*_args: object) -> None:
        if not stopped.is_set():
            stopped.set()
            threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop_server)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, stop_server)

    def watch_license() -> None:
        while not stopped.wait(1):
            if not license_session.valid():
                license_failed.set()
                stop_server()
                return

    if license_session is not None:
        threading.Thread(target=watch_license, name="license-watch", daemon=True).start()

    print(f"Universal LiDAR Viewer {__version__}")
    print(f"Open {url}")
    mode = "automatic/all IPv4 interfaces" if auto_discovery else "manual"
    print(f"Listening for LiDAR UDP ({mode}) on {listen_ip}:{', '.join(map(str, udp_ports))}")
    if not args.no_browser and webbrowser is not None:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever(poll_interval=0.3)
    finally:
        stopped.set()
        app.stop()
        server.server_close()
    return 3 if license_failed.is_set() else 0


if __name__ == "__main__":
    raise SystemExit(main())
