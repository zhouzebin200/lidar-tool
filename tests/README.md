# 测试

从项目根目录执行。Python 测试保留在 `tests/`，保证标准 unittest 发现命令不变。

```powershell
python -m unittest discover -s tests -v
```

完整的 Windows DLL 测试应在 Windows x64 Python 下运行；macOS 不能加载仓库中的 Windows DLL。Windows 网卡脚本测试也包含平台条件，不能将 macOS 全套结果直接视为 Windows 部署结果。

| 范围 | 文件 |
| --- | --- |
| 收包与设备识别 | `test_capture.py`、`test_detection.py`、`test_protocol.py` |
| 原生解码与回放 | `test_native_plugins.py`、`test_pcap_replay.py` |
| 网络与网关 | `test_network_bootstrap.py`、`test_no_network_mutation.py`、`test_gateway.py`、`test_ptp.py`、`test_raw_ptc.py` |
| 报告和设备查询 | `test_hesai_ptc.py`、`test_lidar_web_scraper.py` |
| 授权与发布 | `test_licensing.py`、`test_license_runtime.py`、`test_private_release.py`、`test_release_audit.py`、`test_windows_release.py`、`test_npcap_bootstrap.py` |
| 浏览器行为 | `browser/` |
| 人工检查工具 | `manual/`，使用合成数据，非真实雷达 |

浏览器回归：

```sh
node tests/browser/device_selection_check.js
node tests/browser/report_device_switch_check.js
```

第二项需要 Node.js、Playwright 和 Chrome。手动查看合成点云与报告：

```sh
python tests/manual/report_integration_harness.py
```

该工具监听 `http://127.0.0.1:17889`，结束时按 Ctrl+C。测试日志放到 `build/`，需要保留的验收证据放到 `docs/verification-日期/`，真实 PCAP 不提交到仓库。
