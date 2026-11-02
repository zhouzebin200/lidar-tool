from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from ulv import npcap_bootstrap as n

class NpcapBootstrapTests(unittest.TestCase):
    def test_accepts_current_and_newer_running_installation(self):
        for version in ('1.79', '1.88', '1.90', '1.91', '2.0'):
            self.assertTrue(n.is_usable({'version': version, 'dll_present': True, 'running': True}))
        for value in ({'version':'0.99','dll_present':True,'running':True}, {'version':'1.90','dll_present':False,'running':True}, {'version':'1.90','dll_present':True,'running':False}, {}):
            self.assertFalse(n.is_usable(value))

    def test_usable_installation_never_runs_installer(self):
        with patch.object(n.sys,'platform','win32'), patch.object(n,'probe',return_value={'version':'1.90','dll_present':True,'running':True}), patch.object(n.subprocess,'run') as run:
            self.assertTrue(n.ensure_npcap(Path('/missing')))
            run.assert_not_called()

    def test_invalid_installer_is_not_executed(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);p=root/'vendor/npcap/npcap-1.90.exe';p.parent.mkdir(parents=True);p.write_bytes(b'not the installer')
            with patch.object(n.sys,'platform','win32'), patch.object(n,'probe',return_value={}), patch.object(n,'notify'), patch.object(n.subprocess,'run') as run:
                self.assertFalse(n.ensure_npcap(root));run.assert_not_called()

    def test_install_is_interactive_and_rechecks(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);p=root/'vendor/npcap/npcap-1.90.exe';p.parent.mkdir(parents=True);p.write_bytes(b'fixture')
            import hashlib
            ready={'version':'1.90','dll_present':True,'running':True}
            with patch.object(n.sys,'platform','win32'), patch.object(n,'INSTALLER_SHA256',hashlib.sha256(b'fixture').hexdigest()), patch.object(n,'probe',side_effect=[{},ready]), patch.object(n.subprocess,'run') as run:
                run.return_value.returncode=0
                self.assertTrue(n.ensure_npcap(root))
                self.assertEqual(run.call_args.args[0],[str(p),'/no_kill=yes'])

    def test_cancel_and_reboot_do_not_loop_or_exit_application(self):
        import hashlib
        for code in (1, 3010, 350, 1618):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                root=Path(directory);p=root/'vendor/npcap/npcap-1.90.exe';p.parent.mkdir(parents=True);p.write_bytes(b'fixture')
                with patch.object(n.sys,'platform','win32'), patch.object(n,'INSTALLER_SHA256',hashlib.sha256(b'fixture').hexdigest()), patch.object(n,'probe',return_value={}), patch.object(n,'notify') as notify, patch.object(n.subprocess,'run') as run:
                    run.return_value.returncode=code
                    self.assertFalse(n.ensure_npcap(root))
                    self.assertEqual(run.call_count,1)
                    notify.assert_called_once()

    def test_healthy_version_with_broken_driver_is_not_replaced(self):
        for version in ('1.88', '1.90', '1.91', '2.0'):
            with self.subTest(version=version), patch.object(n.sys,'platform','win32'), patch.object(n,'probe',return_value={'version':version,'dll_present':True,'running':False}), patch.object(n,'notify'), patch.object(n.subprocess,'run') as run:
                self.assertFalse(n.ensure_npcap(Path('/missing')))
                run.assert_not_called()
