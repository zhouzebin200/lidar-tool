# 砺见智行桌面工具：石墨黑与冰蓝界面设计 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将现有 C++17 / Dear ImGui / GLFW 跨平台桌面端彻底重构为精致、克制、信息层级清晰的“石墨黑与冰蓝”专业点云工作台，在保留全部底层 PCAP、实时以太网监听、PTC 硬件诊断、着色视口、报告导出功能的前提下实现与用户选定设计图 100% 视觉对齐。

**Architecture:** 
- 在 `ui_theme.hpp/cpp` 中集中沉淀石墨黑与冰蓝设计规范、几何圆角/内边距、三层表面层次（底色、面板、抬升卡片、输入槽）及语义色定义；
- 在 `ui_workspace.cpp` 中全面重构工作区四大核心区域（顶部集成导航与状态栏、左侧设备与滚动日志区、中央 3D 视口浮层控制区与 Turbo 标尺、右侧显示与离线回放属性面板）；
- 在 `main.cpp` 中规范化跨平台统一中文字体加载路径（相对于执行文件与资源目录），并在 `CMakeLists.txt` 中保证构建产物自动打包对应资源。

**Tech Stack:** C++17, Dear ImGui (Docking), GLFW 3, OpenGL 3.3 Core, GLSL, CMake, Zig (for Win64 cross-compile).

**Spec:** 用户需求指令《砺见智行桌面工具：石墨黑与冰蓝界面设计》与用户确认设计效果图 (`media_1793627942071_5d3bc74c.jpg`).

## Global Constraints

- 三维背景 `#0C1118`，面板背景 `#151C26`，抬升卡片 `#1C2633`，输入框区域 `#111923`。
- 边框 `#2A3747`，正文文本 `#E8EEF7`，次要说明文字 `#9BAEC3`。
- 强调冰蓝 `#74B9FF`，主要操作按钮深蓝底高对比白字（对比度 >= 4.5:1）。
- 成功绿 `#10B981`，等待/状态琥珀黄 `#F59E0B`，错误红 `#EF4444`。
- 间距基准 8px，面板内边距 16px，控件圆角 6px，面板圆角 8px，细边框 1px，纯色背景无发光或装饰动画。
- 默认窗口 1600×960（顶部约 72px、左侧约 264px、右侧约 320px），最小工作区兼容 1100×720，较小宽度下指标与内容不重叠、长面板允许独立垂直滚动。
- 原生 C++17 无任何 Web/Node/Electron 运行时依赖；保留现有 PCAP 回放、以太网监听、PTC 诊断、PDF 导出全功能。

## Review Focus

1. **窄视口布局挤压 (1100x720)**：在 1100×720 极限窗口下，顶部导航项与右侧指标是否重叠或遮挡？期望：自适应排布，指标不重叠，视口保持主视野。
2. **左侧设备面板与日志覆盖**：日志条数增多时是否会覆盖设备信息或端口标签？期望：日志区使用独立 `BeginChild` 滚动区，设备卡片保持置顶。
3. **状态双重表达（文字 + 颜色）**：空闲、回放、实时是否同时具备文字和色彩标签？期望：绝不单纯依赖颜色，回放显示 `[ ▶ 回放中 ]`，实时显示 `[ ⚡ 实时监听 ]`，空闲显示 `[ 准备就绪 ]`。
4. **视口空闲提示准确性**：无数据流且未回放时，视口中央是否明确提示“准备就绪：可导入录包或配置实时监听”，且不虚假显示 `LIVE`。
5. **视口 Turbo 高度/反射率垂直色标图例**：右上方色标图例刻度是否对齐、数值与 Turbo 渐变条显示清晰无锯齿。

---

### Task 1: 调色板与全局几何参数重构 (ui_theme)

**Files:**
- Modify: `cpp/src/ui/ui_theme.hpp`
- Modify: `cpp/src/ui/ui_theme.cpp`

- [ ] **Step 1: 在 `ui_theme.hpp` 中声明石墨黑与冰蓝主题接口与语义颜色常量**
  - 定义 `apply_graphite_iceblue_theme()`
  - 定义语义颜色常量结构体 `ThemeColors`（3D背景 `#0C1118`、面板 `#151C26`、表面 `#1C2633`、输入框 `#111923`、边框 `#2A3747`、正文 `#E8EEF7`、次要 `#9BAEC3`、冰蓝 `#74B9FF`、主按钮 `#1B6CA8`、成功绿 `#10B981`、琥珀黄 `#F59E0B` 等）
- [ ] **Step 2: 在 `ui_theme.cpp` 中实现完整 ImGui 样式与颜色表**
  - 设置 `WindowRounding = 8.0f`, `ChildRounding = 8.0f`, `FrameRounding = 6.0f`, `PopupRounding = 6.0f`
  - 设置 `WindowPadding = ImVec2(16.0f, 16.0f)`, `FramePadding = ImVec2(10.0f, 6.0f)`, `ItemSpacing = ImVec2(8.0f, 8.0f)`
  - 设置 `WindowBorderSize = 1.0f`, `ChildBorderSize = 1.0f`, `FrameBorderSize = 1.0f`
  - 填入所有 `ImGuiCol_*` 颜色映射，使全部基础组件默认即呈现石墨黑与冰蓝质感
- [ ] **Step 3: 编译验证**
  - 执行 `cmake --build cpp/build -j4` 验证编译无警告通过

---

### Task 2: 顶部导航栏重构 (draw_top_bar)

**Files:**
- Modify: `cpp/src/ui/ui_workspace.cpp:82-270`
- Modify: `cpp/src/ui/ui_workspace.hpp`

- [ ] **Step 1: 品牌 Logo 与标题区域规范化**
  - 绘制几何线框二十面体/雷达图形 Logo (利用 ImDrawList 绘制清晰硬核的蓝白几何线框)
  - 标题输出为粗体大字 `砺见智行` (White `#E8EEF7`)，副标题输出 `激光雷达测试工作台` (Slate `#9BAEC3`)
  - 彻底移除对用户无帮助的技术调试字样
- [ ] **Step 2: 中部导航栏胶囊/分段切换控件**
  - 统一为 5 项：`点云显示`、`检测报告`、`网络设置`、`PTP 设置`、`支持型号`
  - 选中项使用深蓝底 (`#0284C7` / `#1B6CA8`) + 冰蓝文字/细线高亮；未选中项使用透明无边框 + 次要文字色，悬停轻度提亮
- [ ] **Step 3: 右侧指标与状态指示**
  - 点数：`128,640 点`（或实时点数），帧率：`10.0 FPS`
  - 状态胶囊标签：同时结合文字与颜色
    - 回放中：金色/琥珀色线框 `[ ▶ 回放中 ]` (`#F59E0B`)
    - 实时流：绿色线框 `[ ⚡ 实时监听 ]` (`#10B981`)
    - 空闲：暗灰蓝线框 `[ 准备就绪 ]` (`#9BAEC3`)
  - 窄屏自适应：动态计算宽度，防止导航与指标重叠
- [ ] **Step 4: 编译验证**
  - 运行 `cmake --build cpp/build -j4` 确保无编译错误

---

### Task 3: 左侧面板重构 (draw_devices_panel)

**Files:**
- Modify: `cpp/src/ui/ui_workspace.cpp:275-429`

- [ ] **Step 1: 设备概览卡片 (DEVICES)**
  - 卡片抬升底色 `#1C2633`，边框 `#2A3747`，圆角 8px
  - 顶部行：状态圆点（在线绿 / 离线灰）+ 雷达简标 + 型号大字（如 `Hesai AT128` 或 `Hesai Pandar 40P`）+ 厂商 `禾赛科技`
  - 详细参数网格：型号、序列号、IP 地址、固件版本（标签灰色 `#9BAEC3`，数值浅白 `#E8EEF7`）
- [ ] **Step 2: 输入数据源卡片 (DATA SOURCE)**
  - 卡片展示当前数据源类型（`PCAP 离线回放` 或 `以太网实时数据流`）
  - 显示文件名/流状态、数据包总量计数、已捕获/回放时长
- [ ] **Step 3: 监听端口网格卡片 (PORTS)**
  - 2x2 结构化排布：
    - `UDP 2368 LiDAR数据` | `UDP 51100 状态信息`
    - `UDP 8080 配置端口`  | `UDP 6668 时间同步`
- [ ] **Step 4: 运行日志独立滚动区域 (LOGS)**
  - 使用 `ImGui::BeginChild("SystemLogs", ...)` 设立独立滚动视口
  - 限制最大高度或撑满底部空间，日志行带时间戳（如 `14:20:11 已加载 PCAP 文件...`）
  - 彻底杜绝日志过长时固定定位覆盖上方设备内容的问题
- [ ] **Step 5: 编译验证**
  - 运行 `cmake --build cpp/build -j4` 验证通过

---

### Task 4: 中央三维点云视口浮层与标尺重构 (draw_viewport_overlays)

**Files:**
- Modify: `cpp/src/ui/ui_workspace.cpp:430-471`
- Modify: `cpp/src/renderer/point_cloud_renderer.cpp` (如有需要调整背景清除色为 `#0C1118`)

- [ ] **Step 1: 视口背景色统一为石墨黑 `#0C1118`**
  - 检查 OpenGL `glClearColor` 设置为 `(0.047f, 0.067f, 0.094f, 1.0f)`
- [ ] **Step 2: 视口左上角状态浮层 Chip**
  - 悬浮半透明小胶囊：`PCAP 回放 · Hesai AT128` 或 `以太网实时流 · Hesai Pandar 40P`，圆角 6px，实色深灰蓝底
- [ ] **Step 3: 视口右上角 Turbo 高度/反射率垂直标尺 (Color Scale Bar)**
  - 标题：`高度着色 (m)` 或 `反射强度`
  - 垂直绘制 Turbo 彩色渐变条（Red 30m -> Yellow 20m -> Green 10m -> Cyan 0m -> Blue -10m）
  - 右侧标注刻度数值与短线
- [ ] **Step 4: 视口底部操作提示与坐标轴指示**
  - 左下角：`[🖱️ 旋转]  [🖱️ 平移]  [🖱️ 缩放]`
  - 右下角：XYZ 三色坐标系示意（X 红、Y 绿、Z 蓝）
- [ ] **Step 5: 视口底部悬浮回放进度控制条 (Timeline Controller)**
  - 仅在离线回放或有数据包时显示悬浮 Timeline Bar：
    - 播放/暂停图标按钮 `[⏸] / [▶]`
    - 细腻平滑进度条（深底蓝轨，冰蓝滑块）
    - 时间刻度标签 `00:12 / 01:48`
    - 倍速选择下拉框 `1.0 ×`
- [ ] **Step 6: 空闲状态居中友好提示**
  - 当无数据流且未回放时，中央显示低调提示：`准备就绪：可导入录包或配置实时监听`，不显示虚假的 `LIVE`
- [ ] **Step 7: 编译验证**
  - 执行 `cmake --build cpp/build -j4` 确保正常通过

---

### Task 5: 右侧显示与控制面板重构 (draw_controls_panel & draw_render_tab)

**Files:**
- Modify: `cpp/src/ui/ui_workspace.cpp:472-699`

- [ ] **Step 1: 标题与分区 (PROPERTIES)**
  - 标题：`显示与控制` (White bold) 与右侧小字 `PROPERTIES`
  - 划分为三大功能卡片组：`点云渲染`、`视角切换`、`离线回放/实时控制`
- [ ] **Step 2: 点云渲染分组**
  - 着色方式下拉框：`高度 · Turbo`、`反射强度`、`方位角`、`纯白`
  - 点大小调节：滑块 1.0 - 5.0 px，右侧显示 `2.0 px`
  - 最大距离调节：滑块 10 - 200 m，右侧显示 `120 m`
  - 勾选框：`[✓] 圆点抗锯齿` (绑定 `GL_POINT_SMOOTH`)
- [ ] **Step 3: 视角切换 2x2 按钮网格**
  - 2x2 按钮组：
    - `[ 🧊 透视 ]` (选中高亮) | `[ ✛ 俯视 ]`
    - `[ ⬚ 前视 ]`              | `[ ◨ 侧视 ]`
  - 按钮带对应微小几何图标，中性深色底，点击平滑切换相机朝向
- [ ] **Step 4: 离线回放操作区**
  - 导入主操作按钮：大尺寸实色冰蓝/深蓝主按钮 `[ 📁 导入 PCAP 文件 ]`，对比度 >= 4.5:1
  - 次级按钮行：`[ ▶ 开始/暂停 ]`、`[ ⏹ 停止 ]`、`[ ↺ 重置 ]`，中性深底
  - 勾选框：`[✓] 循环回放`
- [ ] **Step 5: 底部状态卡片与快捷操作**
  - 状态指示：绿色圆点 + `准备就绪`，次行 `可导入录包或配置实时监听`
  - 若检测到在线硬件雷达，提供 `打开雷达控制页 ↗` 按钮
- [ ] **Step 6: 编译验证**
  - 执行 `cmake --build cpp/build -j4` 检查通过

---

### Task 6: 网络、PTP、支持型号与报告工作台统一视觉升级

**Files:**
- Modify: `cpp/src/ui/ui_workspace.cpp:700-1390`

- [ ] **Step 1: 网络设置页面 (`draw_network_tab`)**
  - 适配石墨黑 `#151C26` 与抬升卡片 `#1C2633`，统一输入框深色背景 `#111923` 与冰蓝边框高亮
  - 保持网络参数、单播/组播/广播切换与 `打开雷达控制页 ↗` 按钮正常生效
- [ ] **Step 2: PTP 设置与支持型号页面 (`draw_ptp_tab`, `draw_support_tab`)**
  - 统一卡片样式、网格布局与状态灯
- [ ] **Step 3: 质检报告工作台 (`draw_report_modal`)**
  - 目录导航统一为深蓝底与冰蓝细线，8 个章节表单统一控件样式
  - 外观实物照上传与缩略图卡片匹配新圆角与深色底
  - Chromium 矢量 PDF 导出与 HTML 导出按钮视觉加固
- [ ] **Step 4: 编译验证**
  - 执行 `cmake --build cpp/build -j4` 确保全部页面无样式破损

---

### Task 7: 跨平台字体加载路径加固与资源打包

**Files:**
- Modify: `cpp/src/main.cpp`
- Modify: `cpp/CMakeLists.txt`

- [ ] **Step 1: 建立相对资源路径解析机制**
  - 编写可执行文件目录解析函数 `get_executable_directory()`
  - 字体寻找优先级：
    1. `<exe_dir>/../Resources/fonts/` (macOS Bundle) 或 `<exe_dir>/resources/fonts/` (Windows/Linux)
    2. `resources/fonts/` (相对当前运行路径)
    3. 系统原生高质量字体兜底（Windows `msyh.ttc`, macOS `PingFang.ttc` / `STHeiti Light.ttc`, Linux `wqy-microhei.ttc`）
    4. 默认字体 `AddFontDefault()`
- [ ] **Step 2: 更新 CMakeLists.txt 资源同步**
  - 将字体资源与插件资源在构建时同步拷贝至应用包与构建目录
- [ ] **Step 3: 编译验证**
  - 执行 `cmake --build cpp/build -j4`

---

### Task 8: 双端编译与全量测试套件回归

**Files:**
- Output: `cpp/build/bin/ulv_viewer`
- Output: `砺见智行激光雷达测试工具.app`
- Output: `cpp/dist_windows_x64/ulv_viewer.exe`

- [ ] **Step 1: 运行全量 C++ 单元与集成测试套件**
  - 执行 `./cpp/build-tests/bin/ulv_test_suite`，确保全部 74 项测试 100% PASS
- [ ] **Step 2: 编译 Windows x64 目标产物**
  - 执行 `cmake --build cpp/build-win -j4`
  - 同步更新 `cpp/dist_windows_x64/ulv_viewer.exe`
- [ ] **Step 3: 重新打包 macOS .app 原生应用包**
  - 确认 `砺见智行激光雷达测试工具.app` 内嵌最新二进制与配置

---

### Task 9: 视觉真机验收与效果图留存

**Files:**
- Screenshots in artifact directory / workspace

- [ ] **Step 1: 启动 macOS 原生应用程序**
  - 启动 `./cpp/build/bin/ulv_viewer`
- [ ] **Step 2: 截取并比对核心界面**
  - 默认 1600×960 视口截图
  - 极限 1100×720 视口截图（检查导航与指标无重叠）
  - PCAP 导入回放状态截图（检查悬浮进度条、Turbo 色标图例、回放中状态胶囊）
  - 检测报告中心界面截图
- [ ] **Step 3: 与用户确认设计图进行逐项对照并向用户交付**
