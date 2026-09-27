# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['C:/Users/zzb/Documents/Playground/universal-lidar-viewer/report_app.py'],
    pathex=[],
    binaries=[],
    datas=[('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/web/report.html', 'web'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/web/report.css', 'web'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/web/report.js', 'web'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/web/report-assessment.js', 'web'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/web/report-export.js', 'web')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='LidarReportStudio',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
