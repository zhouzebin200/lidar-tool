import unittest
from unittest.mock import patch
from ulv.lidar_web_scraper import LidarWebScraper


class TestLidarWebScraper(unittest.TestCase):
    def setUp(self):
        self.scraper = LidarWebScraper()

    def test_pandar40_prefers_web_data_over_at128_ptc(self):
        responses = {
            "device_info": {"Head": {"ErrorCode": "0"}, "Body": {
                "SN": "PA4031CC5E9331CD57", "ProdName": "Pandar40P",
                "ProdDate": "2021-08-03", "SW_Ver": "2.10.4",
                "HW_Ver": "1.0.0", "FW_Ver": "4.51", "LaserNum": "40"}},
            "TimeStatistic": {"Head": {"ErrorCode": "0"}, "Body": {
                "StartupTimes": "2525", "TotalWorkingTime": "236309"}},
        }

        def fetch(url):
            for name, payload in responses.items():
                if f"object={name}" in url:
                    return payload, True
            return None, False

        with patch.object(self.scraper, "_fetch", side_effect=fetch), \
             patch("ulv.lidar_web_scraper.HesaiPtcClient.query_all", side_effect=AssertionError("PTC must not run")):
            result = self.scraper.scrape("192.168.1.201")

        self.assertTrue(result["success"])
        self.assertEqual(result["source"], "lidar_web")
        self.assertEqual(result["data"]["model"], "Pandar40P")
        self.assertEqual(result["data"]["sn"], "PA4031CC5E9331CD57")
        self.assertEqual(result["data"]["startup_times"], "2525")

    def test_extract_hesai_data(self):
        pages = {
            "http://192.168.1.201/pandar.cgi?action=get&object=device_info": {
                "Head": {"ErrorCode": "0", "Message": "Success"},
                "Body": {
                    "SN": "QT3CC855913CC857",
                    "Mac": "EC:9F:0D:00:59:E7",
                    "SW_Ver": "2.0.12",
                    "HW_Ver": "2.2.0",
                    "FW_Ver": "2.0.20",
                    "Model": "Pandar_ZYNQ",
                    "ProdDate": "2020-11-02",
                    "ProdName": "PandarQT",
                    "LaserNum": "64",
                    "Up_Fpga_Ver": "2.0.10",
                }
            },
            "http://192.168.1.201/pandar.cgi?action=get&object=lidar_config": {
                "Head": {"ErrorCode": "0", "Message": "Success"},
                "Body": {
                    "SpinSpeed": "2",
                    "DestIp": "255.255.255.255",
                    "DestPort": "2368",
                    "PTPStatus": "Free Run",
                }
            },
            "http://192.168.1.201/pandar.cgi?action=get&object=ethernet_all": {
                "Head": {"ErrorCode": "0", "Message": "Success"},
                "Body": {
                    "Control_IP": {
                        "IPv4": "192.168.1.201",
                        "Mask": "255.255.255.0",
                        "Gateway": "192.168.1.1",
                        "VlanFlag": 0,
                    }
                }
            },
            "http://192.168.1.201/pandar.cgi?action=get&object=TimeStatistic": {
                "Head": {"ErrorCode": "0", "Message": "Success"},
                "Body": {
                    "StartupTimes": "138",
                    "CurrentTemp": "40.39",
                    "TotalWorkingTime": "7041",
                }
            }
        }

        extracted = self.scraper.extract_fields(pages, default_ip="192.168.1.201")
        self.assertEqual(extracted["brand"], "禾赛 (Hesai)")
        self.assertEqual(extracted["model"], "PandarQT")
        self.assertEqual(extracted["laser_num"], "64")
        self.assertEqual(extracted["sn"], "QT3CC855913CC857")
        self.assertEqual(extracted["prod_date"], "2020-11-02")
        self.assertEqual(extracted["firmware"], "FW 2.0.20 / SW 2.0.12")
        self.assertEqual(extracted["hardware"], "HW 2.2.0 (FPGA 2.0.10)")
        self.assertEqual(extracted["rpm"], "600 RPM (10 Hz)")
        self.assertEqual(extracted["temperature"], "40.39 ℃")
        self.assertEqual(extracted["ip"], "192.168.1.201")
        self.assertEqual(extracted["subnet_mask"], "255.255.255.0")
        self.assertEqual(extracted["gateway"], "192.168.1.1")
        self.assertEqual(extracted["target_ip"], "255.255.255.255")
        self.assertEqual(extracted["target_port"], "2368")
        self.assertEqual(extracted["mac"], "EC:9F:0D:00:59:E7")
        self.assertEqual(extracted["startup_times"], "138")
        self.assertEqual(extracted["total_operation_time"], "4.9 天 (117 h 21 min)")
        self.assertEqual(extracted["working_hours"], "4.9 天 (约 117 小时 21 分钟 / 117 h 21 min)")
        self.assertEqual(extracted["usage_str"], "开机 138 次 / 累计时长 4.9 天 (117 h 21 min)")

        # Check section notes
        notes = self.scraper.generate_section_notes(extracted, "192.168.1.201")
        self.assertIn("40.39 ℃", notes[1])
        self.assertIn("4.9 天", notes[1])
        self.assertIn("Total Operation Time", notes[1])
        self.assertIn("192.168.1.201", notes[2])
        self.assertIn("2368", notes[2])
        self.assertIn("QT3CC855913CC857", notes[3])
        self.assertIn("PandarQT (64线)", notes[3])
        self.assertIn("600 RPM (10 Hz)", notes[5])
        self.assertIn("Free Run", notes[6])

    def test_generic_html_extraction(self):
        html_content = """
        <html><body>
        <h1>LiDAR Configuration</h1>
        <div>Serial Number: RS128-TEST-8888</div>
        <div>Model: RS-Helios</div>
        <div>Operating Voltage: 12.0 V</div>
        <div>Operating Current: 1.25 A</div>
        <div>Motor Speed: 1200 RPM</div>
        <div>Temperature: 36.5 ℃</div>
        <div>Target IP: 192.168.1.102</div>
        <div>Target Port: 6699</div>
        </body></html>
        """
        extracted = self.scraper.extract_fields({"http://192.168.1.200/": html_content}, default_ip="192.168.1.200")
        self.assertEqual(extracted["sn"], "RS128-TEST-8888")
        self.assertEqual(extracted["model"], "RS-Helios")
        self.assertEqual(extracted["voltage"], "12.0 V")
        self.assertEqual(extracted["current"], "1.25 A")
        self.assertEqual(extracted["rpm"], "1200 RPM")
        self.assertEqual(extracted["temperature"], "36.5 ℃")
        self.assertEqual(extracted["target_ip"], "192.168.1.102")
        self.assertEqual(extracted["target_port"], "6699")


if __name__ == "__main__":
    unittest.main()
