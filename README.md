# 砺见智行激光雷达测试工具（纯 C++17 原生桌面端）

面向 **Hesai（禾赛）、RoboSense（速腾聚创）、Velodyne、Huawei L306、Seyond/Innovusion（图达通）** 的跨平台原生点云查看与检测报告工具。基于 **C++17 + Dear ImGui (Docking) + GLFW 3.4 + OpenGL 3.3 Core** 构建，零 Python / Node / 浏览器运行时依赖。

## 目录结构

```text
cpp/                  C++ 桌面主工程（UI、OpenGL 渲染、PCAP/PCAPNG、UDP/PTC、报告导出、测试）
  cmake/              Zig Windows x64 交叉编译工具链
  resources/          内置 NotoSansSC 中文字体与 OFL 许可
  src/                主程序源码（core / net / renderer / ui）
  tests/              C++ 单元测试、压力测试与无头 UI 测试（fixtures/111.pcap）
  third_party/        内置 GLFW 3.4、GLAD、Dear ImGui、stb_image_write.h
native/               五大厂商 C ABI 解码插件源码（生成 plugins/*.dylib 与 *.dll）
plugins/              编译产出的动态库插件
corrections/          雷达标称校准文件（Hesai / Seyond / Velodyne）
third_party/          厂商 SDK 源码（rs_driver / HesaiLidar_SDK_2.0 / inno-lidar-sdk）
scripts/              一键双端编译、SDK 获取与 E2E 测试脚本
砺见智行激光雷达测试工具.app   macOS 独立应用包
cpp/dist_windows_x64/       Windows x64 绿色免安装目录
```

## 快速编译与运行

### 1. 一键编译（macOS + Windows x64）

```sh
./scripts/build_all.sh all      # 同时编译 macOS 与 Windows x64
./scripts/build_all.sh mac      # 仅编译 macOS 插件、主程序、.app 与测试
./scripts/build_all.sh win      # 仅交叉编译 Windows x64 插件、EXE 并组装便携包
```

> 交叉编译 Windows x64 需在 `PATH` 中提供 `zig`（或设置环境变量 `ZIG_EXECUTABLE`），也可在 Windows 上直接使用 Visual Studio / MinGW CMake 原生编译 `native/` 与 `cpp/`。

### 2. macOS 手动编译

```sh
cmake -B native/build-mac -S native -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-mac -j4

cmake -B cpp/build -S cpp -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j4
```

输出：
- 命令行 / 可执行文件：`cpp/build/bin/ulv_viewer`
- macOS 应用包：`砺见智行激光雷达测试工具.app`（已内置 `plugins/*.dylib`、`corrections/`、`fonts/`）

### 3. Windows x64 交叉编译（macOS 宿主机 via Zig）

```sh
cmake -B native/build-win -S native -DCMAKE_TOOLCHAIN_FILE="$PWD/cpp/cmake/zig-windows-x64-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-win -j4

cmake -B cpp/build-win -S cpp -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows-x64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build-win -j4
```

## 运行测试

```sh
./scripts/run_e2e_tests.sh
```

或单独运行：
- `./cpp/build-tests/bin/ulv_test_suite`（74 项核心单元测试与压力测试）
- `./cpp/build-tests/bin/ulv_ui_tests`（DPI 缩放、回放预览与无头 ImGui 布局测试）
- `./cpp/build/bin/ulv_viewer --test ./cpp/tests/fixtures/111.pcap`（真实 AT128 PCAP 全量解码验证）
