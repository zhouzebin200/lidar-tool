# 砺见智行激光雷达测试工具 (Pure C++ Native 桌面端) 交接文档

> **生成时间**：2026-11-02  
> **当前状态**：已完成核心架构搭建、1:1 网页端工业 UI 复刻、OpenGL 3.3 动态自适应 Turbo 着色、八大项弹窗式检测报告工作台（Report Studio）、PCAPNG 格式支持、车载 SOME/IP 自动脱壳、多厂商雷达真实录包全量兼容、点云出帧率与 UI 刷新率解耦定拍（10.0 FPS）、默认以太网实时数据流监听模式，并通过 macOS arm64 本机与 Windows 11 x64 交叉编译及全量 74/74 自动化测试。  
> **Git 状态**：遵照指令，当前所有新增与修改均保留在本地工作区，**尚未执行 `git commit` 或 `git push`**。

---

## 一、 项目背景与技术选型

为了彻底摆脱 Python 运行时环境与浏览器（Web）依赖，提高软件交付的保密性、防逆向能力与跨平台便携性，本项目采用 **路线 A（纯 C++17 + Dear ImGui Docking + GLFW 3.4 + OpenGL 3.3 Core Profile）** 构建独立桌面客户端：

- **零外部运行时依赖**：UI、3D 渲染、PCAP 解析、网络监听、报告导出全部静态编译进单一可执行文件（macOS 约 `2.2 MB`，Windows 约 `9.8 MB`）。
- **C ABI 硬件解码插件架构**：复用并扩展 `native/` 目录下的五大厂商 C++ SDK 插件（`.dll` / `.dylib` / `.so`），通过 20 字节紧凑点结构 `ulv_point_v1` 零拷贝直传 OpenGL VBO。
- **跨平台支持**：同一套源码原生支持 Windows x64、macOS (Apple Silicon / Intel) 及 Ubuntu Linux。

---

## 二、 目录结构与核心模块说明

```text
universal-lidar-viewer/
├── native/                           # 厂商硬件解码插件源码 (C ABI)
│   ├── include/
│   │   ├── ulv_plugin.h              # 稳定 C ABI 接口与 20B 点结构体定义 (ulv_point_v1)
│   │   └── macos_socket_compat.h     # macOS 平台 recvmmsg 与字节序宏 (htobe16/32/64) 兼容头
│   └── CMakeLists.txt                # 插件跨平台构建配置
├── plugins/                          # 编译产出的动态库插件 (.dll / .dylib)
│   ├── libulv_hesai.{dll,dylib}
│   ├── libulv_robosense.{dll,dylib}
│   ├── libulv_velodyne.{dll,dylib}
│   ├── libulv_huawei_l306.{dll,dylib}
│   └── libulv_seyond.dll
├── corrections/                      # 各雷达厂商角度/光学校准文件 (如 AT128E3X.dat)
└── cpp/                              # 【新增】纯 C++ 独立桌面端主工程
    ├── CMakeLists.txt                # 主程序 CMake 构建脚本
    ├── HANDOVER.md                   # 本交接文档
    ├── cmake/                        # 交叉编译工具链配置
    │   ├── zig-windows-x64-toolchain.cmake
    │   └── zig-{cc,cxx,ar,ranlib}-win.sh
    ├── third_party/                  # 源码级内置第三方轻量库 (零外部依赖)
    │   ├── glfw/                     # GLFW 3.4 窗口与上下文管理
    │   ├── glad/                     # OpenGL 3.3 Core Profile 标准加载器 (Windows/Linux)
    │   └── imgui/                    # Dear ImGui (Docking 分支)
    ├── include/
    │   ├── ulv_gl.hpp                # 跨平台 OpenGL 头文件分发器
    │   ├── ulv_point_types.hpp       # C++ 侧点云与包围盒数据结构
    │   └── math3d.hpp                # 轻量级 3D 向量与 4x4 矩阵运算库
    ├── src/
    │   ├── main.cpp                  # 程序入口、渲染主循环、资源自动寻址、EMA 动态范围计算
    │   ├── renderer/
    │   │   ├── pointcloud_renderer.* # OpenGL 3.3 点云渲染器 (4段 Turbo 伪彩 / Gamma 反射率 / 分类色)
    │   │   ├── grid_renderer.*       # 3D 地面网格、10m~100m 同心测距环与 RGB 坐标轴
    │   │   └── camera.hpp            # 3D 轨迹球相机 (旋转/平移/缩放 + 透视/俯视/前视/侧视预设)
    │   ├── ui/
    │   │   ├── ui_workspace.*        # 1:1 复刻网页端三栏布局 + 居中弹出式【检测报告工作台】
    │   │   └── ui_theme.*            # 深空黑 (#0b1116) + 砺见红 (#e21d2f) + 科技蓝工业主题
    │   ├── core/
    │   │   ├── decoder_manager.*     # 跨平台 dlopen/LoadLibrary 插件管理器
    │   │   ├── frame_buffer.hpp      # 线程安全点云双缓冲队列
    │   │   ├── ptp_monitor.*         # IEEE 1588v2 / 802.1AS (gPTP) 授时同步报文解析器
    │   │   └── report_generator.*    # 八大检测项数据模型与 HTML/PDF 报告导出器
    │   └── net/
    │       ├── pcap_reader.*         # Classic PCAP 解析引擎 (支持 802.1Q / QinQ VLAN 剥离)
    │       ├── udp_receiver.*        # Winsock2 / POSIX 跨平台非阻塞多端口 UDP 接收器
    │       └── detector.*            # 五大厂商 UDP 载荷魔数零拷贝自动嗅探器
    ├── build/                        # macOS 本机编译产物目录 (bin/ulv_viewer)
    ├── build-win/                    # Windows x64 交叉编译产物目录 (bin/ulv_viewer.exe)
    └── dist_windows_x64/             # 打包就绪的 Windows x64 绿色免安装发布包
```

---

## 三、 编译与构建指南

### 1. macOS 本机编译 (Apple Silicon / Intel)

#### (1) 编译硬件解码插件 (`plugins/*.dylib`)
```bash
cmake -B native/build-mac -S native -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-mac -j4
cp native/build-mac/*.dylib plugins/
```

#### (2) 编译 C++ 桌面端主程序 (`cpp/build/bin/ulv_viewer`)
```bash
cmake -B cpp/build -S cpp -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j4
```
编译完成后，可执行文件位于 `cpp/build/bin/ulv_viewer`。

---

### 2. 在 macOS 上交叉编译 Windows x64 版本 (`ulv_viewer.exe`)

项目已配置好基于 **Zig C/C++ 交叉编译器** 的工具链（脚本位于 `cpp/cmake/zig-windows-x64-toolchain.cmake`）：

```bash
# 若首次配置或清理后重新配置：
cmake -B cpp/build-win -S cpp -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows-x64-toolchain.cmake

# 增量编译 Windows x64 可执行文件：
cmake --build cpp/build-win -j4
```
> **注意**：如果换用其他电脑编译 Windows 版本，请检查 `cpp/cmake/zig-cc-win.sh` 等 4 个包装脚本中的 `zig` 可执行文件路径（当前指向本机 `/Users/zzb/.cache/uv/archive-v0/-uU-kKaKeqQq9YtN/ziglang/zig`），或直接在 Windows 上使用 Visual Studio / MinGW-w64 原生编译。

#### 组装 Windows 绿色便携发布包 (`cpp/dist_windows_x64/`)
```bash
mkdir -p cpp/dist_windows_x64/plugins cpp/dist_windows_x64/corrections
cp cpp/build-win/bin/ulv_viewer.exe cpp/dist_windows_x64/
cp plugins/*.dll cpp/dist_windows_x64/plugins/
cp -r corrections/* cpp/dist_windows_x64/corrections/
```

---

## 四、 运行与测试验证

### 1. 命令行无头自检模式 (`--test`)
用于自动化验证插件加载、标定文件读取与 PCAP 全量解码帧数：
```bash
# macOS 本机测试
./cpp/build/bin/ulv_viewer --test /Users/zzb/Desktop/111.pcap

# Windows PowerShell 测试
.\ulv_viewer.exe --test .\111.pcap
```
正常输出应显示加载全部插件，针对 `111.pcap`（Hesai AT128）解析 `20,703` 个报文、完整输出 `30` 帧点云（最大单帧 `141,275` 点），返回码为 `0`。

### 2. 桌面 GUI 模式
```bash
# 直接启动（默认进入以太网实时 UDP 监听模式，监听端口 2368, 51100, 6668, 8080，随时准备渲染实时点云）
./cpp/build/bin/ulv_viewer

# 或指定任意 PCAP / PCAPNG 文件直接回放
./cpp/build/bin/ulv_viewer /path/to/capture.pcap
```

---

## 五、 关键技术细节与已解决的坑（接手必读）

1. **资源目录自动寻址 (`locate_resource_dirs`)**：
   - 早期版本仅检查当前工作目录 `./plugins` 和 `../plugins`，导致在 `cpp/build/bin/` 下直接启动时找不到根目录的 `plugins/` 和 `corrections/`（从而导致 macOS 下点云解码插件未加载、不出点云）。
   - **已修复**：在 `cpp/src/main.cpp` 中实现了 `locate_resource_dirs(argv[0], ...)`，同时基于当前目录与可执行文件真实路径（`argv[0]`）向上递归搜索 `.`、`..`、`../..`、`../../..`，无论从哪个目录启动都能 100% 定位到 `plugins/` 与 `corrections/`。

2. **点云动态范围自适应与着色器 (`pointcloud_renderer.cpp`)**：
   - 完全对齐原网页端 `web/app.js` 的着色算法：
     - 模式 `0`：**高度 · Turbo 伪彩**（4 段高对比度色彩插值：深蓝 ➔ 科技青 ➔ 翠绿 ➔ 明黄 ➔ 炽红）。
     - 模式 `1`：**反射强度 · 灰度标尺**（带 `pow(v, 0.55)` Gamma 矫正，大幅提亮暗部细节）。
     - 模式 `2`：**测距 · Turbo 伪彩**。
     - 模式 `3`：**扫描线 · 分类色**（基于黄金分割率 `0.61803398875` 散列）。
   - **EMA 动态范围平滑**：在 `main.cpp` 每帧点云出队时，自动按步长抽样统计当前帧真实的 `zMin`、`zMax` 和 `intensityMax`，并以 `0.8 / 0.2` 指数移动平均更新，避免固定阈值导致颜色单一。

3. **OpenGL 3.3 Core 跨平台加载器 (`ulv_gl.hpp` & `imconfig.h`)**：
   - macOS 原生使用系统框架 `<OpenGL/gl3.h>`。
   - Windows / Linux 使用内置的 `glad`（`cpp/third_party/glad`），并在 `cpp/third_party/imgui/imconfig.h` 底部定义了 `IMGUI_IMPL_OPENGL_LOADER_CUSTOM`，使 ImGui 与自定义渲染器共享同一个 GLAD 上下文。

4. **中文字体与乱码规避**：
   - 启动时自动探测加载系统原生中文字体（Windows: `C:\Windows\Fonts\msyh.ttc` / `simhei.ttf`；macOS: `/System/Library/Fonts/PingFang.ttc`；Linux: `wqy-microhei.ttc`），加载 `GetGlyphRangesChineseFull()`。
   - 注意：系统标准中文字体不包含彩色 Unicode Emoji（如 `📂`、`⏸`），在按钮文字中应使用纯中文或标准 ASCII 符号，避免显示为 `?`。

---

## 六、 已完成核心增强特性 (Milestones 1~5 全部落地)

1. **跨平台原生文件选择对话框 (`cpp/src/ui/file_dialog.hpp/.cpp`)**：
   - 点击“导入 PCAP 录包文件”自动唤起操作系统原生对话框（macOS Cocoa `NSOpenPanel` / Windows Win32 `GetOpenFileNameA` / Linux `zenity`）；
   - 支持 `.pcap`、`.pcapng`、`.cap` 文件格式智能过滤，选定后自动无缝切换至回放模式、重置解码管线并即刻开始 3D 点云渲染。
2. **实时以太网 UDP 抓包与 UI 交互联动 (`cpp/src/net/udp_receiver.hpp/.cpp`)**：
   - 右侧【网络设置】面板提供“开始实时监听 / 停止监听”全功能控制及厂商常用端口预设（禾赛、速腾、华为等）；
   - 支持多端口非阻塞 `poll`/`WSAPoll` 接收，通过互斥锁与原子变量保护点云解码管道，并提供实时收包数、Mbps 吞吐量及 FPS 平滑遥测。
3. **雷达 PTC 9347 / Web 诊断参数自动拉取 (`cpp/src/net/ptc_client.hpp/.cpp`)**：
   - 实现了纯 C++17 的 PTC 1.0/2.0 二进制协议客户端与 HTTP REST API 自动降级客户端；
   - 在居中检测报告工作台中上线「⚡ 一键采集雷达参数」按钮，通过异步后台线程（`std::future`）执行请求（1.5s 超时控制，绝不阻塞 UI 渲染帧），自动提取 S/N 序列号、固件版本、实时转速及各通道工作温度并填入报告表单。
4. **3D 视口离屏点云截图与 Base64 报告嵌入 (`cpp/src/renderer/viewport_capture.hpp/.cpp` & `image_encoder.hpp/.cpp`)**：
   - 建立 1280x720 (16:9 HD) 独立离屏 FBO 渲染通道，只截取干净纯粹的点云与坐标网格画面，避开 ImGui 弹窗和控件遮挡，并提供视口降级与纯内存 CPU 光栅化回退；
   - 内置单头文件 `stb_image_write.h` 进行纯内存 PNG 压缩，并通过 RFC 4648 规范直接编码为 Base64 Data URI（零磁盘临时垃圾文件）；
   - 自动嵌入检测报告第 5 项“点云成像与空间分布”，新增 `@media print` 打印 CSS 与顶栏 `window.print()` 一键打印/导出 PDF 按钮，并在导出后支持自动唤起系统默认浏览器一键预览。
5. **跨平台双端编译与全量自动化测试**：
   - macOS arm64 原生编译（`cmake --build cpp/build`）零警告零错误；
   - Windows x64 交叉编译（`cmake --build cpp/build-win`）零警告零错误，便携发布包生成于 `cpp/dist_windows_x64/`；
   - 74/74 项单元与集成测试全部通过，20,709 报文无头自检（`--test`）解码 100% 保持稳定。
6. **点云出帧率（10.0 FPS）与显示器刷新率（100 Hz / 60 Hz）彻底解耦与定拍**：
   - 顶栏遥测状态拆分为「点云帧率（如 10.0 FPS / 等待中为 -- FPS）」、「出帧周期（如 100.0 ms）」以及「UI 渲染（如 100 Hz / 60 Hz）」，解决此前将屏幕渲染高刷新率误认作雷达点云频率的问题；
   - PCAP 回放加入严格 10.0 Hz 帧间隔定拍（`target_frame_interval = 100ms`）与微秒级高精度同步，杜绝突发过冲或降频；单 tick 安全阈值提升至 10,000 包，确保速腾聚创 RSEMX 等单帧高密度包不被截断。
7. **PCAPNG 块格式与高精度时间戳原生解析 (`cpp/src/net/pcap_reader.hpp/.cpp`)**：
   - 完整支持 Section Header Block (SHB 0x0A0D0D0A)、Interface Description Block (IDB 0x00000001) 及 Enhanced Packet Block (EPB 0x00000006)；
   - 解析 IDB Option 9（`if_tsresol`），支持纳秒级（$10^{-9}$）与微秒级时间戳自适应对齐。
8. **车载 SOME/IP 协议头自动嗅探与无损脱壳 (`cpp/src/net/detector.cpp` & `cpp/src/core/decoder_manager.cpp`)**：
   - 自动识别并剥离汽车电子域控制器常见的 8 字节、12 字节与 16 字节 SOME/IP 封装，精准对接各雷达 SDK 解码器（如速腾 M2、EMX 等车载录包）。
9. **多厂商雷达真实录包全量兼容与角度标定挂载 (`/Volumes/软件/lidardata`)**：
   - **禾赛 PandarQT (QT64)**：精准识别 `major=3, minor=1` 为 PandarQT，挂载官方 64 线标定角度，避免错误挂载 AT128 标定；
   - **禾赛 Pandar40P**：支持 1262 字节 0xFFEE 报文特征嗅探；
   - **速腾聚创 RSM1 / RSM2 / RSEMX / RSHELIOS**：全数支持普通 PCAP 与 PCAPNG，自动脱壳出帧；
   - **威力登 Velodyne VLP-16**：自动挂载 `corrections/velodyne/VLP16db.yaml` 离线光学校准；
   - **图达通 Seyond Robin-W**：自动挂载 `corrections/seyond/Robin-W.bin` 离线角度表，并修复 ARM64 下 `-mcrc32` 编译宏；
   - **华为 Huawei L306**：完整解析 100Mbps 64,643 点/帧。
10. **默认启动行为优化：即刻进入以太网实时数据流监听模式**：
    - 启动时不再默认自动回放 AT128 录包，而是直接清空视口，自动开启监听多厂商常用端口（`2368, 51100, 6668, 8080`），状态栏提示“正在等待以太网数据流...”，随时待命接收网络数据并实时出点云。
11. **一键直达雷达 Web 控制后台 / 数据页面**：
    - 在左侧【INPUT / 设备卡片】、右侧【网络设置】以及居中【检测报告工作台】顶栏均上线「打开雷达控制页 ↗」科技蓝按钮；
    - 自动感知局域网嗅探到的雷达 IP（如 `192.168.1.201`），点击后通过操作系统默认浏览器直接弹出雷达管理后台。
12. **PDF 专业矢量导出故障根因排查与真实 PDF 生成修复**：
    - **故障根因**：原版本 `export_pdf` 仅简单将 HTML 字符串写入扩展名为 `.pdf` 的文本文件，导致操作系统默认 PDF 阅读器（macOS Preview / Win Acrobat）在解析文件头时因缺少 `%PDF-` 二进制特征码而报错崩溃；
    - **彻底修复**：引入 Chromium 无头打印管道（兼容 macOS Google Chrome / Windows Microsoft Edge / Linux Chromium），自动生成符合标准规范的 `%PDF-1.4` 矢量二进制文档，若本地未安装无头转换器则无缝自动打开带打印控件的标准 HTML 报告，彻底杜绝文件损坏报错。
13. **检测报告支持本地上传雷达外观检验照片**：
    - 在【检测报告工作台】第 1 项（外观）中增加原生图片选择器（`show_open_image_dialog`），支持 JPG、PNG、WebP、BMP 图片一键上传；
    - 自动对实物照片进行 RFC 4648 Base64 编码并生成 Data URI，嵌入专业报告第 1 项，支持照片图注自定义与左侧导航 `[📷]` 状态角标指示。
14. **3D 视口滚轮缩放与触控板双指缩放彻底修复**：
    - **故障根因**：原版本依赖 ImGui 内部的 `io.MouseWheel`，但由于其在 `ImGui::NewFrame()` 之前被读取（事件尚未从 GLFW 队列解包到 IO），导致滚轮事件经常被置零；同时受 `!io.WantCaptureMouse` 偶发阻塞，且原线性缩放公式 `1.0 - delta * 0.12` 在快速拨动滚轮时可能导致缩放系数变为负数甚至坍缩；
    - **彻底修复**：在 GLFW 层面挂载原生滚轮钩子（`glfw_scroll_callback_hook`），精准识别鼠标指针在 3D 视口区域（`in_3d_viewport`），引入指数级平滑缩放算法（`factor = std::exp(-delta * 0.08)`），确保在 Mac 触控板惯性滑动和 Windows 物理滚轮下均丝滑无阻。
