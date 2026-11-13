# 构建与验证脚本

| 脚本 | 用途 |
| --- | --- |
| `build_all.sh` | 一键构建 macOS 原生应用（`.app` + `.dylib`）与 Windows x64 绿色包（Zig 交叉编译 `.exe` + `.dll`） |
| `run_e2e_tests.sh` | 构建并运行 C++ 单元测试、无头 UI 测试及 PCAP 回归验证 |
| `fetch_sdks.ps1` | 在缺少 `third_party/` 时拉取第三方雷达 SDK 源码 |
| `prepare_hesai_sdk.ps1` / `prepare_seyond_sdk.ps1` | 第三方 SDK 初始化适配脚本 |

在项目根目录执行，如 `./scripts/build_all.sh all`。
