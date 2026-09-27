"""Build the original viewer with the integrated report workbench."""
from pathlib import Path
import subprocess
import sys
import os

root = Path(__file__).resolve().parents[1]
command = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--onefile', '--windowed', '--uac-admin', '--name', '砺见智行工作室激光雷达测试工具']
for directory in ('web', 'corrections', 'config'):
    command += ['--add-data', str(root / directory) + os.pathsep + directory]
for plugin in (root/'plugins').glob('*.dll'):
    command += ['--add-binary', str(plugin) + os.pathsep + 'plugins']
command += [str(root / 'run.py')]
subprocess.run(command, cwd=root, check=True)
