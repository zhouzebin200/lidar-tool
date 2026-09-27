"""Passive packet classification for supported vendor LiDAR streams.

The detector only identifies traffic that already reaches the host. Actual
point decoding remains the responsibility of a native vendor adapter.
"""

from __future__ import annotations

from dataclasses import dataclass


HESAI_PROTOCOLS: dict[tuple[int, int], str] = {
    (1, 4): "Pandar128 / OT128 / JT128",
    (1, 8): "JT16",
    (3, 1): "PandarQT",
    (3, 2): "QT128",
    (4, 1): "AT128",
    (4, 3): "AT128",
    (4, 7): "ATX",
    (6, 1): "PandarXT / XT32M",
    (7, 2): "FT120",
    (7, 3): "FTX",
}

HESAI_LEGACY_LENGTHS: dict[int, str] = {
    1262: "Pandar40",
    1270: "Pandar40",
    1194: "Pandar64",
    1198: "Pandar64",
}

ROBOSENSE_MSOP_LENGTHS: dict[int, str] = {
    1210: "RSM1",
    1342: "RSM2",
    1200: "RSE1",
    812: "RSEMX",
    1404: "RSMX",
    1448: "RSM3",
    62152: "RSM1_JUMBO",
}

ROBOSENSE_MSOP_V1 = bytes.fromhex("55 aa 05 0a 5a a5 50 a0")
ROBOSENSE_MSOP_MECH = bytes.fromhex("55 aa 05 5a")
ROBOSENSE_MSOP_MEMS = bytes.fromhex("55 aa 5a a5")
ROBOSENSE_MSOP_AIRYLITE = bytes.fromhex("5a ff 55 aa")
ROBOSENSE_DIFOP = bytes.fromhex("a5 ff 00 5a 11 11 55 55")
ROBOSENSE_DIFOP_PREFIX = ROBOSENSE_DIFOP[:4]
ROBOSENSE_DIFOP_REVERSED = ROBOSENSE_DIFOP[::-1]

SEYOND_DATA_MAGIC = 0x176A
SEYOND_STATUS_MAGIC = 0x186B
SEYOND_MODELS: dict[int, str] = {
    0: "Falcon-K1",
    1: "Robin-W",
    2: "Robin-E",
    3: "Falcon-K2",
    4: "Falcon-K24",
    5: "Robin-E1X / ELITE",
    6: "Robin-E2X",
    7: "HB-D1",
    8: "Robin-E2",
}


def normalize_lidar_payload(data: bytes) -> bytes:
    """Remove a validated LiDAR transport envelope before SDK decoding.

    RS-M2 can emit its normal 1342/256-byte SDK packets inside an 8-byte
    transport record on custom UDP ports. The record contains protocol 7,
    packet kind 0x0500/0x0501 and the inner length in network byte order.
    Some firmware writes the eight-byte DIFOP identifier in reverse order;
    normalize that identifier to the byte sequence expected by rs_driver.
    """
    if len(data) < 12:
        return data
    inner_size = int.from_bytes(data[4:8], "big")
    if inner_size != len(data) - 8:
        return data
    inner = data[8:]

    # Two RoboSense transports use the same validated length-at-offset-four
    # record framing. M2 prefixes protocol/kind (00 07 05 00/01); automotive
    # EMX prefixes a monotonically increasing 32-bit record sequence. EMX's
    # online packet is therefore 820 bytes: sequence + length 812 + MSOP.
    packet_kind = int.from_bytes(data[2:4], "big")
    typed_m2 = data[0:2] == b"\x00\x07" and packet_kind in (0x0500, 0x0501)
    known_inner = inner.startswith((
        ROBOSENSE_MSOP_V1,
        ROBOSENSE_MSOP_MECH,
        ROBOSENSE_MSOP_MEMS,
        ROBOSENSE_MSOP_AIRYLITE,
        ROBOSENSE_DIFOP_PREFIX,
        ROBOSENSE_DIFOP_REVERSED,
    ))
    if not typed_m2 and not known_inner:
        return data
    if inner.startswith((
        ROBOSENSE_MSOP_V1,
        ROBOSENSE_MSOP_MECH,
        ROBOSENSE_MSOP_MEMS,
        ROBOSENSE_MSOP_AIRYLITE,
    )):
        return inner
    if inner.startswith(ROBOSENSE_DIFOP_REVERSED):
        return ROBOSENSE_DIFOP + inner[len(ROBOSENSE_DIFOP):]
    if inner.startswith(ROBOSENSE_DIFOP_PREFIX):
        return inner
    return data

VELODYNE_PACKET_SIZE = 1206
VELODYNE_BANKS = {b"\xff\xee", b"\xff\xdd", b"\xff\xcc", b"\xff\xbb"}
VELODYNE_MODEL_IDS: dict[int, str] = {
    0x21: "HDL-32E",
    0x22: "VLP-16",
    0x23: "VLP-16 Puck LITE",
    0x24: "VLP-16 Hi-Res",
    0x28: "VLP-32C",
    0x40: "HDL-64E S3",
    0xA0: "VLS-128",
    0xA1: "VLS-128",
}


def _is_huawei_l306(data: bytes) -> bool:
    """Validate the capture-derived Huawei L306/D3 point packet."""
    return (
        len(data) == 1021
        and data[:4] == b"\x00\x00\x00\x00"
        and data[23] < 4
        and int.from_bytes(data[25:27], "big") < 1500
        and int.from_bytes(data[27:29], "big") < 1500
        and data[53:55] == b"\xff\xdd"
        and data[-4:] == b"\x00\x00\x00\x00"
    )


def _velodyne_model(data: bytes) -> tuple[str, float, str] | None:
    """Validate the twelve firing blocks and read the factory product ID."""
    if len(data) != VELODYNE_PACKET_SIZE:
        return None
    headers: list[bytes] = []
    for block in range(12):
        offset = block * 100
        header = data[offset:offset + 2]
        rotation = int.from_bytes(data[offset + 2:offset + 4], "little")
        if header not in VELODYNE_BANKS or rotation >= 36000:
            return None
        headers.append(header)
    product_id = data[1205]
    exact = VELODYNE_MODEL_IDS.get(product_id)
    if exact:
        return exact, 0.999, f"validated firing blocks and product ID 0x{product_id:02X}"
    if b"\xff\xcc" in headers or b"\xff\xbb" in headers:
        return "VLS-128", 0.98, "validated four-bank VLS-128 firing layout"
    if b"\xff\xdd" in headers:
        return "HDL-64E", 0.93, "validated upper/lower bank firing layout"
    return "Velodyne VLP/HDL 16/32 family", 0.90, "validated twelve Velodyne firing blocks"


def _robosense_ruby_model(data: bytes) -> str | None:
    """Identify Ruby channel count from its block layout and generation byte.

    Ruby and Ruby Plus share the same packet magic and packet length. The
    channel block stride is the reliable discriminator; header byte 30 then
    distinguishes the original (0x03) and Plus (0x04) generations.
    """
    if len(data) != 1248 or len(data) < 857 or data[80] != 0xFE:
        return None
    lidar_type = data[30]
    generation = {0x03: "RS", 0x04: "RSP"}.get(lidar_type)
    if data[468] == 0xFE and data[856] == 0xFE:
        return f"{generation}128" if generation else "RS128 / RSP128"
    if data[324] == 0xFE and data[568] == 0xFE and data[812] == 0xFE:
        return f"{generation}80" if generation else "RS80 / RSP80"
    return None


def _robosense_mechanical_model(data: bytes) -> tuple[str, str] | None:
    """Resolve newer mechanical models before generic candidate probing.

    Airy, Fairy, Helios and Ruby intentionally share the same four-byte MSOP
    identifier and 1248-byte packet size. Airy's SDK packet header exposes a
    dedicated lidar-type byte at offset 31; 0x31 is the value observed on the
    wire and consumed by the official RSAIRY layout.
    """
    ruby = _robosense_ruby_model(data)
    if ruby:
        return ruby, "matched Ruby block layout and generation"
    if len(data) == 1248 and len(data) > 42 and data[31] == 0x31 and data[42:44] == b"\xff\xee":
        channel_mode = {0x01: 48, 0x02: 96, 0x03: 192}.get(data[32])
        suffix = f" ({channel_mode}-channel mode)" if channel_mode else ""
        return "RSAIRY", f"matched Airy lidar-type 0x31{suffix}"
    return None


@dataclass(frozen=True, slots=True)
class Detection:
    vendor: str
    model: str
    confidence: float
    reason: str


def detect_packet(data: bytes, destination_port: int) -> Detection | None:
    """Return the best passive classification for a single UDP payload."""

    data = normalize_lidar_payload(data)

    if _is_huawei_l306(data):
        return Detection(
            "Huawei", "L306", 0.999,
            "validated L306/D3 192-channel packet layout",
        )

    # Seyond/Innovusion UDP payloads expose a packed InnoCommonHeader. Check
    # both the protocol magic and declared packet size so ordinary traffic
    # that happens to start with 6A 17 cannot be misclassified.
    if len(data) >= 26:
        magic = int.from_bytes(data[0:2], "little")
        declared_size = int.from_bytes(data[10:14], "little")
        major = data[2]
        lidar_type = data[15]
        if (
            magic in (SEYOND_DATA_MAGIC, SEYOND_STATUS_MAGIC)
            and declared_size == len(data)
            and 1 <= major <= 8
            and lidar_type in SEYOND_MODELS
        ):
            kind = "data" if magic == SEYOND_DATA_MAGIC else "status"
            return Detection(
                "Seyond",
                SEYOND_MODELS[lidar_type],
                0.995,
                f"validated Inno {kind} header v{major}.{data[3]}",
            )

    velodyne = _velodyne_model(data)
    if velodyne:
        model, confidence, reason = velodyne
        return Detection("Velodyne", model, confidence, reason)

    # Position packets carry GPS/NMEA and status rather than point samples.
    if destination_port == 8308 and len(data) == 512:
        return Detection("Velodyne", "model pending data packet", 0.72, "standard position packet")

    if len(data) >= 4 and data[0:2] == b"\xee\xff":
        major, minor = data[2], data[3]
        # AT128 functional-safety/status datagrams reuse the point protocol
        # prefix but are far shorter than an MSOP packet.
        if (major, minor) == (4, 3) and len(data) < 500:
            return None
        model = HESAI_PROTOCOLS.get((major, minor), f"Hesai protocol {major}.{minor}")
        confidence = 0.99 if (major, minor) in HESAI_PROTOCOLS else 0.86
        return Detection("Hesai", model, confidence, f"EEFF protocol {major}.{minor}")

    legacy_hesai = HESAI_LEGACY_LENGTHS.get(len(data))
    if legacy_hesai and destination_port == 2368:
        return Detection("Hesai", legacy_hesai, 0.82, f"legacy packet length {len(data)}")

    if data.startswith(ROBOSENSE_MSOP_V1):
        model = "RS16 / RS32 / RSBP"
        return Detection("RoboSense", model, 0.94, "matched 8-byte MSOP identifier")

    if data.startswith(ROBOSENSE_MSOP_MECH):
        resolved = _robosense_mechanical_model(data)
        if resolved:
            model, reason = resolved
            confidence = 0.999 if "/" not in model else 0.96
            return Detection("RoboSense", model, confidence, reason)
        model = "RS48 / RSP48" if len(data) == 1268 else "Ruby / Helios / AIRY / FAIRY family"
        return Detection("RoboSense", model, 0.91, "matched mechanical MSOP identifier")

    if data.startswith(ROBOSENSE_MSOP_MEMS):
        model = ROBOSENSE_MSOP_LENGTHS.get(len(data), "RoboSense MEMS family")
        return Detection("RoboSense", model, 0.93, "matched 4-byte MSOP identifier")

    if data.startswith(ROBOSENSE_MSOP_AIRYLITE) and len(data) == 1280:
        return Detection("RoboSense", "RSAIRYLITE_ETH", 0.98, "matched AIRYLITE MSOP signature")

    if data.startswith(ROBOSENSE_DIFOP_PREFIX):
        return Detection("RoboSense", "model pending MSOP", 0.91, "matched DIFOP identifier")

    if destination_port in (6699, 7788, 7766, 6688):
        return Detection("RoboSense", "unknown candidate", 0.45, "standard RoboSense port")
    if destination_port == 2368 and len(data) >= 500:
        return Detection("Hesai", "unknown candidate", 0.4, "standard Hesai port")
    if destination_port == 8010 and len(data) >= 54:
        return Detection("Seyond", "unknown candidate", 0.4, "standard Seyond UDP port")
    return None
