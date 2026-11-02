# 发布构建

| 入口 | 用途 | 输出 |
| --- | --- | --- |
| `build_unlicensed.py` | 当前无授权 Windows x64 单 EXE，内置离线 Npcap 向导 | `dist/unlicensed-x64/` |
| `build_report.py` | 融合授权版，解压目录运行 | `dist/customer/`，按需构建，目前没有保留旧包 |
| `build_openwrt_gateway.py` | OpenWrt 网关 IPK | 由脚本参数决定 |
| `audit_public_release.py` | 检查交付目录是否混入源码、调试文件或私密材料 | 审计结果 |
| `make_release.py` | 兼容旧的一键命令；主入口转到 `build_unlicensed.py` | 同当前无授权版 |
| `legacy/` | 历史 PyInstaller、多版本和 BAT 生成工具 | 仅维护历史场景使用 |

在项目根目录、Windows x64 Python 环境执行：

```powershell
python -m pip install nuitka==4.3 cryptography ordered-set zstandard
python packaging/build_unlicensed.py
python packaging/audit_public_release.py dist/unlicensed-x64
```

构建前需要 `plugins/` 中的五厂商 x64 DLL，以及 `vendor/npcap/npcap-1.90.exe`。构建入口会检查必需 DLL 和安装器 SHA-256。DLL 编译方法见 [原生适配器说明](../native/README.md)，Npcap 来源见 [依赖记录](../vendor/npcap/README.md)。

构建不会覆盖同名交付目录。确认旧版本不再需要后再清理旧输出。编译缓存和中间文件放 `build/`，最终 EXE、说明、哈希放 `dist/`；禁止把整个项目目录作为安装包。
