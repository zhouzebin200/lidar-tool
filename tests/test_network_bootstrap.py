import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ulv.network_bootstrap import WindowsNetworkBootstrapManager, _windows_interface_guid
from ulv.server import Application


class NetworkBootstrapTests(unittest.TestCase):
    def test_extracts_only_a_valid_npcap_guid(self):
        value = r"\Device\NPF_{01234567-89AB-CDEF-0123-456789ABCDEF}"
        self.assertEqual(
            _windows_interface_guid(value),
            "01234567-89AB-CDEF-0123-456789ABCDEF",
        )
        self.assertEqual(_windows_interface_guid(r"\Device\NPF_bad"), "")

    def test_recovery_record_validation_rejects_script_fields(self):
        valid = {
            "guid": "01234567-89AB-CDEF-0123-456789ABCDEF",
            "vlan_keyword": "RegVlanid",
            "requested_ip": "172.20.2.55",
            "original_ipv4": ["192.168.1.102"],
            "original_vlan": 0,
            "active_vlan": 2,
            "priority_keyword": "*PriorityVLANTag",
            "original_priority": 0,
            "dhcp_enabled": False,
            "vlan_changed": True,
        }
        self.assertEqual(
            WindowsNetworkBootstrapManager._validate_record(valid)["requested_ip"],
            "172.20.2.55",
        )
        invalid = dict(valid, vlan_keyword="x'; Remove-Item C:\\*")
        with self.assertRaises(ValueError):
            WindowsNetworkBootstrapManager._validate_record(invalid)

    def test_host_ip_is_derived_from_detected_flow(self):
        multicast = {"source_ip": "172.16.4.31", "destination_ip": "225.0.4.21"}
        unicast = {"source_ip": "192.168.10.201", "destination_ip": "192.168.10.55"}
        self.assertEqual(Application._host_ip_for_flow(multicast), "172.16.4.100")
        self.assertEqual(Application._host_ip_for_flow(unicast), "192.168.10.55")

    def test_empty_recovery_is_a_noop(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = WindowsNetworkBootstrapManager(Path(directory) / "recovery.json")
            success, message = manager.restore_all()
            self.assertTrue(success)
            self.assertEqual(message, "")

    def test_apply_journals_then_restore_removes_journal(self):
        original = {
            "guid": "01234567-89AB-CDEF-0123-456789ABCDEF",
            "vlan_keyword": "RegVlanid",
            "original_vlan": 0,
            "priority_keyword": "*PriorityVLANTag",
            "original_priority": 0,
            "dhcp_enabled": False,
            "original_ipv4": ["192.168.1.102"],
        }
        with tempfile.TemporaryDirectory() as directory:
            journal = Path(directory) / "network-recovery.json"
            manager = WindowsNetworkBootstrapManager(journal)
            scripts = []

            def fake_run(script, timeout=45):
                scripts.append(script)
                self.assertTrue(journal.is_file())
                return True, ""

            with patch("ulv.network_bootstrap.sys.platform", "win32"), \
                 patch("ulv.network_bootstrap.windows_is_admin", return_value=True), \
                 patch.object(manager, "_apply_fast", return_value=(False, "test fallback")), \
                 patch.object(manager, "_query_adapter", return_value=(True, original)), \
                 patch.object(manager, "_run_powershell", side_effect=fake_run):
                success, _ = manager.apply(
                    r"\Device\NPF_{01234567-89AB-CDEF-0123-456789ABCDEF}",
                    "172.16.4.31", "172.16.4.100", 4,
                )
                self.assertTrue(success)
                self.assertIn("New-NetIPAddress", scripts[0])
                self.assertIn("*PriorityVLANTag", scripts[0])
                restored, _ = manager.restore_all()
                self.assertTrue(restored)
                self.assertFalse(journal.exists())
                self.assertIn("Remove-NetIPAddress", scripts[1])


if __name__ == "__main__":
    unittest.main()
