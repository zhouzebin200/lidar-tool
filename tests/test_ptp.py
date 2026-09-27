import socket
from pathlib import Path
import tempfile
import unittest

from ulv.ptp import PtpController, network_interfaces


class PtpTests(unittest.TestCase):
    def test_interfaces_are_safe_kernel_names(self):
        expected = {name for _index, name in socket.if_nameindex() if name != "lo"}
        self.assertEqual({item["name"] for item in network_interfaces()}, expected)

    def test_validates_generic_ptp_profile(self):
        interface = next((name for _index, name in socket.if_nameindex()), "")
        with tempfile.TemporaryDirectory() as directory:
            controller = PtpController(Path(directory) / "ptp.json", Path(directory) / "run")
            value = controller.validate({
                "enabled": True,
                "interface": interface,
                "role": "master",
                "transport": "L2",
                "timestamping": "software",
                "domain": 7,
                "priority1": 100,
                "delay_mechanism": "P2P",
            })
            self.assertEqual(value["domain"], 7)
            self.assertEqual(value["role"], "master")

    def test_rejects_out_of_range_domain(self):
        with self.assertRaisesRegex(ValueError, "Domain"):
            PtpController.validate({"enabled": False, "domain": 128})


if __name__ == "__main__":
    unittest.main()
