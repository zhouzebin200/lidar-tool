import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ulv.capture import CapturedArp, PacketCapture
from ulv.network_bootstrap import MacOSNetworkBootstrapManager
from ulv.registry import DeviceRegistry


class MacOSNetworkTests(unittest.TestCase):
    def test_alias_is_journaled_and_only_added_address_is_restored(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Path(directory) / "network-recovery.json"
            manager = MacOSNetworkBootstrapManager(journal)
            addresses = ["192.168.11.22"]
            changes = []

            def run(args):
                if args[0].endswith("networksetup"):
                    return True, "Hardware Port: USB 10/100 LAN\nDevice: en5\nEthernet Address: 00:24:27:88:02:3c\n"
                if len(args) == 2:
                    return True, "ether 00:24:27:88:02:3c\n" + "\n".join(f"inet {ip} netmask 0xffffff00" for ip in addresses)
                self.assertTrue(journal.exists())
                self.assertEqual(json.loads(journal.read_text())["records"][0]["requested_ip"], "192.168.11.222")
                changes.append(args)
                if args[2] == "alias":
                    addresses.append(args[3])
                else:
                    addresses.remove(args[3])
                return True, ""

            with patch("ulv.network_bootstrap.sys.platform", "darwin"), \
                    patch("ulv.network_bootstrap.os.geteuid", return_value=0), \
                    patch.object(manager, "_run", side_effect=run):
                self.assertTrue(manager.apply("en5", "192.168.11.41", "192.168.11.222", 0)[0])
                self.assertEqual(addresses, ["192.168.11.22", "192.168.11.222"])
                # A restarted process recovers the recorded alias too.
                restarted = MacOSNetworkBootstrapManager(journal)
                with patch.object(restarted, "_run", side_effect=run):
                    self.assertTrue(restarted.recover_pending()[0])
                self.assertEqual(addresses, ["192.168.11.22"])
                self.assertFalse(journal.exists())
                self.assertEqual(changes[-1], ["/sbin/ifconfig", "en5", "-alias", "192.168.11.222"])

    def test_existing_target_is_not_owned_or_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = MacOSNetworkBootstrapManager(Path(directory) / "journal.json")
            with patch("ulv.network_bootstrap.sys.platform", "darwin"), \
                    patch("ulv.network_bootstrap.os.geteuid", return_value=0), \
                    patch.object(manager, "_interface", return_value=(True, {"addresses": ["192.168.11.222"]})), \
                    patch.object(manager, "_run") as run:
                self.assertTrue(manager.apply("en5", "192.168.11.41", "192.168.11.222", 0)[0])
                self.assertTrue(manager.restore_all()[0])
                run.assert_not_called()
                self.assertFalse(manager.journal_path.exists())

    def test_no_authority_vlan_and_invalid_targets_do_not_mutate(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = MacOSNetworkBootstrapManager(Path(directory) / "journal.json")
            with patch("ulv.network_bootstrap.sys.platform", "darwin"), \
                    patch("ulv.network_bootstrap.os.geteuid", return_value=501), \
                    patch.object(manager, "_run") as run:
                self.assertFalse(manager.apply("en5", "192.168.11.41", "192.168.11.222", 0)[0])
                run.assert_not_called()
            with patch("ulv.network_bootstrap.sys.platform", "darwin"), \
                    patch("ulv.network_bootstrap.os.geteuid", return_value=0), \
                    patch.object(manager, "_run") as run:
                for target, vlan in [("192.168.11.222", 5), ("192.168.11.1", 0), ("10.0.0.22", 0)]:
                    self.assertFalse(manager.apply("en5", "192.168.11.41", target, vlan)[0])
                run.assert_not_called()

    def test_wifi_is_rejected(self):
        manager = MacOSNetworkBootstrapManager(Path("unused"))
        with patch.object(manager, "_run", return_value=(True, "Hardware Port: Wi-Fi\nDevice: en0\n")):
            self.assertFalse(manager._interface("en0")[0])

    def test_unplugged_or_replaced_adapter_keeps_recovery_journal(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = MacOSNetworkBootstrapManager(Path(directory) / "journal.json")
            manager._write([{"interface": "en5", "mac": "00:24:27:88:02:3c",
                             "requested_ip": "192.168.11.222", "original_ipv4": ["192.168.11.22"]}])
            with patch("ulv.network_bootstrap.os.geteuid", return_value=0), \
                    patch.object(manager, "_interface", return_value=(True, {"mac": "aa:bb:cc:dd:ee:ff"})), \
                    patch.object(manager, "_run") as run:
                self.assertFalse(manager.restore_all()[0])
                run.assert_not_called()
                self.assertTrue(manager.journal_path.exists())

    def test_failed_add_can_be_recovered_without_removing_original_ip(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = MacOSNetworkBootstrapManager(Path(directory) / "journal.json")
            adapter = {"mac": "00:24:27:88:02:3c", "addresses": ["192.168.11.22"]}
            with patch("ulv.network_bootstrap.sys.platform", "darwin"), \
                    patch("ulv.network_bootstrap.os.geteuid", return_value=0), \
                    patch.object(manager, "_interface", return_value=(True, adapter)), \
                    patch.object(manager, "_run", side_effect=[(True, "inet 192.168.11.22"), (False, "failure")]):
                self.assertFalse(manager.apply("en5", "192.168.11.41", "192.168.11.222", 0)[0])
                self.assertTrue(manager.journal_path.exists())
                with patch.object(manager, "_run") as run:
                    self.assertTrue(manager.restore_all()[0])
                    run.assert_not_called()
                    self.assertFalse(manager.journal_path.exists())

    def test_same_subnet_bpearl_arp_triggers_before_any_udp(self):
        requests = []
        capture = PacketCapture(DeviceRegistry(), lambda *_: None,
                                local_addresses=("192.168.11.22",),
                                arp_network_adapt_callback=lambda flow: requests.append(flow))
        request = CapturedArp("40:2c:76:80:2b:10", "ff:ff:ff:ff:ff:ff", 1,
                              "192.168.11.41", "192.168.11.222", (), ())
        for i in range(4):
            capture._handle_arp(0, "USB LAN", "en5", request, (i + 1) * 1_000_000_000)
            if i < 2:
                self.assertFalse(requests)
        self.assertEqual(len(requests), 1)
        self.assertEqual(requests[0]["destination_ip"], "192.168.11.222")
        self.assertEqual(requests[0]["trigger"], "arp")

    def test_unknown_arp_does_not_adapt_same_subnet(self):
        requests = []
        capture = PacketCapture(DeviceRegistry(), lambda *_: None,
                                local_addresses=("192.168.11.22",),
                                arp_network_adapt_callback=lambda flow: requests.append(flow))
        request = CapturedArp("00:24:27:88:02:3c", "ff:ff:ff:ff:ff:ff", 1,
                              "192.168.11.44", "192.168.11.55", (), ())
        for i in range(4):
            capture._handle_arp(0, "USB LAN", "en5", request, i * 1_000_000_000)
        self.assertFalse(requests)


if __name__ == "__main__":
    unittest.main()
