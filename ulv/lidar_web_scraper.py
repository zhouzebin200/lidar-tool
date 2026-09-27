"""LiDAR embedded web management interface scraper and data parser.

Probes the LiDAR's built-in HTTP server, crawls status/info/config subpages,
and extracts key parameters (S/N, model, firmware, voltage, current, RPM,
temperatures, network settings, and diagnostics) for test reports and assessments.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import html
import json
import re
import time
from typing import Any
from urllib.error import URLError
from urllib.parse import urljoin, urlparse
from urllib.request import Request, urlopen

from ulv.hesai_ptc import HesaiPtcClient


HESAI_CGI_PATHS = (
    "/pandar.cgi?action=get&object=device_info",
    "/pandar.cgi?action=get&object=lidar_config",
    "/pandar.cgi?action=get&object=ethernet_all",
    "/pandar.cgi?action=get&object=TimeStatistic",
)

COMMON_PROBE_PATHS = (
    "/",
    "/index.html",
    "/status.json",
    "/data.json",
    "/get_lidar_status",
    "/status.html",
    "/settings.html",
    "/cgi/info.json",
    "/cgi/status.json",
    "/cgi/diag.json",
    "/api/status",
    "/api/device_info",
) + HESAI_CGI_PATHS


class LidarWebScraper:
    """Scrapes and parses LiDAR embedded web configuration interfaces."""

    def __init__(self, timeout: float = 2.0, max_workers: int = 4) -> None:
        self.timeout = timeout
        self.max_workers = max_workers

    def scrape(self, target: str, port: int = 80) -> dict[str, Any]:
        """Probe the target LiDAR via Hesai PTC (TCP 9347) or HTTP web management interface."""
        clean_target = target.strip()
        target_port = port
        if "://" in clean_target:
            parsed_raw = urlparse(clean_target)
            ip = parsed_raw.hostname or clean_target
            if parsed_raw.port:
                target_port = parsed_raw.port
        elif ":" in clean_target and not clean_target.startswith("["):
            parts = clean_target.split(":")
            ip = parts[0].strip()
            if parts[1].isdigit():
                target_port = int(parts[1])
        else:
            ip = clean_target

        # An explicitly selected control port uses PTC. The default report path
        # must prefer the device's HTTP API: Pandar40P also answers PTC 9347,
        # but its payload layout is not the AT128 layout used by HesaiPtcClient.
        ptc_timeout = max(self.timeout, 2.0)
        if target_port != 80:
            ptc_client = HesaiPtcClient(timeout=ptc_timeout)
            ptc_res = ptc_client.query_all(ip, port=target_port)
            if ptc_res.get("success"):
                return ptc_res

        base_url = self._normalize_base_url(target, port)
        parsed = urlparse(base_url)
        ip = parsed.hostname or target

        # Probe the primary index page or device_info first
        primary_content, primary_is_json = self._fetch(base_url)

        # Also probe Hesai device_info directly in case root redirects or takes longer
        hesai_info_url = urljoin(base_url, "/pandar.cgi?action=get&object=device_info")
        hesai_content, _ = self._fetch(hesai_info_url)

        if primary_content is None and hesai_content is None:
            if target_port == 80:
                ptc_res = HesaiPtcClient(timeout=min(self.timeout, 1.5)).query_all(ip, port=9347)
                if ptc_res.get("success"):
                    return ptc_res
            return {
                "success": False,
                "ip": ip,
                "base_url": base_url,
                "error": f"无法连接到雷达 ({ip})：未在端口 9347 发现 Hesai PTC 协议响应，且 HTTP 网页管理端 ({base_url}) 连接超时",
                "data": {},
                "section_notes": {},
            }

        fetched: dict[str, str | dict] = {}
        if primary_content is not None:
            fetched[base_url] = primary_content
        if hesai_content is not None:
            fetched[hesai_info_url] = hesai_content

        # Check if target is Hesai
        is_hesai = (
            hesai_content is not None
            or (isinstance(primary_content, str) and "pandar" in primary_content.lower())
        )

        candidate_paths: list[str] = []
        if is_hesai:
            candidate_paths.extend(HESAI_CGI_PATHS)
        else:
            candidate_paths.extend(COMMON_PROBE_PATHS)
            if isinstance(primary_content, str):
                candidate_paths.extend(self._discover_subpages(base_url, primary_content))

        unique_paths = list(dict.fromkeys(candidate_paths))
        urls_to_fetch = [
            urljoin(base_url, path)
            for path in unique_paths
            if urljoin(base_url, path) not in fetched
        ]

        # Fetch candidate pages concurrently
        with ThreadPoolExecutor(max_workers=self.max_workers) as executor:
            future_to_url = {
                executor.submit(self._fetch, url): url
                for url in urls_to_fetch[:10]
            }
            for future in as_completed(future_to_url):
                url = future_to_url[future]
                try:
                    content, _ = future.result()
                    if content is not None:
                        fetched[url] = content
                except Exception:
                    pass

        extracted = self.extract_fields(fetched, default_ip=ip)
        notes = self.generate_section_notes(extracted, ip)

        return {
            "success": True,
            "source": "lidar_web",
            "ip": ip,
            "base_url": base_url,
            "data": extracted,
            "section_notes": notes,
            "fetched_urls": list(fetched.keys()),
        }

    @staticmethod
    def _normalize_base_url(target: str, default_port: int = 80) -> str:
        target = target.strip()
        if not target.startswith(("http://", "https://")):
            if ":" in target and not target.endswith("]"):
                parts = target.split(":")
                target = f"http://{parts[0]}:{parts[1]}"
            else:
                target = f"http://{target}" if default_port == 80 else f"http://{target}:{default_port}"
        if not target.endswith("/"):
            target += "/"
        return target

    def _fetch(self, url: str) -> tuple[str | dict | None, bool]:
        req = Request(
            url,
            headers={
                "User-Agent": "UniversalLiDARViewer/1.0 (Embedded LiDAR Inspection Agent)",
                "Accept": "application/json, text/html, */*",
            },
        )
        try:
            with urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read(512 * 1024)
                content_type = resp.headers.get("Content-Type", "").lower()
                text = raw.decode("utf-8", errors="replace")
                if "json" in content_type or text.strip().startswith(("{", "[")):
                    try:
                        return json.loads(text), True
                    except json.JSONDecodeError:
                        pass
                return text, False
        except (URLError, OSError, TimeoutError):
            return None, False

    @staticmethod
    def _discover_subpages(base_url: str, html_text: str) -> list[str]:
        found: list[str] = []
        patterns = (
            r'href=["\']([^"\']+\.(?:html|htm|php|json))["\']',
            r'url:\s*["\']([^"\']+)["\']',
            r'fetch\(["\']([^"\']+)["\']',
            r'\.get\(["\']([^"\']+)["\']',
            r'src=["\']([^"\']+\.js)["\']',
        )
        for pattern in patterns:
            for match in re.finditer(pattern, html_text, re.IGNORECASE):
                path = match.group(1)
                if any(k in path.lower() for k in ("status", "info", "setting", "config", "diag", "device", "net")):
                    found.append(path)
        return found

    def extract_fields(self, pages: dict[str, str | dict], default_ip: str = "") -> dict[str, Any]:
        """Parse structured and unstructured data across all retrieved pages."""
        extracted: dict[str, Any] = {
            "brand": "",
            "model": "",
            "laser_num": "",
            "sn": "",
            "prod_date": "",
            "firmware": "",
            "hardware": "",
            "voltage": "",
            "current": "",
            "power": "",
            "temperature": "",
            "rpm": "",
            "ip": default_ip,
            "target_ip": "",
            "target_port": "",
            "subnet_mask": "",
            "gateway": "",
            "mac": "",
            "vlan": "",
            "startup_times": "",
            "total_operation_time": "",
            "system_uptime": "",
            "working_hours": "",
            "ptp_status": "",
            "diagnostics": "",
        }

        # 1. Specialized Hesai parser if Hesai responses exist
        self._extract_hesai(pages, extracted)

        # 2. General JSON parser for other fields
        for url, content in pages.items():
            if isinstance(content, dict):
                self._extract_from_json(content, extracted)

        # 3. HTML / text regex parser fallback
        for url, content in pages.items():
            if isinstance(content, str):
                self._extract_from_html(content, extracted)

        # Post-processing / defaults
        op_time = extracted.get("total_operation_time") or extracted.get("working_hours") or ""
        st_times = extracted.get("startup_times") or ""
        parts: list[str] = []
        if st_times:
            parts.append(f"开机 {st_times} 次")
        if op_time:
            parts.append(f"累计时长 {op_time}")
        extracted["usage_str"] = " / ".join(parts)

        return extracted

    def _extract_hesai(self, pages: dict[str, str | dict], out: dict[str, Any]) -> None:
        """Extract data from Hesai Pandar cgi responses."""
        hesai_detected = False
        for url, content in pages.items():
            if not isinstance(content, dict):
                continue
            body = content.get("Body")
            if not isinstance(body, dict):
                continue

            # Check for device_info
            if "SN" in body and ("ProdName" in body or "SW_Ver" in body):
                hesai_detected = True
                out["brand"] = "禾赛 (Hesai)"
                out["sn"] = str(body.get("SN", "")).strip()
                out["prod_date"] = str(body.get("ProdDate", "")).strip()
                out["mac"] = str(body.get("Mac", "")).strip()
                out["laser_num"] = str(body.get("LaserNum", "")).strip()

                prod_name = str(body.get("ProdName", "")).strip()
                model_raw = str(body.get("Model", "")).strip()
                out["model"] = prod_name or model_raw

                fw = str(body.get("FW_Ver", "")).strip()
                sw = str(body.get("SW_Ver", "")).strip()
                if fw and sw:
                    out["firmware"] = f"FW {fw} / SW {sw}"
                elif fw or sw:
                    out["firmware"] = fw or sw

                hw = str(body.get("HW_Ver", "")).strip()
                up_fpga = str(body.get("Up_Fpga_Ver", "")).strip()
                if hw and up_fpga:
                    out["hardware"] = f"HW {hw} (FPGA {up_fpga})"
                elif hw:
                    out["hardware"] = hw

            # Check for TimeStatistic
            if "StartupTimes" in body or "CurrentTemp" in body or "TotalWorkingTime" in body or "SystemUptime" in body:
                hesai_detected = True
                out["startup_times"] = str(body.get("StartupTimes", "")).strip()

                raw_time = str(body.get("TotalWorkingTime", "")).strip()
                if raw_time.isdigit():
                    # Hesai firmware stores TotalWorkingTime in minutes.
                    # The radar web management page displays:
                    # Math.floor(TotalWorkingTime / 60) + ' h ' + (TotalWorkingTime % 60) + ' min'
                    total_min = int(raw_time)
                    h = total_min // 60
                    m = total_min % 60
                    days_val = total_min / 1440.0
                    days_str = f"{days_val:.1f}" if days_val < 10 else f"{days_val:.1f}".rstrip("0").rstrip(".")
                    days_formatted = f"{days_str} 天"
                    out["total_operation_time"] = f"{days_formatted} ({h} h {m} min)"
                    out["working_hours"] = f"{days_formatted} (约 {h} 小时 {m} 分钟 / {h} h {m} min)"
                elif raw_time:
                    out["total_operation_time"] = raw_time
                    out["working_hours"] = raw_time

                raw_uptime = str(body.get("SystemUptime", "")).strip()
                if raw_uptime.isdigit():
                    up_min = int(raw_uptime)
                    uh = up_min // 60
                    um = up_min % 60
                    out["system_uptime"] = f"{uh} h {um} min"
                elif raw_uptime:
                    out["system_uptime"] = raw_uptime

                raw_temp = str(body.get("CurrentTemp", "")).strip()
                if raw_temp:
                    out["temperature"] = f"{raw_temp} ℃"

            # Check for lidar_config
            if "SpinSpeed" in body or "DestIp" in body:
                hesai_detected = True
                speed_code = str(body.get("SpinSpeed", "")).strip()
                speed_map = {
                    "1": "300 RPM (5 Hz)",
                    "2": "600 RPM (10 Hz)",
                    "3": "1200 RPM (20 Hz)",
                }
                out["rpm"] = speed_map.get(speed_code, f"{speed_code} (Code)") if speed_code else ""
                out["target_ip"] = str(body.get("DestIp", "")).strip()
                out["target_port"] = str(body.get("DestPort", "")).strip()
                out["ptp_status"] = str(body.get("PTPStatus", "")).strip()

            # Check for ethernet_all
            if "Control_IP" in body and isinstance(body["Control_IP"], dict):
                hesai_detected = True
                cip = body["Control_IP"]
                out["ip"] = str(cip.get("IPv4", out["ip"])).strip()
                out["subnet_mask"] = str(cip.get("Mask", "")).strip()
                out["gateway"] = str(cip.get("Gateway", "")).strip()
                vflag = cip.get("VlanFlag", 0)
                vid = cip.get("VlanID", "")
                out["vlan"] = f"VLAN ID {vid}" if vflag else "未启用"

        if hesai_detected and not out["brand"]:
            out["brand"] = "禾赛 (Hesai)"

    def _extract_from_json(self, data: dict[str, Any], out: dict[str, Any]) -> None:
        flat = self._flatten_dict(data)

        for key, val in flat.items():
            sval = str(val).strip()
            if not sval:
                continue
            lkey = key.lower()

            if not out["sn"] and any(k in lkey for k in ("serial", "sn", "serial_number", "serialnumber")):
                if len(sval) >= 4 and not any(bad in sval.lower() for bad in ("null", "none", "false")):
                    out["sn"] = sval

            if not out["model"] and any(k in lkey for k in ("model", "lidar_type", "devicetype", "product", "prodname")):
                out["model"] = sval

            if not out["firmware"] and any(k in lkey for k in ("firmware", "sw_ver", "software", "app_ver", "fw_ver")):
                out["firmware"] = sval

            if not out["hardware"] and any(k in lkey for k in ("hardware", "hw_ver")):
                out["hardware"] = sval

            if not out["voltage"] and any(k in lkey for k in ("voltage", "power_v", "volt")):
                out["voltage"] = f"{sval} V" if not sval.lower().endswith("v") else sval

            if not out["current"] and any(k in lkey for k in ("current", "power_a", "curr")) and not any(bad in lkey for bad in ("temp", "temperature")):
                out["current"] = f"{sval} A" if not sval.lower().endswith("a") else sval

            if not out["rpm"] and any(k in lkey for k in ("rpm", "motorspeed", "motor_speed", "speed")):
                out["rpm"] = f"{sval} RPM" if not sval.lower().endswith("rpm") else sval

            if not out["temperature"] and any(k in lkey for k in ("temp", "temperature", "rt_top", "rt_bottom")):
                out["temperature"] = f"{sval} ℃" if not ("℃" in sval or "°c" in sval.lower()) else sval

            if not out["target_ip"] and any(k in lkey for k in ("dest_ip", "destination_ip", "target_ip", "dip")):
                out["target_ip"] = sval

            if not out["target_port"] and any(k in lkey for k in ("dest_port", "destination_port", "dport", "msop_port")):
                out["target_port"] = sval

            if not out["mac"] and "mac" in lkey:
                out["mac"] = sval

            if not out["subnet_mask"] and any(k in lkey for k in ("mask", "netmask", "subnet")):
                out["subnet_mask"] = sval

            if not out["gateway"] and "gateway" in lkey:
                out["gateway"] = sval

            if not out["vlan"] and "vlan" in lkey:
                out["vlan"] = sval

    def _extract_from_html(self, text: str, out: dict[str, Any]) -> None:
        clean = re.sub(r"<(script|style)[^>]*>.*?</\1>", " ", text, flags=re.DOTALL | re.IGNORECASE)
        clean = html.unescape(clean)

        patterns = {
            "sn": (
                r"(?:S/N|Serial\s*Number|序列号|机器编号|雷达编号)[\s:：]+([A-Za-z0-9_\-]+)",
                r'name=["\'](?:sn|serial)["\'][^>]*value=["\']([^"\']+)["\']',
            ),
            "model": (
                r"(?:Model|LiDAR\s*Model|雷达型号|设备型号)[\s:：]+([A-Za-z0-9_\-]+)",
            ),
            "firmware": (
                r"(?:Firmware\s*Version|Software\s*Version|固件版本|软件版本|APP\s*版本)[\s:：]+([vV0-9._\-]+)",
            ),
            "hardware": (
                r"(?:Hardware\s*Version|硬件版本)[\s:：]+([vV0-9._\-]+)",
            ),
            "voltage": (
                r"(?:Voltage|Operating\s*Voltage|工作电压|供电电压|输入电压)[\s:：]+([0-9.]+)\s*(?:V|伏)?",
            ),
            "current": (
                r"(?:Current|Operating\s*Current|工作电流|整机电流)[\s:：]+([0-9.]+)\s*(?:A|mA|毫安|安)?",
            ),
            "rpm": (
                r"(?:Motor\s*Speed|Rotation\s*Speed|Speed|转速|电机转速)[\s:：]+([0-9]+)\s*(?:RPM|rpm|转/分)?",
            ),
            "temperature": (
                r"(?:Temperature|Temp|内部温度|工作温度|机芯温度)[\s:：]+([0-9.\-]+)\s*(?:℃|°C|C)?",
            ),
            "mac": (
                r"(?:MAC|物理地址|MAC\s*地址)[\s:：]+([0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2})",
            ),
            "target_ip": (
                r"(?:Destination\s*IP|Target\s*IP|目标\s*IP)[\s:：]+([0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3})",
            ),
            "target_port": (
                r"(?:Destination\s*Port|Target\s*Port|MSOP\s*Port|目标端口|点云端口)[\s:：]+([0-9]{2,5})",
            ),
            "gateway": (
                r"(?:Gateway|网关)[\s:：]+([0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3})",
            ),
            "subnet_mask": (
                r"(?:Netmask|Subnet\s*Mask|子网掩码)[\s:：]+([0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3})",
            ),
        }

        for field, regexes in patterns.items():
            if out.get(field):
                continue
            for rgx in regexes:
                match = re.search(rgx, clean, re.IGNORECASE)
                if match:
                    val = match.group(1).strip()
                    if field in ("voltage", "current", "rpm", "temperature") and not re.search(r"[a-zA-Z℃°]", val):
                        suffix = {"voltage": " V", "current": " A", "rpm": " RPM", "temperature": " ℃"}.get(field, "")
                        val += suffix
                    out[field] = val
                    break

    @staticmethod
    def _flatten_dict(d: dict[str, Any], parent_key: str = "", sep: str = ".") -> dict[str, Any]:
        items: list[tuple[str, Any]] = []
        for k, v in d.items():
            new_key = f"{parent_key}{sep}{k}" if parent_key else k
            if isinstance(v, dict):
                items.extend(LidarWebScraper._flatten_dict(v, new_key, sep=sep).items())
            else:
                items.append((new_key, v))
        return dict(items)

    @staticmethod
    def generate_section_notes(data: dict[str, Any], ip: str) -> dict[int, str]:
        """Format values observed on the device web UI without inventing conclusions."""
        notes: dict[int, str] = {}

        # 02 供电 (Section Index 1)
        power_lines = ["[雷达网页端实测数据]"]
        if data.get("voltage"):
            power_lines.append(f"工作电压：{data['voltage']}")
        if data.get("current"):
            power_lines.append(f"工作电流：{data['current']}")
        if data.get("power"):
            power_lines.append(f"整机功耗：{data['power']}")
        if data.get("temperature"):
            power_lines.append(f"机芯内部实时温度：{data['temperature']}")
        if data.get("total_operation_time") or data.get("working_hours"):
            op_t = data.get("working_hours") or data.get("total_operation_time")
            power_lines.append(f"累计运行时间 (Total Operation Time)：{op_t}")
        if data.get("startup_times"):
            power_lines.append(f"开机启动次数 (Start-up Counts)：{data['startup_times']} 次")
        if data.get("system_uptime"):
            power_lines.append(f"本次运行时间 (System Uptime)：{data['system_uptime']}")
        power_lines.append("是否处于允许范围需按对应型号规格判定。")
        notes[1] = "\n".join(power_lines)

        # 03 网络连接 (Section Index 2)
        net_lines = ["[雷达网页端网络配置]"]
        net_lines.append(f"雷达 IP：{data.get('ip') or ip}")
        if data.get("subnet_mask"):
            net_lines.append(f"子网掩码：{data['subnet_mask']}")
        if data.get("gateway"):
            net_lines.append(f"网关地址：{data['gateway']}")
        if data.get("target_ip"):
            net_lines.append(f"点云目标 IP：{data['target_ip']}")
        if data.get("target_port"):
            net_lines.append(f"点云目标端口：{data['target_port']}")
        if data.get("mac"):
            net_lines.append(f"物理 MAC：{data['mac']}")
        if data.get("vlan"):
            net_lines.append(f"VLAN 配置：{data['vlan']}")
        net_lines.append("以上为设备网页返回的配置，链路质量需结合抓包统计判定。")
        notes[2] = "\n".join(net_lines)

        # 04 SDK识别 (Section Index 3)
        sdk_lines = ["[雷达固件与设备信息]"]
        if data.get("brand"):
            sdk_lines.append(f"设备品牌：{data['brand']}")
        if data.get("model"):
            laser_str = f" ({data['laser_num']}线)" if data.get("laser_num") else ""
            sdk_lines.append(f"硬件型号：{data['model']}{laser_str}")
        if data.get("sn"):
            sdk_lines.append(f"设备序列号 S/N：{data['sn']}")
        if data.get("firmware"):
            sdk_lines.append(f"软件/固件版本：{data['firmware']}")
        if data.get("hardware"):
            sdk_lines.append(f"硬件/主板版本：{data['hardware']}")
        if data.get("prod_date"):
            sdk_lines.append(f"出厂生产日期：{data['prod_date']}")
        sdk_lines.append("以上为设备网页读取值，SDK解码与标定状态以查看器状态为准。")
        notes[3] = "\n".join(sdk_lines)

        # 06 转速 (Section Index 5)
        rpm_lines = ["[雷达网页端电机转速参数]"]
        if data.get("rpm"):
            rpm_lines.append(f"电机设定/实测转速：{data['rpm']}")
        rpm_lines.append("未读取到转速时不作默认推断；机械状态需由检验人员确认。")
        notes[5] = "\n".join(rpm_lines)

        # 07 丢包 (Section Index 6)
        diag_lines = ["[雷达网页端诊断与运行自检]"]
        if data.get("ptp_status"):
            diag_lines.append(f"时钟/PTP同步状态：{data['ptp_status']}")
        if data.get("diagnostics"):
            diag_lines.append(f"诊断详情：{data['diagnostics']}")
        diag_lines.append("网页状态不能代替网络丢包率统计；结论需结合序号或抓包结果。")
        notes[6] = "\n".join(diag_lines)

        return notes
