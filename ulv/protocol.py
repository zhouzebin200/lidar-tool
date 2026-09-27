"""Compact binary protocol shared by the backend and browser renderer."""

from __future__ import annotations

from dataclasses import dataclass
import struct
import time
from typing import Iterable


MAGIC = b"ULV1"
VERSION = 1
HEADER = struct.Struct("<4sHHIIIQ")
POINT = struct.Struct("<ffffHBB")


@dataclass(frozen=True, slots=True)
class Point:
    x: float
    y: float
    z: float
    intensity: float
    ring: int = 0
    return_type: int = 0
    flags: int = 0


def encode_frame(
    device_id: int,
    sequence: int,
    points: Iterable[Point],
    timestamp_ns: int | None = None,
) -> bytes:
    """Encode one complete render frame.

    Frames are intentionally simple: a fixed 24-byte header followed by fixed
    20-byte points. This lets JavaScript upload attributes to the GPU without
    JSON conversion or per-point object allocation.
    """

    point_list = list(points)
    timestamp_ns = time.time_ns() if timestamp_ns is None else timestamp_ns
    payload = bytearray(HEADER.size + POINT.size * len(point_list))
    HEADER.pack_into(
        payload,
        0,
        MAGIC,
        VERSION,
        0,
        device_id,
        sequence,
        len(point_list),
        timestamp_ns,
    )
    offset = HEADER.size
    for point in point_list:
        POINT.pack_into(
            payload,
            offset,
            point.x,
            point.y,
            point.z,
            point.intensity,
            point.ring,
            point.return_type,
            point.flags,
        )
        offset += POINT.size
    return bytes(payload)


def decode_header(frame: bytes) -> tuple[int, int, int, int]:
    if len(frame) < HEADER.size:
        raise ValueError("frame is shorter than the ULV header")
    magic, version, _flags, device_id, sequence, point_count, timestamp_ns = HEADER.unpack_from(frame)
    if magic != MAGIC:
        raise ValueError("invalid ULV frame magic")
    if version != VERSION:
        raise ValueError(f"unsupported ULV protocol version: {version}")
    expected = HEADER.size + point_count * POINT.size
    if len(frame) != expected:
        raise ValueError(f"invalid ULV frame size: expected {expected}, got {len(frame)}")
    return device_id, sequence, point_count, timestamp_ns
