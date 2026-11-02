# 2026-10-23 E2 查看器接入

基线 main 34abaaa；仅修改网页查看器的速腾解码链路，ROS 保持原样。

验证：10 项 C++ ASAN/UBSAN 场景通过，Python 共 91 项，88 项通过，3 项依赖未安装的禾赛/Seyond Mac 插件，显式跳过。5 项新增 Python 测试全部通过。合成 PCAP 1152 包输出 2 帧、每帧 67784 点，无解码错误或丢包。未验证真实 E2 抓包或 Windows 构建。独立审查发现的重启帧号恢复问题已修复并复审。

macOS 本地插件 SHA256：`0bffb629feccae6250ec74d8ff5d4412f6d37beba8b9bbd87e8c7e96582b9d34`。使用步骤见 [E2_VIEWER.md](docs/E2_VIEWER.md)，详情见 [E2_VALIDATION.json](docs/E2_VALIDATION.json)。

# 砺见智行工作室激光雷达测试工具 开发与维护 Review

> 快照日期：2026-09-30  
> 项目版本：v0.2.1；包含 2026-09-30 未发布更新  
> 运行目录：`universal-lidar-viewer`

---

## 2026-09-30 本次改动 Review：macOS 网络自适应

审阅对象为源码提交 [f913f51](https://github.com/zhouzebin200/universal-lidar-viewer/commit/f913f51d9b063f79ff04060c80f05f277fb64455)。以下结论来自代码核查、自动化测试和本机打包检查，未包含离线雷达的实机验收。

### 问题与行为变化

雷达的目标 IP 与电脑地址不一致时，雷达可能持续请求目标 IP 的 ARP 解析，导致 MSOP/DIFOP UDP 数据无法开始发送。原来的自动网络配置仅覆盖 Windows；此次增加 macOS 的临时 IP 适配，使电脑可以响应雷达请求的地址。

这次解决的是接收前的网络条件，没有修改 RoboSense DIFOP 的字段布局或解码算法。界面状态字段是否正确，仍需要真实数据验证。

### 文件与核查重点

| 文件 | 改动及核查重点 |
| --- | --- |
| `ulv/network_bootstrap.py` | 管理员启动、物理有线网卡识别、添加 IPv4 alias、先写恢复记录再修改网卡；恢复时核对网卡 MAC，只移除本程序添加的地址。 |
| `ulv/capture.py` | 独立的 ARP 适配入口；重复请求、私有或链路本地地址、同一 /24、源过滤及设备 MAC 条件；未知 ARP 不触发修改。 |
| `ulv/server.py` | 选择 macOS 管理器，接入 UDP/ARP 适配回调，处理广播目标地址，退出时等待适配线程完成后恢复。 |
| `tests/test_macos_network_bootstrap.py` | 8 项用例覆盖地址所有权、恢复、权限及 VLAN 拒绝、无线网卡拒绝、网卡拔出或替换、失败恢复和 ARP 引导。 |
| `tests/test_network_bootstrap.py` | Windows 回退测试显式模拟 Windows 平台，避免受运行测试的宿主平台影响。 |
| `tests/test_no_network_mutation.py` | macOS BUNDLE 打包配置不再被要求包含 Windows UAC 字段。 |
| `packaging/build_macos.py` | 收集资源和现有 dylib 插件，构建 .app、临时签名及拖拽安装 DMG；脚本不编译缺失的厂商插件。 |

### 已完成验证

- 54 项相关测试通过，包括网络管理、抓包、协议识别、PCAP 回放及本机已有的部分原生插件用例。
- macOS arm64 应用构建成功；应用签名完整性检查及 DMG 校验通过。使用的是 ad-hoc 临时签名，没有完成 Developer ID 签名及公证。
- 打包程序在关闭提权的模式下接收 3 个合成、1248 字节 DIFOP UDP 包，RoboSense 插件分发调用计数为 3；程序正常退出。
- 本机缺少 Hesai/Seyond 原生插件，扩大原生插件测试范围时出现 3 项相关失败。因此不能将本次 54 项结果描述为全量测试通过。

复现本次测试（Python 环境及对应原生插件须就绪）：

```sh
python -m unittest \
  tests.test_macos_network_bootstrap \
  tests.test_network_bootstrap \
  tests.test_capture \
  tests.test_no_network_mutation \
  tests.test_detection \
  tests.test_pcap_replay \
  tests.test_native_plugins.NativePluginTests.test_velodyne_vlp16_decodes_complete_revolution \
  tests.test_native_plugins.NativePluginTests.test_huawei_l306_uses_demo_packet_layout_scale_and_calibration
```

macOS 构建（需安装 PyInstaller，并先准备对应架构的插件）：

```sh
python packaging/build_macos.py
```

### 限制与待实机验收

- 自动配置当前限于无 VLAN 的物理 USB/以太网接口，使用 /24 地址；无线网卡和 VLAN 自动配置不在本次支持范围内。
- 首次接入时，RoboSense MAC 前缀是设备识别线索，不是身份认证；已识别设备可使用其缓存 MAC 触发适配。
- 管理员授权与实际网卡地址增删尚未完成本次实机验证。雷达恢复连接后，应检查：目标地址不一致时能否自动收到 MSOP/DIFOP；真实 DIFOP 状态字段；正常退出后临时地址清理；异常退出后的恢复；拔插原 USB 网卡后的恢复记录处理。
- 验收时同时核对原有 IP 和 DHCP 保持正常，并记录真实数据包、界面状态及恢复结果。

---

## 1. 产品定位与架构目标

本项目支持 Windows/x86-64 和 macOS/arm64，是**通用激光雷达实时点云查看器与自动化检测报告一体化工作台**（砺见智行工作室激光雷达测试工具）。

核心定位：
1. **单进程一体化**：点云解码、网络发现、标定维护与自动化报告生成在同一进程内协同工作，避免端口争用与网络设备冲突；
2. **多厂商多架构统一抽象**：统一支持 RoboSense、Hesai、Huawei、Velodyne、Seyond 等主流车载与工业级激光雷达；
3. **二层自适应无感接入**：Windows 依托 Npcap 自动识别物理网卡、雷达源/目的 IP、端口、802.1Q/QinQ VLAN，支持跨网段 ARP 激活，无需用户手工配置繁琐静态 IP；
4. **全型号自动化检测报告**：内置厂商专属遥测与诊断协议（如禾赛 PTC 1.0 / 2.0），自动采集供电、电气、多通道温度、内部湿度、固件版本、运行机构指标并导出结构化检测报告。

---

## 2. 当前核心能力矩阵

### 2.1 网络层与服务鲁棒性
- **二层嗅探与过滤**：Npcap/libpcap 自动遍历宿主机物理网卡，实时解析以太网帧、VLAN 及 QinQ 头部；
- **自适应网络配置**：Windows 支持临时 IPv4 / VLAN 配置；macOS 支持无 VLAN 直连的 IPv4 alias 配置。两者使用恢复记录；抓包不可用时 UDP Socket 按配置的端口监听；
- **端口冲突自愈（WinError 10013 消除）**：深度规避 Windows Hyper-V / WSL2 / winnat 动态随机保留端口范围。默认端口 17777 遭遇权限限制时，自动平滑回退探测 `18888`、`27777`、`37777`，杜绝启动闪退。

### 2.2 点云解码与标定
- **原生 ABI v2 高性能插件**：核心计算下沉至 C++ 原生 DLL 动态库（`plugins/libulv_<vendor>.dll`），采用紧凑 20 字节点结构直接送入 WebGL2 显存；
- **设备工厂标定优先**：优先自雷达内部获取出厂标定参数（如 PTC 标定、AngleHV 表），在无法读取时平滑降级至标称修正表（`corrections/`）。

### 2.3 禾赛（Hesai）全系列自动化报告与协议引擎（2026-09-27 重大升级）
| 型号类别 | 代表型号 | 协议栈支持 | 物理架构与遥测特性 |
|---|---|---|---|
| **混合固态主雷达** | AT128 / AT128P / ATX / ATXP / ETX | PTC 1.0 ("Gt") & PTC 2.0 ("Wu") | 电压、电流、整机功耗、9 通道激光/主控测温、转镜转速 |
| **纯固态 Flash 补盲** | FT120 / FTX | PTC 1.0 / 2.0 | 面阵纯固态 0-RPM 感知、跳过 0x27 电气指令、内部相对湿度、0x06 PTP 纳秒级授时诊断 |
| **全景机械式雷达** | OT128 / AT360 / Pandar40 / XT32 | PTC 1.0 / 2.0 | 228 字节专属设备字典、8 通道温度、内部相对湿度、马达实时 RPM 与闭环状态 |
| **超轻量化系列** | JT128 / JT16 / JT32 / MT | PTC 1.0 / 2.0 | 协议 Schema 动态映射、通道线数、固件版本自适应读取 |

---

## 3. 核心数据流

```text
[物理网络 / PCAP 文件]
         │
         ▼
    ulv/capture.py (Npcap 二层抓包 / UDP 通配回退)
         │
         ▼
   ulv/detection.py (厂商 / 协议 / 包类型识别)
         │
         ▼
    ulv/registry.py (物理设备聚合 / MAC 与端口会话跟踪)
    ┌────┴──────────────────────────┐
    ▼                               ▼
[点云数据流]                   [检测与控制流]
ulv/native_plugins.py          ulv/hesai_ptc.py (PTC 客户端)
plugins/libulv_<vendor>.dll     config/varID/*.csv (寄存器字典)
    │                               │
    ▼                               ▼
WebSocket (/ws/points)          HTTP API (/api/report/*)
    │                               │
    ▼                               ▼
web/app.js (WebGL2 画布渲染)    web/ 报告工作台 (PDF/Markdown 导出)
```

---

## 4. 关键源码与资产索引

- `run.py`：开发与控制台启动入口；
- `ulv/server.py`：单进程一体化服务器，集成 HTTP / WebSocket / 报告 API，内置端口冲突回退逻辑；
- `ulv/hesai_ptc.py`：禾赛全系列 PTC 通信引擎，双协议栈报头编解码，物理架构感知检测报告生成器；
- `config/varID/`：禾赛 13 款主流雷达协议寄存器字典（CSV 格式）与 `auth_config.csv` 挑战应答认证表；
- `ulv/capture.py`：Npcap 二层嗅探与 ARP 流激活机制；
- `ulv/detection.py`：多厂商数据包特征与指纹识别；
- `ulv/registry.py`：多设备聚合管理器；
- `ulv/native_plugins.py`：原生解码 DLL 调度器；
- `packaging/build_report.py`：当前单文件 EXE（包含所有依赖 DLL 与 config 资源）的权威打包脚本；
- `dist/砺见智行工作室激光雷达测试工具.exe`：当前交付给最终用户的 Windows x64 便携程序。

---

## 5. 运行、测试与构建验收

### 5.1 运行服务
```powershell
python run.py
```
*自动监听本机网卡流量并绑定空闲端口（默认 17777，遇冲突自动切换至 18888）。*

### 5.2 执行全量单元测试
```powershell
python -m unittest discover -s tests -v
```
*2026-09-27 的历史验收记录为 78 项测试通过；测试数量会随新增用例变化。本次 macOS 验证范围和缺失插件导致的失败见上方 Review，全量验收应在插件齐全的环境执行。*

### 5.3 重新构建单文件分发包
```powershell
python packaging/build_report.py
```
*构建产物输出至 `dist/砺见智行工作室激光雷达测试工具.exe` 以及配套的 zip 压缩包。*

---

## 6. 后续维护与接手规范

1. **协议改动回归**：任何针对 `ulv/hesai_ptc.py` 或解码插件的变更，必须首先运行 `tests/test_hesai_ptc.py` 及全量测试套件；
2. **字典驱动优先**：新增或修改禾赛雷达型号时，优先更新 `config/varID/<MODEL>.csv`，保持数据结构与固件版本解耦；
3. **架构感知保护**：对纯固态 Flash 雷达（如 FT120/FTX）不可强行下发电机驱动或不存在的电气监控指令，避免引发设备异常保护；
4. **单文件打包资产核对**：若新增前端静态资源或配置目录，必须在 `packaging/build_report.py` 的 `datas` 列表中登记，确保打包生成的单文件 EXE 在脱离源码目录时依然能够正常加载。
