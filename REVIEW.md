# 砺见智行工作室激光雷达测试工具 开发与维护 Review

> 快照日期：2026-09-27  
> 项目版本：v0.2.1  
> 运行目录：`universal-lidar-viewer`

---

## 1. 产品定位与架构目标

本项目是运行在 Windows/x86-64 平台上的**通用激光雷达实时点云查看器与自动化检测报告一体化工作台**（砺见智行工作室激光雷达测试工具）。

核心定位：
1. **单进程一体化**：点云解码、网络发现、标定维护与自动化报告生成在同一进程内协同工作，避免端口争用与网络设备冲突；
2. **多厂商多架构统一抽象**：统一支持 RoboSense、Hesai、Huawei、Velodyne、Seyond 等主流车载与工业级激光雷达；
3. **二层自适应无感接入**：依托 Npcap 自动识别物理网卡、雷达源/目的 IP、端口、802.1Q/QinQ VLAN，支持跨网段 ARP 激活，无需用户手工配置繁琐静态 IP；
4. **全型号自动化检测报告**：内置厂商专属遥测与诊断协议（如禾赛 PTC 1.0 / 2.0），自动采集供电、电气、多通道温度、内部湿度、固件版本、运行机构指标并导出结构化检测报告。

---

## 2. 当前核心能力矩阵

### 2.1 网络层与服务鲁棒性
- **二层嗅探与过滤**：Npcap 自动遍历宿主机物理网卡，实时解析以太网帧、VLAN 及 QinQ 头部；
- **自适应网络配置**：需要跨网段或广播诱发流时，自动写入临时 IPv4 / VLAN 并具备完备的恢复机制；未安装 Npcap 时自动降级为全端口 UDP Socket 通配监听；
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
*当前工程包含 78 项自动化测试（覆盖各厂商解码、多型号 PTC 模拟服务端、边界条件与安全认证），必须保证 100% 全部通过。*

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
