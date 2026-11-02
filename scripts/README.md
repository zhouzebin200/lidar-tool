# 开发辅助脚本

| 阶段 | 工具 |
| --- | --- |
| 获取 SDK | `fetch_sdks.ps1` |
| 适配厂商 SDK | `prepare_hesai_sdk.ps1`、`prepare_seyond_sdk.ps1` |
| 编译 Windows 原生 DLL | `build_native_portable.py`、`build_native_zig.ps1` |
| 验证真实 PCAP | `validate_pcap.py` |

在项目根目录执行这些工具。原生编译细节见 [native/README.md](../native/README.md)，成品发布入口放在 [packaging/](../packaging/README.md)。
