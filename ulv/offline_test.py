"""Headless PCAP decoder regression used to verify a self-contained bundle."""

from __future__ import annotations

import json
from pathlib import Path
import struct
import tempfile
import time

from .capture import parse_ethernet_udp
from .detection import detect_packet, normalize_lidar_payload
from .native_plugins import NativePluginManager
from .registry import DeviceRegistry


PCAP_MAGICS = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1),
    b"\xa1\xb2\x3c\x4d": (">", 1),
}


def pcap_packets(path: Path):
    with path.open("rb") as source:
        header = source.read(24)
        if len(header) != 24 or header[:4] not in PCAP_MAGICS:
            raise ValueError("only classic Ethernet PCAP is supported")
        byte_order, fraction_ns = PCAP_MAGICS[header[:4]]
        if struct.unpack_from(f"{byte_order}I", header, 20)[0] != 1:
            raise ValueError("PCAP link type is not Ethernet")
        record = struct.Struct(f"{byte_order}IIII")
        while True:
            raw = source.read(record.size)
            if not raw:
                return
            if len(raw) != record.size:
                raise ValueError("truncated PCAP record header")
            seconds, fraction, captured_size, _wire_size = record.unpack(raw)
            frame = source.read(captured_size)
            if len(frame) != captured_size:
                raise ValueError("truncated PCAP frame")
            yield seconds * 1_000_000_000 + fraction * fraction_ns, frame


def test_pcap(path: Path, plugins: Path, corrections: Path, cache: Path, authorize=None) -> dict:
    registry = DeviceRegistry()
    output_frames = 0

    def emitted(_frame: bytes) -> None:
        nonlocal output_frames
        output_frames += 1

    manager = NativePluginManager(
        plugins,
        emitted,
        registry.update_model,
        calibration_cache_directory=cache,
        default_correction_directory=corrections / "hesai",
        authorize=authorize,
    )
    records = accepted = 0
    vendors: set[str] = set()
    error = ""
    try:
        for timestamp_ns, frame in pcap_packets(path):
            if authorize is not None and not authorize():
                raise ValueError("许可证已失效，停止离线解码")
            records += 1
            packet = parse_ethernet_udp(frame)
            if packet is None or not packet.payload:
                continue
            payload = normalize_lidar_payload(packet.payload)
            detection = detect_packet(payload, packet.destination_port)
            if detection is None:
                continue
            vendors.add(detection.vendor)
            device = registry.observe(
                packet.source_ip,
                packet.source_port,
                packet.destination_port,
                len(payload),
                detection,
                source_mac=packet.source_mac,
            )
            manager.feed(
                device,
                payload,
                timestamp_ns,
                packet.source_port,
                packet.destination_port,
            )
            accepted += 1
    except (OSError, ValueError) as exc:
        error = str(exc)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        status = manager.status()
        if all(plugin["packet_queue_depth"] == 0 for plugin in status["loaded"]):
            time.sleep(0.4)
            break
        time.sleep(0.05)
    status = manager.status()
    relevant = [plugin for plugin in status["loaded"] if plugin["vendor"] in vendors]
    frames = sum(int(plugin["frames"]) for plugin in relevant)
    nonzero = max((int(plugin["last_nonzero_points"]) for plugin in relevant), default=0)
    result = {
        "file": str(path),
        "records": records,
        "accepted_packets": accepted,
        "vendors": sorted(vendors),
        "frames": frames,
        "callback_frames": output_frames,
        "last_points": nonzero,
        "models": sorted({str(plugin["last_model"]) for plugin in relevant if plugin["last_model"]}),
        "decode_errors": sum(int(plugin["decode_errors"]) for plugin in relevant),
        "dropped_packets": sum(int(plugin["dropped_packets"]) for plugin in relevant),
        "plugin_errors": status["errors"],
        "error": error,
    }
    result["success"] = bool(accepted and frames and nonzero and not error and not status["errors"])
    manager.close()
    return result


def run_offline_test(path: Path, plugins: Path, corrections: Path, cache: Path | None = None, authorize=None) -> dict:
    pcaps = sorted(path.rglob("*.pcap")) if path.is_dir() else [path]
    if cache is not None:
        cache.mkdir(parents=True, exist_ok=True)
        results = [test_pcap(pcap, plugins, corrections, cache / pcap.stem, authorize) for pcap in pcaps]
    else:
        with tempfile.TemporaryDirectory(prefix="ulv-isolated-cache-") as temporary:
            root = Path(temporary)
            results = [test_pcap(pcap, plugins, corrections, root / pcap.stem, authorize) for pcap in pcaps]
    return {
        "mode": "offline-isolated-pcap-test",
        "input": str(path),
        "pcap_count": len(pcaps),
        "passed": sum(bool(result["success"]) for result in results),
        "failed": sum(not result["success"] for result in results),
        "success": bool(results) and all(result["success"] for result in results),
        "results": results,
    }


def write_report(report: dict, path: Path | None) -> None:
    output = json.dumps(report, ensure_ascii=False, indent=2)
    print(output)
    if path is not None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(output + "\n", encoding="utf-8")
