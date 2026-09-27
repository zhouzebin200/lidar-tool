"""Replay a classic Ethernet PCAP through ULV's real vendor decoder plugins."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from ulv.capture import parse_ethernet_udp
from ulv.detection import detect_packet
from ulv.native_plugins import NativePluginManager
from ulv.protocol import HEADER, POINT
from ulv.registry import DeviceRegistry


PCAP_MAGICS = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1),
    b"\xa1\xb2\x3c\x4d": (">", 1),
}


def packets(path: Path):
    with path.open("rb") as source:
        header = source.read(24)
        if len(header) != 24 or header[:4] not in PCAP_MAGICS:
            raise ValueError("only classic PCAP files are supported by this validator")
        byte_order, fraction_ns = PCAP_MAGICS[header[:4]]
        link_type = struct.unpack_from(f"{byte_order}I", header, 20)[0]
        if link_type != 1:
            raise ValueError(f"expected Ethernet link type 1, got {link_type}")
        record = struct.Struct(f"{byte_order}IIII")
        while True:
            raw_record = source.read(record.size)
            if not raw_record:
                return
            if len(raw_record) != record.size:
                raise ValueError("truncated PCAP record header")
            seconds, fraction, captured_size, _wire_size = record.unpack(raw_record)
            frame = source.read(captured_size)
            if len(frame) != captured_size:
                raise ValueError("truncated PCAP frame")
            yield seconds * 1_000_000_000 + fraction * fraction_ns, frame


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pcap", type=Path)
    parser.add_argument("--plugins", type=Path, default=ROOT / "plugins")
    parser.add_argument("--snapshot", type=Path, help="write top/front/side diagnostic PNG")
    parser.add_argument("--correction", type=Path, help="Hesai angle correction file")
    args = parser.parse_args()

    registry = DeviceRegistry()
    decoded_frames: list[bytes] = []
    manager = NativePluginManager(
        args.plugins,
        lambda frame: decoded_frames.__setitem__(slice(None), [frame]),
        registry.update_model,
        correction_file=args.correction,
        default_correction_directory=ROOT / "corrections" / "hesai",
    )
    records = accepted = 0
    first_timestamp_ns = last_timestamp_ns = None
    for timestamp_ns, frame in packets(args.pcap):
        if first_timestamp_ns is None:
            first_timestamp_ns = timestamp_ns
        last_timestamp_ns = timestamp_ns
        records += 1
        packet = parse_ethernet_udp(frame)
        if packet is None or not packet.payload:
            continue
        detection = detect_packet(packet.payload, packet.destination_port)
        if detection is None:
            continue
        device = registry.observe(
            packet.source_ip,
            packet.source_port,
            packet.destination_port,
            len(packet.payload),
            detection,
            source_mac=packet.source_mac,
        )
        manager.feed(
            device,
            packet.payload,
            timestamp_ns,
            packet.source_port,
            packet.destination_port,
        )
        accepted += 1

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        status = manager.status()
        if all(plugin["packet_queue_depth"] == 0 for plugin in status["loaded"]):
            time.sleep(0.25)
            break
        time.sleep(0.05)
    frame_analysis = None
    snapshot_points: list[tuple[float, float, float, int]] = []
    if decoded_frames:
        frame = decoded_frames[-1]
        point_count = HEADER.unpack_from(frame)[5]
        elevations: dict[int, list[float]] = {}
        for index in range(point_count):
            x, y, z, _intensity, ring, _return_type, _flags = POINT.unpack_from(
                frame, HEADER.size + index * POINT.size
            )
            snapshot_points.append((x, y, z, ring))
            radius = math.hypot(x, y)
            if radius > 1e-6:
                elevations.setdefault(ring, []).append(math.degrees(math.atan2(z, radius)))
        ring_medians = {
            str(ring): round(statistics.median(values), 4)
            for ring, values in sorted(elevations.items()) if values
        }
        frame_analysis = {
            "rings_with_points": len(ring_medians),
            "elevation_median_min": min(ring_medians.values()) if ring_medians else None,
            "elevation_median_max": max(ring_medians.values()) if ring_medians else None,
            "ring_elevation_medians": ring_medians,
        }
    if args.snapshot and snapshot_points:
        import matplotlib.pyplot as plt
        import numpy as np

        sampled = np.asarray(snapshot_points[::max(1, len(snapshot_points) // 30000)])
        figure, axes = plt.subplots(1, 3, figsize=(15, 5), facecolor="#090c10")
        views = ((0, 1, "Top · XY", "X (m)", "Y (m)"),
                 (0, 2, "Front · XZ", "X (m)", "Z (m)"),
                 (1, 2, "Side · YZ", "Y (m)", "Z (m)"))
        for axis, (a, b, title, xlabel, ylabel) in zip(axes, views):
            axis.set_facecolor("#090c10")
            axis.scatter(sampled[:, a], sampled[:, b], c=sampled[:, 3], s=.25,
                         cmap="turbo", alpha=.75, linewidths=0)
            axis.set_title(title, color="white")
            axis.set_xlabel(xlabel, color="#aebbc5")
            axis.set_ylabel(ylabel, color="#aebbc5")
            axis.tick_params(colors="#73818d")
            axis.grid(color="#25313a", linewidth=.35)
            axis.set_aspect("equal", adjustable="box")
        figure.suptitle("ULV vendor SDK output · last decoded frame", color="white")
        figure.tight_layout()
        args.snapshot.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(args.snapshot, dpi=160, facecolor=figure.get_facecolor())
        plt.close(figure)
    capture_duration = (
        (last_timestamp_ns - first_timestamp_ns) / 1_000_000_000
        if first_timestamp_ns is not None and last_timestamp_ns is not None
        else 0
    )
    hesai_frames = next(
        (plugin["frames"] for plugin in manager.status()["loaded"] if plugin["vendor"] == "Hesai"),
        0,
    )
    result = {
        "pcap": str(args.pcap),
        "records": records,
        "capture_duration_seconds": round(capture_duration, 6),
        "hesai_output_fps": round(hesai_frames / capture_duration, 3) if capture_duration else 0,
        "accepted_lidar_packets": accepted,
        "devices": registry.list_public(),
        "native_plugins": manager.status(),
        "frame_analysis": frame_analysis,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    manager.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
