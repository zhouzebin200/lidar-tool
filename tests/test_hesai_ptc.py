# -*- coding: utf-8 -*-
import socket
import struct
import threading
import unittest

from ulv.hesai_ptc import HesaiPtcClient
from ulv.lidar_web_scraper import LidarWebScraper


class MockHesaiPtcServer:
    """Mock TCP server implementing Hesai PTC 9347 protocol for testing."""

    def __init__(self, port: int = 0, model: str = "AT128P", use_ptc2: bool = False) -> None:
        self.model = model
        self.use_ptc2 = use_ptc2
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", port))
        self.port = self.sock.getsockname()[1]
        self.sock.listen(5)
        self._running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self) -> None:
        while self._running:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                break
            handler = threading.Thread(target=self._handle, args=(conn,), daemon=True)
            handler.start()

    def _handle(self, conn: socket.socket) -> None:
        conn.settimeout(2.0)
        try:
            while self._running:
                header = conn.recv(8)
                if len(header) < 8:
                    break
                if header[:2] != b"\x47\x74":
                    break
                cmd = header[2]
                payload_len = struct.unpack(">I", header[4:8])[0]
                payload = b""
                while len(payload) < payload_len:
                    chunk = conn.recv(min(4096, payload_len - len(payload)))
                    if not chunk:
                        break
                    payload += chunk

                resp_payload = self._build_response(cmd, payload)
                if resp_payload is None:
                    # Return non-zero return code (e.g. command not supported)
                    if self.use_ptc2:
                        resp_header = b"\x57\x75\x01" + bytes([cmd, 0x01, 0x00, 0x00, 0x00]) + struct.pack(">I", 0)
                    else:
                        resp_header = b"\x47\x74" + bytes([cmd, 0x01]) + struct.pack(">I", 0)
                    conn.sendall(resp_header)
                    continue

                if self.use_ptc2:
                    # PTC 2.0: 12-byte header: "Wu" + ver(1B) + cmd(1B) + ret(1B) + rsv(1B) + check_type(1B) + check_len(1B) + payload_len(4B)
                    resp_header = b"\x57\x75\x01" + bytes([cmd, 0x00, 0x00, 0x00, 0x00]) + struct.pack(">I", len(resp_payload))
                else:
                    # PTC 1.0: 8-byte header: "Gt" + cmd(1B) + ret(1B) + payload_len(4B)
                    resp_header = b"\x47\x74" + bytes([cmd, 0x00]) + struct.pack(">I", len(resp_payload))
                conn.sendall(resp_header + resp_payload)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _build_response(self, cmd: int, payload: bytes) -> bytes | None:
        if cmd == 0x07:  # Get Inventory Info
            if self.model == "OT128":
                buf = bytearray(228)
                sn_bytes = b"OT128202403010001"
                buf[0 : len(sn_bytes)] = sn_bytes
                dom_bytes = b"2024-03-01"
                buf[18 : 18 + len(dom_bytes)] = dom_bytes
                buf[34:40] = bytes([0xEC, 0x9F, 0x0D, 0xAA, 0xBB, 0xCC])
                sw_bytes = b"3.5.10"
                buf[40 : 40 + len(sw_bytes)] = sw_bytes
                hw_bytes = b"1.2.0"
                buf[56 : 56 + len(hw_bytes)] = hw_bytes
                ctrl_bytes = b"2.3.4"
                buf[72 : 72 + len(ctrl_bytes)] = ctrl_bytes
                sensor_bytes = b"1.0.8"
                buf[88 : 88 + len(sensor_bytes)] = sensor_bytes
                buf[106] = 128  # model code
                buf[108] = 128  # lines
                pn_bytes = b"OT128-TOP-MECH"
                buf[109 : 109 + len(pn_bytes)] = pn_bytes
                return bytes(buf)

            buf = bytearray(216)
            if self.model == "FT120":
                sn_bytes = b"FT120202401010001"
                pn_bytes = b"FT120-C1X-128"
            elif self.model == "ATX":
                sn_bytes = b"ATX202405010001"
                pn_bytes = b"ATX-GEN2-128"
            elif self.model == "JT128":
                sn_bytes = b"JT128202406010001"
                pn_bytes = b"JT128-ROBOTIC"
            else:
                sn_bytes = b"AT128P202311200001"
                pn_bytes = b"AT128P-ARA02-128"

            buf[0 : len(sn_bytes)] = sn_bytes
            buf[18 : 18 + len(pn_bytes)] = pn_bytes
            dom_bytes = b"2023-11-20"
            buf[50 : 50 + len(dom_bytes)] = dom_bytes
            buf[66:72] = bytes([0xEC, 0x9F, 0x0D, 0x11, 0x22, 0x33])
            sw_bytes = b"1.1.20"
            buf[72 : 72 + len(sw_bytes)] = sw_bytes
            hw_bytes = b"2.0.0"
            buf[88 : 88 + len(hw_bytes)] = hw_bytes
            fw_bytes = b"2.1.8"
            buf[104 : 104 + len(fw_bytes)] = fw_bytes
            fpga_bytes = b"3.0.12"
            buf[152 : 152 + len(fpga_bytes)] = fpga_bytes
            buf[204] = 128
            return bytes(buf)

        elif cmd == 0x09:  # Get Lidar Status
            if self.model == "OT128":
                buf = bytearray(54)
                struct.pack_into(">I", buf, 0, 7200)  # uptime
                struct.pack_into(">H", buf, 4, 600)   # motor_speed 600 RPM
                for i in range(8):
                    struct.pack_into(">i", buf, 6 + i * 4, 3800 + i * 15)  # temps
                struct.pack_into(">I", buf, 40, 15)   # startup
                struct.pack_into(">I", buf, 44, 864000)  # 10 days
                buf[48] = 2  # PTP Locked
                struct.pack_into(">i", buf, 49, 350)  # humidity 35.0%
                return bytes(buf)

            buf = bytearray(58)
            struct.pack_into(">I", buf, 0, 3600)
            if self.model == "FT120":
                struct.pack_into(">H", buf, 4, 0)     # pure solid state: 0 RPM
            else:
                struct.pack_into(">H", buf, 4, 200)   # 200 RPM
            for i in range(9):
                struct.pack_into(">i", buf, 6 + i * 4, 3550 + i * 20)
            struct.pack_into(">I", buf, 44, 42)
            struct.pack_into(">I", buf, 48, 432000)
            buf[52] = 2  # Locked
            if self.model == "FT120":
                struct.pack_into(">I", buf, 53, 425)  # humidity 42.5%
            return bytes(buf)

        elif cmd == 0x27:  # Get Lidar Monitor
            if self.model == "FT120":
                # FT120 does not implement 0x27 electrical monitor
                return None
            buf = bytearray(64)
            struct.pack_into(">i", buf, 0, 120000)
            struct.pack_into(">i", buf, 4, 1250)
            struct.pack_into(">i", buf, 8, 1500)
            return bytes(buf)

        elif cmd == 0x08:  # Get Config Info
            buf = bytearray(48)
            buf[0:4] = bytes([192, 168, 1, 201])
            buf[4:8] = bytes([255, 255, 255, 0])
            buf[8:12] = bytes([192, 168, 1, 1])
            buf[12:16] = bytes([192, 168, 1, 100])
            struct.pack_into(">H", buf, 16, 2368)
            struct.pack_into(">H", buf, 20, 0 if self.model == "FT120" else 600 if self.model == "OT128" else 200)
            buf[27] = 2  # Dual return
            return bytes(buf)

        elif cmd == 0xFF:  # Extended (Model)
            sub = struct.unpack(">I", payload[:4])[0] if len(payload) >= 4 else 0
            if sub in (0x70, 0x00010070):
                resp = bytearray(5)
                struct.pack_into(">I", resp, 0, sub)
                resp[4] = 60 if self.model == "AT128P" else 128
                return bytes(resp)
            return b""

        elif cmd == 0x8D:  # Work mode & Fault
            return bytes([1, 0])  # Standard mode, no fault

        elif cmd == 0x06:  # PTP diagnostics
            buf = bytearray(16)
            struct.pack_into(">q", buf, 0, 12)   # 12 ns offset
            struct.pack_into(">q", buf, 8, 10)
            return bytes(buf)

        return b""

    def stop(self) -> None:
        self._running = False
        try:
            self.sock.close()
        except OSError:
            pass


class TestHesaiPtc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = MockHesaiPtcServer(model="AT128P")
        cls.port = cls.server.port

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_ptc_query_all(self):
        client = HesaiPtcClient(timeout=2.0)
        res = client.query_all("127.0.0.1", port=self.port)
        self.assertTrue(res["success"])
        self.assertEqual(res["source"], "hesai_ptc")

        data = res["data"]
        self.assertEqual(data["brand"], "禾赛科技")
        self.assertEqual(data["model"], "Hesai AT128P")
        self.assertEqual(data["sn"], "AT128P202311200001")
        self.assertEqual(data["pn"], "AT128P-ARA02-128")
        self.assertEqual(data["prod_date"], "2023-11-20")
        self.assertEqual(data["mac"], "EC:9F:0D:11:22:33")
        self.assertIn("5.0 天", data["usage_str"])
        self.assertIn("120 h", data["usage_str"])
        self.assertIn("开机 42 次", data["usage_str"])
        self.assertEqual(data["voltage"], "12.50 V")
        self.assertEqual(data["current"], "1200.0 mA")
        self.assertEqual(data["power"], "15.00 W")
        self.assertEqual(data["rpm"], "200 RPM")
        self.assertEqual(data["destination_port"], 2368)

        # Verify section notes
        notes = res["section_notes"]
        self.assertIn("AT128P", notes[0])
        self.assertIn("12.50 V", notes[1])
        self.assertIn("15.00 W", notes[1])
        self.assertIn("192.168.1.201", notes[2])
        self.assertIn("2368", notes[2])
        self.assertIn("Hesai AT128P", notes[3])
        self.assertIn("128 线", notes[3])
        self.assertIn("200 RPM", notes[5])

    def test_ft120_pure_solid_state(self):
        """Test FT120 solid-state Flash LiDAR: 0 RPM, humidity, no 0x27 monitor."""
        ft_server = MockHesaiPtcServer(model="FT120")
        try:
            client = HesaiPtcClient(timeout=2.0)
            res = client.query_all("127.0.0.1", port=ft_server.port)
            self.assertTrue(res["success"])
            data = res["data"]
            self.assertEqual(data["model"], "Hesai FT120")
            self.assertEqual(data["model_family"], "FLASH_SOLID_STATE")
            self.assertEqual(data["rpm"], "0 RPM (纯固态 Flash)")
            self.assertEqual(data["humidity"], 42.5)

            notes = res["section_notes"]
            self.assertIn("FT120", notes[0])
            self.assertIn("42.5% RH", notes[1])
            self.assertIn("纯固态面阵 Flash 架构", notes[5])
            self.assertIn("免机械磨损", notes[5])
        finally:
            ft_server.stop()

    def test_ot128_mechanical_360(self):
        """Test OT128 mechanical 360 flagship: 8 temps, humidity, 600 RPM, OT128 layout."""
        ot_server = MockHesaiPtcServer(model="OT128")
        try:
            client = HesaiPtcClient(timeout=2.0)
            res = client.query_all("127.0.0.1", port=ot_server.port)
            self.assertTrue(res["success"])
            data = res["data"]
            self.assertEqual(data["model"], "Hesai OT128")
            self.assertEqual(data["model_family"], "MECHANICAL_360")
            self.assertEqual(data["rpm"], "600 RPM")
            self.assertEqual(data["humidity"], 35.0)
            self.assertEqual(len(data["temperatures"]), 8)

            notes = res["section_notes"]
            self.assertIn("OT128", notes[0])
            self.assertIn("35.0% RH", notes[1])
            self.assertIn("600 RPM", notes[5])
        finally:
            ot_server.stop()

    def test_ptc_2_0_wu_framing(self):
        """Test PTC 2.0 ('Wu' / 0x57 0x75) 12-byte header unpacking."""
        p2_server = MockHesaiPtcServer(model="ATX", use_ptc2=True)
        try:
            client = HesaiPtcClient(timeout=2.0)
            res = client.query_all("127.0.0.1", port=p2_server.port)
            self.assertTrue(res["success"])
            data = res["data"]
            self.assertEqual(data["model"], "Hesai ATX")
            self.assertEqual(data["sn"], "ATX202405010001")
        finally:
            p2_server.stop()

    def test_scraper_integration_with_ptc(self):
        scraper = LidarWebScraper(timeout=2.0)
        res = scraper.scrape("127.0.0.1", port=self.port)
        self.assertTrue(res["success"])
        self.assertEqual(res["data"]["sn"], "AT128P202311200001")
        self.assertEqual(res["data"]["model"], "Hesai AT128P")

    def test_connection_failure(self):
        client = HesaiPtcClient(timeout=0.1)
        res = client.query_all("127.0.0.1", port=1)
        self.assertFalse(res["success"])
        self.assertIn("无法通过 TCP 连接", res["error"])

    def test_scraper_fallback_when_ptc_unavailable(self):
        scraper = LidarWebScraper(timeout=0.1)
        res = scraper.scrape("127.0.0.1", port=1)
        self.assertFalse(res["success"])
        self.assertIn("无法连接到雷达", res["error"])


if __name__ == "__main__":
    unittest.main()
