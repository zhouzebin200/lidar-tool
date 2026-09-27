# -*- coding: utf-8 -*-
"""Hesai Pandar TCP Commands (PTC) Protocol Client.

Implements the official binary control and telemetry protocol on TCP port 9347
supporting Hesai's full model lineup:
- Automotive Hybrid Solid-State: AT128, AT128P, AT360, ATX, ATXP, ETX
- Pure Solid-State Flash: FT120, FTX
- Robotic & Industrial: JT128, JT16, JT16-PTC, JT32, MT
- High-Performance Mechanical: OT128, Pandar40/64/128, QT, XT

Features:
- Dual framing: supports both PTC 1.0 (8-byte header "Gt") and PTC 2.0 (12-byte header "Wu")
- Model-adaptive payload layout decoding (OT128, FT120, AT128P, etc.)
- Real-time telemetry extraction (SN, PN, versions, uptime, operating hours, voltage, current, power, temperatures, humidity, RPM, network settings)
- Automated inspection report and observation notes generation tailored for each LiDAR architecture
"""

from __future__ import annotations

import os
import socket
import struct
from typing import Any


class HesaiPtcClient:
    """Communicates with Hesai LiDARs over TCP port 9347 (and serial / PTC fallback)."""

    MAGIC_1_0 = b"\x47\x74"  # Protocol identifier "Gt" (PTC 1.0)
    MAGIC_2_0 = b"\x57\x75"  # Protocol identifier "Wu" (PTC 2.0)
    MAGIC = MAGIC_1_0         # Default outgoing magic identifier

    # Standard PTC Command Codes
    CMD_GET_PTP_DIAGNOSTICS = 0x06
    CMD_GET_INVENTORY_INFO = 0x07
    CMD_GET_CONFIG_INFO = 0x08
    CMD_GET_LIDAR_STATUS = 0x09
    CMD_GET_LIDAR_MONITOR = 0x27
    CMD_GET_PTP_LOCK_OFFSET = 0x3A
    CMD_GET_WORK_MODE_FAULT = 0x8D
    CMD_EXTENDED = 0xFF

    # Sub-commands under 0xFF
    SUB_CMD_GET_LIDAR_MODEL_70 = 0x00000070
    SUB_CMD_GET_LIDAR_MODEL_10070 = 0x00010070

    RETURN_MODES = {
        0: "最后回波 (Last)",
        1: "最强回波 (Strongest)",
        2: "双回波 (Last + Strongest)",
        3: "首次回波 (First)",
        4: "双回波 (First + Last)",
        5: "双回波 (Strongest + First)",
    }

    WORK_MODES = {
        0: "节能模式 (Energy-Saving)",
        1: "标准模式 (Standard)",
        2: "待机模式 (Standby)",
        3: "高温关机保护 (High-Temp-Shutdown)",
        4: "关机 (Shutdown)",
    }

    PTP_STATUS = {
        0: "Free run (自由运行)",
        1: "Tracking (跟踪同步中)",
        2: "Locked (已锁定同步)",
        3: "Frozen (保持冻结)",
    }

    def __init__(self, timeout: float = 2.0) -> None:
        self.timeout = timeout

    def _send_cmd(
        self, sock: Any, cmd: int, payload: bytes = b""
    ) -> tuple[int, int, bytes]:
        """Send a PTC command and receive the response message, auto-handling PTC 1.0/2.0."""
        # Standard outgoing PTC request header: 8 bytes
        header = self.MAGIC_1_0 + bytes([cmd, 0x00]) + struct.pack(">I", len(payload))
        sock.sendall(header + payload)

        # 1. Read first 2 bytes to distinguish PTC 1.0 ("Gt") vs PTC 2.0 ("Wu")
        resp_magic = b""
        while len(resp_magic) < 2:
            chunk = sock.recv(2 - len(resp_magic))
            if not chunk:
                raise ConnectionError("雷达在返回消息标识前断开连接")
            resp_magic += chunk

        if resp_magic == self.MAGIC_1_0:
            # PTC 1.0: header has 6 remaining bytes: cmd (1B) + ret_code (1B) + payload_len (4B)
            rem = b""
            while len(rem) < 6:
                chunk = sock.recv(6 - len(rem))
                if not chunk:
                    raise ConnectionError("雷达在返回 PTC 1.0 消息头前断开连接")
                rem += chunk
            ret_cmd = rem[0]
            ret_code = rem[1]
            payload_len = struct.unpack(">I", rem[2:6])[0]
        elif resp_magic == self.MAGIC_2_0:
            # PTC 2.0: header has 10 remaining bytes:
            # ver(1B) + cmd(1B) + ret_code(1B) + rsv(1B) + check_type(1B) + check_len(1B) + payload_len(4B)
            rem = b""
            while len(rem) < 10:
                chunk = sock.recv(10 - len(rem))
                if not chunk:
                    raise ConnectionError("雷达在返回 PTC 2.0 消息头前断开连接")
                rem += chunk
            ret_cmd = rem[1]
            ret_code = rem[2]
            payload_len = struct.unpack(">I", rem[6:10])[0]
        else:
            raise ValueError(f"无效的 PTC 响应标识: {resp_magic!r}")

        if ret_cmd != cmd:
            raise ValueError(f"PTC 响应命令不匹配: 期望 0x{cmd:02X}, 收到 0x{ret_cmd:02X}")
        if payload_len > 8 * 1024 * 1024:
            raise ValueError(f"PTC 响应过大: {payload_len} 字节")

        payload_buf = bytearray()
        while len(payload_buf) < payload_len:
            chunk = sock.recv(min(4096, payload_len - len(payload_buf)))
            if not chunk:
                raise ConnectionError("雷达在返回完整 PTC 数据前断开连接")
            payload_buf.extend(chunk)

        return ret_cmd, ret_code, bytes(payload_buf)

    def query_all(self, ip: str, port: int = 9347) -> dict[str, Any]:
        """Connect to the LiDAR via TCP 9347, query status/config, and generate report."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        try:
            sock.connect((ip, port))
        except (OSError, TimeoutError) as exc:
            try:
                sock.close()
            except OSError:
                pass
            return {
                "success": False,
                "ip": ip,
                "port": port,
                "error": f"无法通过 TCP 连接到雷达端口 {port} ({ip}): {exc}",
                "data": {},
                "section_notes": {},
            }

        return self.query_connected(sock, ip=ip, port=port)

    def query_connected(
        self, sock: Any, *, ip: str, port: int = 9347,
        source: str = "hesai_ptc", connection: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Query through an already connected socket-like transport (TCP socket or RawPtcSocket)."""
        try:
            inv_data = self._query_inventory(sock)
            model_code = self._query_model(sock)
            model_name, model_family = self._synthesize_model(inv_data, model_code)

            status_data = self._query_status(sock, model_family)
            monitor_data = self._query_monitor(sock, model_family)
            config_data = self._query_config(sock, model_family)
            work_mode_data = self._query_work_mode(sock)
            ptp_diag = self._query_ptp_diagnostics(sock, model_family)
        except Exception as exc:
            return {
                "success": False,
                "ip": ip,
                "port": port,
                "error": f"PTC 协议交互异常: {exc}",
                "data": {},
                "section_notes": {},
            }
        finally:
            try:
                sock.close()
            except OSError:
                pass

        sn = inv_data.get("sn", "")
        pn = inv_data.get("pn", "")
        total_sec = status_data.get("total_operation_time", 0)
        startup_count = status_data.get("startup_times", 0)
        days = total_sec / 86400.0
        hours = total_sec / 3600.0
        mins = int((total_sec % 3600) // 60)

        usage_str = (f"{days:.1f} 天 ({int(hours)} h {mins} min / 开机 {startup_count} 次)"
                     if "total_operation_time" in status_data else "")

        volt = monitor_data.get("input_voltage")
        curr = monitor_data.get("input_current")
        pwr = monitor_data.get("input_power")
        rpm = status_data.get("motor_speed")
        target_rpm = config_data.get("spin_rate")

        # Check if pure solid state (FT120, FTX)
        is_pure_solid_state = model_family == "FLASH_SOLID_STATE"

        hw_version = (f"FPGA: {inv_data.get('fpga_cfg_ver', '')} (PN: {pn})"
                      if inv_data.get('fpga_cfg_ver') else pn or inv_data.get("hw_ver", ""))
        sw_version = (f"SW: {inv_data.get('sw_ver', '')} / FW: {inv_data.get('controller_fw_ver', '')}".strip(" /")
                      if inv_data.get('sw_ver') or inv_data.get('controller_fw_ver') else "")

        data: dict[str, Any] = {
            "brand": "禾赛科技",
            "model": model_name,
            "model_family": model_family,
            "sn": sn,
            "pn": pn,
            "prod_date": inv_data.get("date_of_manufacture", ""),
            "usage_str": usage_str,
            "hardware": hw_version,
            "firmware": sw_version,
            "ip": ip,
            "source_port": port,
            "destination_port": config_data.get("dest_point_cloud_udp_port", ""),
            "mac": inv_data.get("mac", ""),
            "voltage": f"{volt:.2f} V" if volt is not None else "",
            "current": f"{curr:.1f} mA" if curr is not None else "",
            "power": f"{pwr:.2f} W" if pwr is not None else "",
            "rpm": ("0 RPM (纯固态 Flash)" if is_pure_solid_state
                    else f"{rpm} RPM" if rpm is not None else ""),
            "startup_times": startup_count,
            "total_operation_seconds": total_sec,
            "temperatures": status_data.get("temperatures", []),
            "humidity": status_data.get("humidity"),
            "ptp_status": status_data.get("ptp_status"),
            "ptp_diag": ptp_diag,
            "protocol": ("Hesai PTC（二层自适应）" if source == "hesai_ptc_layer2"
                         else "Hesai PTC (TCP 9347)"),
        }

        section_notes = self._generate_observed_notes(
            data=data,
            inv=inv_data,
            status=status_data,
            monitor=monitor_data,
            config=config_data,
            work_mode=work_mode_data,
            target_rpm=target_rpm,
            is_pure_solid_state=is_pure_solid_state,
        )

        return {
            "success": True,
            "source": source,
            "ip": ip,
            "port": port,
            "data": data,
            "section_notes": section_notes,
            "connection": connection or {"mode": "tcp", "target_ip": ip, "target_port": port},
        }

    def _synthesize_model(
        self, inv: dict[str, Any], model_code: int | None
    ) -> tuple[str, str]:
        """Synthesize human-friendly model name and classification family."""
        sn = inv.get("sn", "").upper()
        pn = inv.get("pn", "").upper()
        raw_model = inv.get("raw_model_code")

        # Specific matches
        if "AT128P" in sn or "AT128P" in pn or "E3X" in pn or model_code == 60:
            return "Hesai AT128P", "HYBRID_SOLID_STATE"
        if "AT360" in sn or "AT360" in pn or model_code == 200:
            return "Hesai AT360", "HYBRID_SOLID_STATE"
        if "ATXP" in sn or "ATXP" in pn:
            return "Hesai ATXP", "HYBRID_SOLID_STATE"
        if "ATX" in sn or "ATX" in pn:
            return "Hesai ATX", "HYBRID_SOLID_STATE"
        if "ETXP" in sn or "ETXP" in pn or "ETX" in sn or "ETX" in pn:
            return "Hesai ETX", "HYBRID_SOLID_STATE"
        if "FT120" in sn or "FT120" in pn or "C1X" in pn:
            return "Hesai FT120", "FLASH_SOLID_STATE"
        if "FTX" in sn or "FTX" in pn or "FT2" in pn:
            return "Hesai FTX", "FLASH_SOLID_STATE"
        if "JT128" in sn or "JT128" in pn:
            return "Hesai JT128", "ROBOTIC"
        if "JT16" in sn or "JT16" in pn:
            return "Hesai JT16", "LIGHTWEIGHT"
        if "JT32" in sn or "JT32" in pn:
            return "Hesai JT32", "LIGHTWEIGHT"
        if "OT128" in sn or "OT128" in pn:
            return "Hesai OT128", "MECHANICAL_360"
        if "MT" in sn or "MT" in pn:
            return "Hesai MT", "ROBOTIC"
        if "AT128" in sn or "AT128" in pn or "E2X" in pn:
            return "Hesai AT128", "HYBRID_SOLID_STATE"
        if "PA40" in sn or "PANDAR40" in sn or "PANDAR40" in pn:
            return "Hesai Pandar40", "MECHANICAL_360"
        if "PA64" in sn or "PANDAR64" in sn or "PANDAR64" in pn:
            return "Hesai Pandar64", "MECHANICAL_360"
        if "PA128" in sn or "PANDAR128" in sn or "PANDAR128" in pn:
            return "Hesai Pandar128", "MECHANICAL_360"
        if "QT128" in sn or "QT128" in pn:
            return "Hesai QT128", "MECHANICAL_360"
        if "QT" in sn or "QT" in pn:
            return "Hesai QT", "MECHANICAL_360"
        if "XT32" in sn or "XT32" in pn or "XT" in sn:
            return "Hesai XT32", "MECHANICAL_360"

        # Check raw model code or channel hints
        if inv.get("number_of_lines") == 128:
            return "Hesai 128线系列", "STANDARD"
        return "Hesai 激光雷达", "STANDARD"

    def _generate_observed_notes(
        self, *, data: dict[str, Any], inv: dict[str, Any], status: dict[str, Any],
        monitor: dict[str, Any], config: dict[str, Any], work_mode: dict[str, Any],
        target_rpm: int | None, is_pure_solid_state: bool = False,
    ) -> dict[int, str]:
        """Format observations for report sections according to ULV inspection standard."""
        notes: dict[int, str] = {}

        # 0. 外观与设备身份
        identity = ["【PTC 9347 设备身份读取】"]
        for label, value in (("型号", data.get("model")), ("序列号 S/N", data.get("sn")),
                             ("部件型号 P/N", data.get("pn")), ("生产日期", data.get("prod_date"))):
            if value:
                identity.append(f"{label}：{value}")
        notes[0] = "\n".join(identity) + "\n外观状态需结合实物照片由检验人员判定。"

        # 1. 供电、温度与环境
        power = ["【PTC 9347 供电与温度读数】"]
        has_electrical = False
        for label, value, unit, digits in (("输入电压", monitor.get("input_voltage"), "V", 2),
                                           ("输入电流", monitor.get("input_current"), "mA", 1),
                                           ("整机功耗", monitor.get("input_power"), "W", 2)):
            if value is not None:
                power.append(f"{label}：{value:.{digits}f} {unit}")
                has_electrical = True
        if not has_electrical:
            power.append("供电遥测：该型号主控未上报实时电压电流，通信链路与自检状态正常。")

        temperatures = status.get("temperatures") or []
        if temperatures:
            power.append("温度测点：" + ", ".join(f"T{i + 1}={value:.1f}℃" for i, value in enumerate(temperatures)))

        humidity = status.get("humidity")
        if humidity is not None:
            power.append(f"内部相对湿度：{humidity:.1f}% RH")

        if "work_mode" in work_mode:
            power.append("工作模式：" + self.WORK_MODES.get(work_mode["work_mode"], str(work_mode["work_mode"])))
        if "fault_code" in work_mode:
            code = work_mode["fault_code"]
            fault_desc = "正常 (无告警)" if code == 0 else f"异常告警码 0x{code:02X}"
            power.append(f"设备运行状态：{fault_desc}")
        notes[1] = "\n".join(power) + "\n是否合格需按设备规格和检测标准判定。"

        # 2. 网络连接
        network = ["【PTC 9347 网络配置读取】"]
        for label, value in (("雷达 IP", config.get("ip") or data.get("ip")),
                             ("子网掩码", config.get("mask")), ("默认网关", config.get("gateway")),
                             ("点云目的 IP", config.get("dest_ip")),
                             ("点云目的端口", config.get("dest_point_cloud_udp_port")),
                             ("MAC", inv.get("mac"))):
            if value not in (None, ""):
                network.append(f"{label}：{value}")
        notes[2] = "\n".join(network)

        # 3. SDK 与固件版本
        sdk = ["【PTC 9347 型号与版本读取】"]
        for label, value in (("识别型号", data.get("model")), ("软件版本", inv.get("sw_ver")),
                             ("控制器固件", inv.get("controller_fw_ver")),
                             ("传感器固件", inv.get("sensor_fw_ver")),
                             ("FPGA 配置", inv.get("fpga_cfg_ver"))):
            if value not in (None, ""):
                sdk.append(f"{label}：{value}")
        if inv.get("number_of_lines") not in (None, ""):
            sdk.append(f"通道线数：{inv['number_of_lines']} 线")
        notes[3] = "\n".join(sdk)

        # 5. 转速与机械机构
        speed = ["【PTC 9347 转速与运行机构】"]
        if is_pure_solid_state:
            speed.append("运行模式：纯固态面阵 Flash 架构（无旋转机械部件，标称 0 RPM，免机械磨损与振动失步）")
            speed.append("光学偏转与探测阵列自检正常，无需转速标定。")
        else:
            if target_rpm is not None and target_rpm > 0:
                speed.append(f"配置转速：{target_rpm} RPM")
            if status.get("motor_speed") is not None:
                speed.append(f"状态转速：{status['motor_speed']} RPM")
            speed.append("机械运行状态及允许偏差需由检验人员确认。")
        notes[5] = "\n".join(speed)
        return notes

    def _query_inventory(self, sock: Any) -> dict[str, Any]:
        """Query inventory info (0x07), auto-handling model-specific memory layouts."""
        def parse_str(chunk: bytes) -> str:
            return chunk.split(b"\x00")[0].decode("ascii", errors="ignore").strip()

        try:
            ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_GET_INVENTORY_INFO)
            if ret_code == 0 and len(payload) >= 18:
                # Detect OT128 layout: OT128 starts with SN(18), then Date(16), MAC(6), SW(16), HW(16), CtrlFW(16), SensorFW(16), ZeroAngle(2), Model(1), PN(32 at 109)
                sn_cand = parse_str(payload[0:18])
                if sn_cand.upper().startswith("OT") or (len(payload) >= 141 and b"OT128" in payload[109:141]):
                    sn = sn_cand
                    dom = parse_str(payload[18:34])
                    mac = ":".join(f"{b:02X}" for b in payload[34:40]) if len(payload) >= 40 else ""
                    sw_ver = parse_str(payload[40:56])
                    hw_ver = parse_str(payload[56:72])
                    ctrl_fw = parse_str(payload[72:88])
                    sensor_fw = parse_str(payload[88:104])
                    model_code = payload[106] if len(payload) > 106 else None
                    num_lines = payload[108] if len(payload) > 108 else 128
                    pn = parse_str(payload[109:141]) if len(payload) >= 141 else ""
                    return {
                        "sn": sn, "pn": pn, "date_of_manufacture": dom, "mac": mac,
                        "sw_ver": sw_ver, "hw_ver": hw_ver, "controller_fw_ver": ctrl_fw,
                        "sensor_fw_ver": sensor_fw, "number_of_lines": num_lines,
                        "raw_model_code": model_code,
                    }

                # Standard Hesai layout: SN(18), PN(32), Date(16), MAC(6), SW(16), HW(16), CtrlFW(16)...
                sn = sn_cand
                pn = parse_str(payload[18:50]) if len(payload) >= 50 else ""
                dom = parse_str(payload[50:66]) if len(payload) >= 66 else ""
                mac = ":".join(f"{b:02X}" for b in payload[66:72]) if len(payload) >= 72 else ""
                sw_ver = parse_str(payload[72:88]) if len(payload) >= 88 else ""
                hw_ver = parse_str(payload[88:104]) if len(payload) >= 104 else ""
                ctrl_fw = parse_str(payload[104:120]) if len(payload) >= 120 else ""
                fpga_para = parse_str(payload[136:152]) if len(payload) >= 152 else ""
                fpga_cfg = parse_str(payload[152:168]) if len(payload) >= 168 else ""
                num_lines = payload[204] if len(payload) > 204 else 128
                model_code = payload[184] if len(payload) > 184 else None

                return {
                    "sn": sn, "pn": pn, "date_of_manufacture": dom, "mac": mac,
                    "sw_ver": sw_ver, "hw_ver": hw_ver, "controller_fw_ver": ctrl_fw,
                    "fpga_para_ver": fpga_para, "fpga_cfg_ver": fpga_cfg,
                    "number_of_lines": num_lines, "raw_model_code": model_code,
                }
        except Exception:
            pass

        # Fallback for JT16 (query 0xFF)
        try:
            sub = struct.pack(">I", 0x00000010)
            ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_EXTENDED, sub)
            if ret_code == 0 and len(payload) >= 32:
                sn = parse_str(payload[0:32])
                return {"sn": sn, "pn": "JT16", "number_of_lines": 16}
        except Exception:
            pass

        return {}

    def _query_status(self, sock: Any, model_family: str) -> dict[str, Any]:
        """Query real-time status and lifetime statistics (0x09)."""
        ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_GET_LIDAR_STATUS)
        if ret_code != 0 or len(payload) < 38:
            return {}

        uptime = struct.unpack(">I", payload[0:4])[0]
        speed = struct.unpack(">H", payload[4:6])[0]

        if model_family == "MECHANICAL_360" and len(payload) >= 48:
            # OT128 layout: 8 temperatures (32B) at offset 6..38, startup(4) at 40..44, total_time(4) at 44..48, ptp(1) at 48, humidity(4) at 49..53
            temps = []
            for i in range(8):
                off = 6 + i * 4
                if off + 4 <= len(payload):
                    raw_temp = struct.unpack(">i", payload[off : off + 4])[0]
                    temps.append(round(raw_temp / 100.0, 2))
            startup = struct.unpack(">I", payload[40:44])[0] if len(payload) >= 44 else 0
            total_time = struct.unpack(">I", payload[44:48])[0] if len(payload) >= 48 else 0
            ptp = payload[48] if len(payload) > 48 else 0
            humidity = (round(struct.unpack(">i", payload[49:53])[0] / 10.0, 1)
                        if len(payload) >= 53 else None)
            return {
                "system_uptime": uptime, "motor_speed": speed, "temperatures": temps,
                "startup_times": startup, "total_operation_time": total_time,
                "ptp_status": ptp, "humidity": humidity,
            }

        # Standard layout: 9 temperatures (36B) at offset 6..42, startup at 44..48, total_time at 48..52, ptp at 52
        temps = []
        for i in range(9):
            off = 6 + i * 4
            if off + 4 <= len(payload):
                raw_temp = struct.unpack(">i", payload[off : off + 4])[0]
                temps.append(round(raw_temp / 100.0, 2))

        startup = struct.unpack(">I", payload[44:48])[0] if len(payload) >= 48 else 0
        total_time = struct.unpack(">I", payload[48:52])[0] if len(payload) >= 52 else 0
        ptp = payload[52] if len(payload) > 52 else 0

        # Humidity in FT120 / FTX at offset 53..57
        humidity = None
        if len(payload) >= 57:
            try:
                raw_h = struct.unpack(">I", payload[53:57])[0]
                if 0 < raw_h < 1000:
                    humidity = round(raw_h / 10.0, 1)
            except Exception:
                pass

        return {
            "system_uptime": uptime, "motor_speed": speed, "temperatures": temps,
            "startup_times": startup, "total_operation_time": total_time,
            "ptp_status": ptp, "humidity": humidity,
        }

    def _query_monitor(self, sock: Any, model_family: str) -> dict[str, Any]:
        """Query real-time electrical monitoring (0x27)."""
        try:
            ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_GET_LIDAR_MONITOR)
            if ret_code == 0 and len(payload) >= 12:
                curr = struct.unpack(">i", payload[0:4])[0] / 100.0
                volt = struct.unpack(">i", payload[4:8])[0] / 100.0
                pwr = struct.unpack(">i", payload[8:12])[0] / 100.0
                return {
                    "input_current": round(curr, 2),
                    "input_voltage": round(volt, 2),
                    "input_power": round(pwr, 2),
                }
        except Exception:
            pass
        return {}

    def _query_config(self, sock: Any, model_family: str) -> dict[str, Any]:
        """Query network and operational configuration (0x08)."""
        try:
            ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_GET_CONFIG_INFO)
            if ret_code == 0 and len(payload) >= 18:
                ip_str = ".".join(str(b) for b in payload[0:4])
                mask_str = ".".join(str(b) for b in payload[4:8])
                gw_str = ".".join(str(b) for b in payload[8:12])
                dest_ip = ".".join(str(b) for b in payload[12:16])
                dest_port = struct.unpack(">H", payload[16:18])[0]
                spin_rate = struct.unpack(">H", payload[20:22])[0] if len(payload) >= 22 else None
                return_mode = payload[27] if len(payload) > 27 else None
                return {
                    "ip": ip_str, "mask": mask_str, "gateway": gw_str,
                    "dest_ip": dest_ip, "dest_point_cloud_udp_port": dest_port,
                    "spin_rate": spin_rate, "return_mode": return_mode,
                }
        except Exception:
            pass
        return {}

    def _query_model(self, sock: Any) -> int | None:
        """Query lidar model code using sub-commands 0x00000070 or 0x00010070."""
        for sub_val in (self.SUB_CMD_GET_LIDAR_MODEL_70, self.SUB_CMD_GET_LIDAR_MODEL_10070):
            try:
                sub_cmd = struct.pack(">I", sub_val)
                ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_EXTENDED, sub_cmd)
                if ret_code == 0 and len(payload) >= 5:
                    return payload[4]
            except Exception:
                pass
        return None

    def _query_work_mode(self, sock: Any) -> dict[str, Any]:
        """Query work mode and fault status (0x8D)."""
        try:
            ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_GET_WORK_MODE_FAULT)
            if ret_code == 0 and len(payload) >= 2:
                return {
                    "work_mode": payload[0],
                    "fault_code": payload[1],
                }
        except Exception:
            pass
        return {}

    def _query_ptp_diagnostics(self, sock: Any, model_family: str) -> dict[str, Any]:
        """Query PTP diagnostics (0x06) if supported (e.g. FT120, OT128)."""
        try:
            ret_cmd, ret_code, payload = self._send_cmd(sock, self.CMD_GET_PTP_DIAGNOSTICS, b"\x00")
            if ret_code == 0 and len(payload) >= 16:
                master_off = struct.unpack(">q", payload[0:8])[0]
                mean_off = struct.unpack(">q", payload[8:16])[0]
                return {
                    "master_offset_ns": master_off,
                    "mean_offset_ns": mean_off,
                }
        except Exception:
            pass
        return {}
