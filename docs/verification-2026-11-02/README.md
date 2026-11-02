# 2026-11-02 验证证据

当前交付为带 Npcap 离线安装向导的无授权单文件 EXE。下表优先查看最新记录，旧日志保留为历史测试证据；旧 EXE 和压缩包已删除。

| 最新验收 | 文件 |
| --- | --- |
| Windows x64 全套测试，101 项通过 | `npcap-101-tests.txt` |
| 内嵌安装器哈希、最终 EXE 页面和插件验证 | `npcap-single-exe-smoke.json` |
| 最终 EXE 对真实 111.pcap 的解码 | `npcap-111-offline.json` |
| 最终构建日志 | `build-npcap.log` |
| 111.pcap 帧间隔与几何统计 | `111-frame-probe.json`、[测试说明](111-pcap测试结果.md) |

其他 `windows-x64-*.txt`、`single-exe-smoke.json`、`build-unlicensed.log` 和无授权单文件发布结果文档属于加入 Npcap 前的测试快照。`macos-tests.txt` 包含因平台差异产生的失败，不是当前 Windows 验收结论。

PD 的 Windows 为 ARM64，通过仿真执行 x64 程序。这些证据不替代物理 Windows x64 网卡、真实雷达或驱动安装完成后的现场验收。Npcap 安装器签名有效，安装分支已做模拟测试；本轮没有实际升级 PD 中的驱动。
