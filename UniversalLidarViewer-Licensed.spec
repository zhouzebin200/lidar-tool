# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['C:/Users/zzb/Documents/Playground/universal-lidar-viewer/run_licensed.py'],
    pathex=[],
    binaries=[('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/plugins/libulv_hesai.dll', 'plugins'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/plugins/libulv_robosense.dll', 'plugins'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/plugins/libulv_seyond.dll', 'plugins'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/plugins/libulv_velodyne.dll', 'plugins')],
    datas=[('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/web', 'web'), ('C:/Users/zzb/Documents/Playground/universal-lidar-viewer/corrections', 'corrections')],
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
    name='UniversalLidarViewer-Licensed',
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
    uac_admin=True,
)
