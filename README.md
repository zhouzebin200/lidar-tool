# 砺见智行激光雷达测试工具

[更新说明](docs/history/CHANGELOG.md) · [开发与维护 Review](docs/history/REVIEW-2026-09-27.md)

面向 RoboSense、Hesai、Huawei、Velodyne 和 Seyond/Innovusion 雷达的跨平台实时点云查看器，集成检测报告工作台。支持 Windows x64 独立免安装运行、macOS 自动网卡自适应与 PCAP 回放，并提供独立 ROS1/ROS2 工作区。

## 直接使用

当前成品：[`dist/unlicensed-x64/Lijian-LidarTool-Windows-x64.exe`](dist/unlicensed-x64/Lijian-LidarTool-Windows-x64.exe)。

复制这一个 EXE 到 Windows 10/11 x64 电脑，双击并允许管理员权限。无需 Python、无需软件许可证；已内置网页、五厂商解码器、标定文件和 Npcap 1.90 离线安装向导。Npcap 安装仍需用户确认其官方协议，检测与安装过程不需要联网下载。旧发布包已删除。

## 从哪里开始

| 需要做什么 | 入口 |
| --- | --- |
| 运行源码 | `run.py` 或 `start.ps1` / `start.sh` |
| 构建当前无授权 EXE | `python packaging/build_unlicensed.py` |
| 构建授权版 | `python packaging/build_report.py` |
| 了解代码和维护方式 | [项目结构与维护](docs/项目结构与维护.md) |
| 查阅使用说明、配置和 API | [完整使用说明](docs/guides/完整使用说明.md) |
| 运行测试 | [测试说明](tests/README.md) |
| 查阅全部文档 | [文档目录](docs/README.md) |

## 目录分工

```text
ulv/          Python 服务、抓包、协议、设备和运行时逻辑
web/          点云、报告、网关的浏览器界面
native/       C++ 解码适配器源码和 C ABI
corrections/  随软件提供的标称标定
config/       静态型号配置与本机运行配置
plugins/      编译后的解码 DLL（生成物）
third_party/  厂商 SDK 源码（本地依赖）
vendor/       离线安装器等厂商二进制依赖
licensing/    授权管理源码和独立保管的私有数据
packaging/    发布构建、内容审计与 legacy/ 历史工具
scripts/      SDK 获取、原生编译、PCAP 验证工具
tests/        Python 回归；browser/ 浏览器测试；manual/ 手动工具
docs/         指南、设计、维护、历史记录和测试证据
build/        中间产物和测试临时文件（不交付）
dist/         当前交付文件（不存旧版本）
tmp/          本地临时文件（不交付）
```

## 跨平台运行

Linux/macOS：

```sh
./start.sh
```

Windows：

```powershell
.\start.ps1
```

启动后访问 <http://127.0.0.1:17777>。真实雷达流量到达已配置的监听端口后会自动识别并显示。

发布给最终用户时可构建当前融合版单文件程序，目标电脑不需要 Python：

```powershell
python -m pip install pyinstaller
python packaging/build_report.py
```

默认生成 `dist/砺见智行工作室激光雷达测试工具.exe`。网页、修正文件和当前 `plugins` 目录中的厂商解码 DLL 均嵌入 EXE。Windows 包应在 Windows 上构建；源码运行仍使用上面的 `start.ps1`。

`packaging/build_portable.py` 是另一个历史构建流程，会另外生成普通版、一机一码授权版和私有发码工具，不是当前报告融合版的发布入口：

- `dist/UniversalLidarViewer-portable/UniversalLidarViewer.exe`：历史普通版，不检查许可证；
- `dist/UniversalLidarViewer-licensed/UniversalLidarViewer-Licensed.exe`：历史授权版；
- `dist/ULV-LicenseAdmin-private/ULV-LicenseAdmin.exe`：私有许可证签发工具，内含 Ed25519 私钥，严禁交给最终用户。

授权版根据 Windows MachineGuid 与系统卷序列号生成机器码。许可证由离线私钥签名，EXE 只保存公钥；签名篡改或复制到不同机器均会验证失败，整个启动和验证过程不访问互联网。首次运行会生成 `machine-code.txt`，管理员据此签发 `license.key`，最终用户把文件放在 EXE 同目录即可。重装系统或更换系统盘可能导致机器码变化。

## 网络配置

默认启用“自动抓包识别”：程序通过 Npcap（Windows）或 libpcap（Linux/macOS）在二层分析所有网卡上的少量流式报文，自动取得雷达源 IP、目标/监听 IP、UDP 源/目标端口以及 802.1Q/QinQ VLAN ID。识别后的原始 UDP 载荷直接送入厂商 SDK，因此目标端口不是默认值时也能解码。

Windows 版启动时申请一次管理员权限。自动模式从点云流取得雷达 IP、入口物理网卡和 VLAN；若目标是组播或广播地址，则在雷达 `/24` 网段选择 `.100` 作为临时本机地址。程序先把网卡原 IPv4、DHCP、VLAN ID 和“优先级/VLAN”开关写入恢复日志，再设置临时地址和 VLAN。网卡重启后抓包与 SDK 自动恢复，普通 PTC TCP 9347 可直接读取报告数据。

macOS 版同样在启动时请求管理员授权。无 VLAN 的 USB/以太网直连可根据已识别的 UDP 流添加目标 `/24` 临时 IP，保留原有 IP 和 DHCP 设置。没有 UDP 时，连续三次来自已识别雷达 MAC（或 RoboSense `40:2c:76` 前缀）的 ARP 请求也能启动适配。例如本机是 `192.168.11.22`、Bpearl 请求 `192.168.11.222`，程序为对应 USB 网卡增加 `.222`，随后通过 libpcap 接收任意 MSOP/DIFOP 端口。未知设备的 ARP 请求不会触发地址修改；自动创建 VLAN 接口暂未支持。

macOS 临时地址在操作前写入恢复记录，正常退出时只删除本程序添加的地址；异常退出后，下次启动先恢复。USB 网卡拔出时保留恢复记录，待原网卡重新接入后恢复，避免删除其他网卡的地址。`--no-elevation` 同时关闭 Windows/macOS 的管理员授权和网络适配。

正常退出或关闭最后一个查看器网页时，程序删除临时 IP并恢复原 VLAN、优先级开关和 DHCP状态。若进程被强制结束，恢复日志会保留；下次启动先恢复上一次配置，再开始新的自动识别。`--no-elevation` 可用于开发和离线场景，关闭网卡自适应。

如果系统没有抓包运行库或当前用户缺少抓包权限，程序自动降级为普通 UDP 通配监听：绑定 `0.0.0.0`，覆盖所有本机 IPv4 网卡，并监听四个解码器的标准在线端口：Hesai `2368/10110`，RoboSense `6699/7788/7766/6688`（EMX DIFOP2 默认为 `7766`），Velodyne 数据/位置包 `2368/8308`，Seyond/Innovusion 数据包 `8010`。网页左侧会明确显示当前是 `Npcap/libpcap` 还是 `socket fallback`。

右侧 `UDP INPUT` 面板可关闭自动监听，切换为手动配置并持久化：

- 本机监听 IP，例如 `0.0.0.0` 或网卡地址 `192.168.1.100`；
- 任意数量的 UDP 端口，以逗号分隔；
- 可选雷达源 IP 或 CIDR 白名单，例如 `192.168.1.201, 10.0.0.0/24`。

点击“应用监听配置”后监听器立即重启。配置写入 `config/runtime.json`。HTTP 服务地址单独使用 `--host` 和 `--port` 指定，避免网页在运行中修改自己的地址后失联。

命令行提供 `--listen-ip` 或 `--udp-ports` 时自动进入手动模式。雷达若使用非标准目标端口，也应在手动模式中填写该端口。

```powershell
python run.py --host 0.0.0.0 --port 17777 --listen-ip 192.168.1.100 --udp-ports 2368,6699,7788
```

### VLAN

自动模式可解析一层 802.1Q 和多层 QinQ 标签；若网卡硬件已经剥离 VLAN 标签，程序仍能接收点云，但只能报告为 `untagged`。Windows 二层抓包需要 Npcap 驱动，并通过 UAC 获得临时网卡配置权限；Linux 通常需要 root 或 `CAP_NET_RAW`，macOS 需要 libpcap/BPF 访问权限。没有这些条件时标准 UDP 降级仍可使用已由操作系统接收的 VLAN 流量。

### PCAP 导入与回放

网页右侧的“PCAP REPLAY”区域可直接选择经典 Ethernet `.pcap` 文件。文件上传到本机服务后按原始抓包时间间隔送入与在线雷达完全相同的自动识别、厂商 SDK、标定和点云渲染链路；默认循环回放，适合持续观察只有几秒钟的测试包，也可以关闭循环或点击“停止”。页面显示当前进度、循环次数和已送入 SDK 的雷达包数。

导入文件只写入操作系统临时目录，停止回放、导入另一文件或关闭软件后自动删除临时副本，不修改也不删除用户选择的源 PCAP。当前支持 classic PCAP 的 Ethernet link type 1；PCAPNG 会给出明确提示。

## 当前能力

- Hesai `EE FF` 协议 major/minor 识别，包括 AT、QT、XT、FT、JT 和 Pandar/OT 型号族；
- Huawei L306/D3 的 1021 字节、192 通道数据协议识别与原生解码；识别时忽略示例源码未定义的前置计数域，使用 `demo_ws_192` 内置的 4 组水平角查表修正、4 组逐通道垂直角修正和基础垂直角表；
- RoboSense MSOP/DIFOP magic 识别，以及 RSM1、RSM2、RSE1、RSEMX 等长度签名；RSM2 的 8 字节网络传输封装和 EMX 的“32 位序号 + 32 位内包长度”封装会自动校验、剥离，被封装的 DIFOP 标识会转换为 `rs_driver` 所需格式；
- Velodyne 标准 1206 字节 firing block 校验与 Product ID 自动识别，支持 VLP-16/Puck LITE/Hi-Res、VLP-32C、HDL-32E、HDL-64E S3 和 VLS-128；
- Seyond/Innovusion `0x176A` 数据包与 `0x186B` 状态包校验，按协议型号字段识别 Falcon-K1/K2/K24、Robin-W/E/E1X/E2/E2X 和 HB-D1；
- 对包头不能唯一确定的 RoboSense 机械式型号，自动并行探测候选 SDK 解码器，并在第一帧有效点云后锁定；
- 二层自动模式按厂商和源 MAC 聚合同一雷达的 MSOP、DIFOP、IMU 等多端口数据，雷达更换 IP 网段后仍保持设备身份和标定缓存；
- 原生 WebGL2 点云渲染、轨道相机，以及自动归一化的高度 Turbo、强度灰度、距离 Turbo、扫描线分类着色；
- 固定长度二进制点云协议；
- 状态 API、监听器错误反馈和重复实例绑定保护；
- 不依赖 ROS、PCL、Node.js 或前端 CDN。

## SDK 接入状态

UDP 发现与型号候选识别已经工作。真实点云解码的稳定 C ABI 位于 `native/include/ulv_decoder_plugin.h`，主程序会自动加载 `plugins` 目录中的动态库。

- `native/plugins/robosense_adapter.cpp` 已接入 `rs_driver` 的 `RAW_PACKET` 和点云回调；
- `native/plugins/hesai_adapter.cpp` 已接入 `UdpParser` 的包级自动识别、并行计算、分帧和点云压紧；
- `native/plugins/velodyne_adapter.cpp` 只移植 `ros-drivers/velodyne` 的原始 UDP 解算、分帧和校准结构，不链接 ROS 2、PCL、rclcpp 或 yaml-cpp；
- `native/plugins/seyond_adapter.cpp` 直接链接官方 Inno LiDAR SDK 的包校验、V1 适配和 XYZ 转换代码；在线 Robin/HB 紧凑包会读取并按设备缓存 AngleHV 角度表，离线 PCAP 会复用已经缓存的设备表；
- Robin-W 在完全隔离且没有设备缓存时使用安装包内嵌的该型号标称 AngleHV 兜底；在线连接后仍优先读取并缓存当前设备自己的工厂表；
- AT128 优先通过 PTC 9347 获取并缓存设备专属修正；跨网段/VLAN 或离线 PCAP 无法访问 PTC 时，自动加载随 Hesai SDK 提供并嵌入安装包的 AT128E2X 标称修正，避免 128 线全部被压到 Z=0。AT128P/E3X 使用对应 E3X 文件；网页会明确区分“雷达工厂标定”和“SDK 标称修正”。
- Windows 下四个原生动态库已使用 Zig/Clang 编译并完成 ABI 加载验证；
- Hesai 原始包适配器会通过 PTC 自动下载设备专属标定并按雷达 MAC 持久缓存，也保留 correction/firetimes 文件入口；当 Pandar40 跨网段导致 PTC 不可达时，使用 Hesai 官方通用 SDK 的 40 线标称角度兜底，避免新版 SDK 2.0 的全零俯仰角把点云压成平面。网页会显示当前标定来源。AT128 会按标定中的多视场编号组装完整点云，JT16、ATX 和 QT128 会自动读取各自所需的专属辅助配置。
- RoboSense 同一设备的 MSOP 和 DIFOP 会送入同一个 `rs_driver` 解码实例；机械式型号会由有效点云探测结果锁定，而不是仅凭共用包头猜测。
- Velodyne 自动读取数据包末尾 Product ID，选择随 `ros-drivers/velodyne` 提供的标准 YAML 校准；HDL-64E 的逐台参数差异较大，内置文件可用于自动出点，精确测量应换成设备自身 `db.xml/YAML` 标定。
- UDP 收包与厂商解码分线程运行，高频流量使用包特征、设备和解码器缓存以及原生批量 ABI；实测 RSM2 VLAN 2 可持续处理约 12,600 包/秒、10 帧/秒而无队列积压。
- SDK 输出会在原生层剔除无效零距离点；20 字节点结构直接作为 WebSocket/WebGL 交错顶点缓冲上传，不再逐点转换。页面 FPS 表示真实点云帧率，数据停止后会清除残帧并提示 `NO LIDAR DATA`。

开发机可运行以下脚本下载 RoboSense、Hesai 和 Seyond/Innovusion 官方 SDK。Velodyne 插件是独立实现，不需要下载或安装 ROS 仓库：

```powershell
./scripts/fetch_sdks.ps1
```

SDK 源码下载完成后，原生插件构建命令见 `native/README.md`。

Windows 不安装 Visual Studio 也可以构建：

```powershell
python -m pip install cmake ninja ziglang
./scripts/build_native_zig.ps1
```

## API

- `GET /api/status`：服务、WebSocket 客户端和监听器状态；
- `GET /api/devices`：发现的设备；
- `GET /api/lidar-web?ip=<雷达IP>`：报告优先读取雷达网页接口，网页不可达时尝试兼容型号的 PTC；
- `GET /api/hesai-ptc?ip=<雷达IP>`：直接查询禾赛 PTC，适用于已验证布局的型号；
- `GET /api/config`：当前 UDP 配置；
- `POST /api/config`：修改监听 IP、端口和源地址过滤；
- `GET /api/gateway`：转发、USB 存储、录包和 PTP 状态；
- `POST /api/gateway/config`：修改转发目标 IP/端口和原端口过滤；
- `POST /api/gateway/recording/start|stop`：开始或停止 USB PCAP；
- `POST /api/ptp/config`：配置/停止 linuxptp 主时钟或从时钟；
- `WS /ws/points`：点云二进制帧。

关闭最后一个点云网页后，服务保留 3 秒重连窗口；页面刷新会在窗口内恢复连接，真正关闭页面则自动退出后台进程并释放 HTTP、UDP 和 Npcap/libpcap 句柄。重复启动时若端口上已经是本程序，新进程会直接打开已有页面并正常退出，不再抛出端口占用错误。

## 测试

```powershell
python -m unittest discover -s tests -v
```

可把经典 Ethernet PCAP 逐包送入与在线模式相同的厂商插件做离线回归：

```powershell
python scripts/validate_pcap.py D:/test.pcap
```

单文件 EXE 还提供无抓包、无浏览器、无管理员网络操作的隔离自测入口，可测试一个 PCAP 或整个目录并生成 JSON 报告：

```powershell
UniversalLidarViewer.exe --offline-test D:/lidardata --offline-report offline-report.json
```

## EM4 ROS SDK 修复版

独立 ROS1/ROS2 工作区位于 [`ros/em4`](ros/em4/README.md)，包含 EM4 新版压缩列解码、DIFOP2 标定、520 通道修正、VLAN/自定义端口配置及测试记录。该工作区单独编译，不会替换当前点云查看器的解码插件。

已通过 12 项 EM4 回归和原 SDK 58 项测试；原始 PCAPNG 经驱动核心输出 95 帧、36,392,507 个有效点。当前验证主机没有 ROS，ROS 话题发布仍需在目标 Ubuntu/ROS 环境验证。编译与启动步骤见[使用说明](ros/em4/README_EM4_%E4%B8%AD%E6%96%87.md)。

## E2 网页查看器

已接入用户提供的 RoboSense RSE2 SDK 协议与标定，支持在线数据和 PCAP 回放，无需 ROS。需要重建 RoboSense 原生插件；[E2 使用与验证说明](docs/E2_VIEWER.md)。

## 构建与交付说明

源码运行需要 Python 3.10+。构建当前 Windows EXE 需要 Windows x64 Python、Nuitka 4.3、项目要求的解码 DLL 和官方 Npcap 安装器，详见 [打包说明](packaging/README.md)。

`licensing/private/`、签发记录、许可证和本机网络状态不属于客户交付内容。Npcap 免费版不支持静默安装；随产品对外分发需取得相应 OEM 权利，见 [Npcap 官方说明](https://npcap.com/oem/redist)。
