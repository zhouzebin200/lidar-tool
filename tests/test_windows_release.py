import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'packaging'))
import build_report
import make_release
import build_unlicensed

class WindowsReleaseTests(unittest.TestCase):
    def test_rejects_missing_decoders(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, 'missing'):
                build_report.validate_plugins(Path(directory))

    def test_rejects_non_x64_decoders(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for vendor in ('robosense', 'hesai', 'huawei_l306', 'velodyne', 'seyond'):
                data = bytearray(128)
                data[:2] = b'MZ'
                struct.pack_into('<I', data, 60, 64)
                data[64:68] = b'PE\0\0'
                struct.pack_into('<H', data, 68, 0xAA64)
                (root / f'libulv_{vendor}.dll').write_bytes(data)
            with self.assertRaisesRegex(RuntimeError, 'x64'):
                build_report.validate_plugins(root)

    def test_native_build_failure_stops_release(self):
        import subprocess
        with patch.object(make_release.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'native')):
            with self.assertRaises(subprocess.CalledProcessError):
                make_release.build_native_plugins()

    def test_single_file_uses_unlicensed_entry_and_admin_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            import shutil
            (root / 'vendor/npcap').mkdir(parents=True)
            shutil.copy2(ROOT / 'vendor/npcap/npcap-1.90.exe', root / 'vendor/npcap/npcap-1.90.exe')
            with patch.object(build_unlicensed, 'ROOT', root), \
                 patch.object(build_unlicensed.sys, 'platform', 'win32'), \
                 patch.object(build_unlicensed.sysconfig, 'get_platform', return_value='win-amd64'), \
                 patch.object(build_unlicensed, 'validate_plugins'), \
                 patch.object(build_unlicensed.subprocess, 'run', side_effect=RuntimeError('compile boundary')) as run:
                with self.assertRaisesRegex(RuntimeError, 'compile boundary'):
                    build_unlicensed.main()
            command = run.call_args.args[0]
            self.assertIn('--mode=onefile', command)
            self.assertIn('--windows-uac-admin', command)
            self.assertEqual(command[-1], str(root / 'run.py'))
