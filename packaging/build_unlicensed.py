"""Build the explicitly unlicensed Windows x64 single executable."""
from pathlib import Path
import hashlib
import os
import sysconfig
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from audit_public_release import audit
from build_report import validate_plugins

ROOT = Path(__file__).resolve().parents[1]
NAME = 'Lijian-LidarTool-Windows-x64'


def main():
    if sys.platform != 'win32' or sysconfig.get_platform() != 'win-amd64':
        raise RuntimeError('Build with Windows x64 Python')
    validate_plugins(ROOT / 'plugins')
    from ulv.npcap_bootstrap import INSTALLER_SHA256
    installer = ROOT / 'vendor/npcap/npcap-1.90.exe'
    if not installer.is_file() or hashlib.sha256(installer.read_bytes()).hexdigest() != INSTALLER_SHA256:
        raise RuntimeError('Official Npcap 1.90 installer missing or checksum mismatch')
    output = ROOT / 'build' / 'unlicensed-onefile'
    destination = ROOT / 'dist' / 'unlicensed-x64'
    if destination.exists():
        shutil.rmtree(destination)
    if output.exists():
        shutil.rmtree(output)
    command = [sys.executable, '-m', 'nuitka', '--mode=onefile', '--mingw64',
               '--assume-yes-for-downloads', '--windows-console-mode=disable', '--windows-uac-admin',
               '--include-package=ulv', '--jobs=4',
               f'--output-dir={output}', f'--output-filename={NAME}.exe',
               f'--report={output / "compilation-report.xml"}',
               f'--user-package-configuration-file={ROOT / "packaging/ulv.nuitka-package.config.yml"}',
               f'--include-data-dir={ROOT / "web"}=web',
               f'--include-data-dir={ROOT / "corrections"}=corrections',
               '--nofollow-import-to=unittest,setuptools,licensing,tests', str(ROOT / 'run.py')]
    # Nuitka normally excludes .bin as potential code; these are calibration tables.
    for path in (ROOT / 'corrections').rglob('*.bin'):
        command.insert(-1, f'--include-data-files={path}={path.relative_to(ROOT).as_posix()}')
    environment = os.environ.copy()
    environment.setdefault('NUITKA_CACHE_DIR', str(Path.home() / '.cache' / 'nuitka'))
    subprocess.run(command, cwd=ROOT, env=environment, check=True)
    problems = audit(output / 'run.dist')
    if problems:
        raise RuntimeError('\n'.join(problems))
    destination.mkdir(parents=True)
    executable = destination / f'{NAME}.exe'
    shutil.copy2(output / executable.name, executable)
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    executable.with_suffix('.exe.sha256').write_text(f'{digest}  {executable.name}\n', encoding='ascii')
    problems = audit(destination)
    if problems:
        raise RuntimeError('\n'.join(problems))
    print(executable)


if __name__ == '__main__':
    main()
