# Project: Universal LiDAR Viewer C++ Native Production Readiness

## Architecture
- **Language & Runtime**: Pure C++17, zero Python/Node/Web runtime dependencies.
- **UI & Graphics**: Dear ImGui (Docking/Navigation) + OpenGL 3.3 Core Profile + GLFW.
- **Networking**: Non-blocking sockets (`poll` on macOS/Linux, `WSAPoll` on Windows) supporting multi-port UDP ingestion and TCP PTC client.
- **Decoder Pipeline**: Vendor plugin shared libraries (`libulv_hesai`, `libulv_robosense`, `libulv_velodyne`, `libulv_huawei_l306`), thread-safe `DecoderManager`, and thread-safe `FrameBufferQueue`.
- **Diagnostics & Reporting**: `HesaiPtcClient` (TCP 9347 / HTTP 80 fallback) auto-populating `InspectionReportData`, with offline 3D viewport FBO screenshot capture (`stb_image_write` PNG + Base64) embedded in self-contained HTML/PDF reports.
- **Build Targets**: macOS arm64 native (`cmake --build cpp/build`) and Windows x64 cross-compilation (`cmake --build cpp/build-win` via Zig).

## Code Layout
```
cpp/
├── CMakeLists.txt                       # Root build configuration
├── cmake/
│   ├── zig-windows-x64-toolchain.cmake  # Windows x64 cross-toolchain
│   └── zig-*-win.sh                     # Zig wrapper scripts
├── src/
│   ├── main.cpp                         # Main application lifecycle & render loop
│   ├── core/
│   │   ├── decoder_manager.hpp/.cpp     # Decoder lifecycle & packet dispatch (needs mutex)
│   │   ├── frame_buffer.hpp             # Thread-safe double-buffered frame queue
│   │   ├── report_generator.hpp/.cpp    # Inspection report data model & HTML/PDF exporter
│   │   └── pcap_reader.hpp/.cpp         # PCAP/PCAPNG file parser
│   ├── net/
│   │   ├── udp_receiver.hpp/.cpp        # Multi-port non-blocking UDP receiver
│   │   └── ptc_client.hpp/.cpp          # NEW: LiDAR PTC 9347 & Web diagnostic client
│   ├── renderer/
│   │   ├── camera.hpp                   # Orbit camera
│   │   ├── pointcloud_renderer.hpp/.cpp # 3D point cloud VAO/VBO shader pipeline
│   │   ├── grid_renderer.hpp/.cpp       # Ground grid and range ring renderer
│   │   └── viewport_capture.hpp/.cpp    # NEW: 3D viewport FBO screenshot & PNG encoder
│   └── ui/
│       ├── ui_workspace.hpp/.cpp        # ImGui workspace layout & panels
│       └── file_dialog.hpp/.cpp         # NEW: Cross-platform native file dialog
└── third_party/
    ├── glad/                            # OpenGL loader
    ├── glfw/                            # Window & context management
    ├── imgui/                           # Immediate mode GUI
    └── stb/stb_image_write.h            # Single-header PNG encoder
```

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---|---|---|---|
| 1 | Native File Dialog | Cross-platform native file selection dialog (Win32 `GetOpenFileNameA` / macOS `osascript` or Cocoa) invoked on "导入 PCAP 录包文件" | M1 | ORIGINAL_REQUEST §R1 |
| 2 | Seamless PCAP Loading | Load selected PCAP, reset decoder and timeline, switch to `PcapReplay` mode, and start visualization | M1 | ORIGINAL_REQUEST §R1 |
| 3 | Real-time UDP UI Controls | Network Settings panel functional controls ("开始实时监听" / "停止监听"), IP & port list inputs with vendor presets | M2 | ORIGINAL_REQUEST §R2 |
| 4 | Thread-safe Packet Ingestion | Feed UDP packets directly into `DecoderManager` & `PtpMonitor` with internal mutex protection | M2 | ORIGINAL_REQUEST §R2 |
| 5 | Real-time Throughput Stats | 1s rolling calculation of PPS and Mbps displayed in Network panel and top-bar status badges | M2 | ORIGINAL_REQUEST §R2 |
| 6 | LiDAR PTC 9347 Client | Pure C++ TCP 9347 client sending commands `0x07`, `0x09`, `0x27` with 1.5s timeout & HTTP fallback | M3 | ORIGINAL_REQUEST §R3 |
| 7 | Diagnostic Parameter Parsing | Parse Serial Number, Firmware Version, Motor RPM, and Temperatures from PTC binary responses | M3 | ORIGINAL_REQUEST §R3 |
| 8 | Report Studio Parameter Auto-Fill | Asynchronous "一键采集雷达参数" in Report Studio populating `report_data_` and observation notes | M3 | ORIGINAL_REQUEST §R3 |
| 9 | 3D Viewport Screenshot Capture | Offscreen FBO (1280x720 16:9) capture of point cloud rendering, avoiding ImGui modal occlusions | M4 | ORIGINAL_REQUEST §R4 |
| 10 | PNG & Base64 Encoding | Memory PNG compression via `stb_image_write.h` and pure C++17 Base64 Data URI generation | M4 | ORIGINAL_REQUEST §R4 |
| 11 | HTML Report Embedding & Printing | Embed Base64 screenshot into Section 5 of HTML report, auto-open in browser, and support `window.print()` | M4 | ORIGINAL_REQUEST §R4 |
| 12 | macOS arm64 Clean Compilation | Zero-error compilation of `ulv_viewer` on macOS arm64 | M5 | ORIGINAL_REQUEST §R5 |
| 13 | Windows x64 Cross-Compilation | Zero-error cross-compilation of `ulv_viewer.exe` on Windows x64 via Zig | M5 | ORIGINAL_REQUEST §R5 |
| 14 | Headless Test Verification | Regression verification using `./cpp/build/bin/ulv_viewer --test /Users/zzb/Desktop/111.pcap` | M5 | ORIGINAL_REQUEST §R5 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Native System File Dialog | Implement `file_dialog.hpp/.cpp`, hook to "导入 PCAP 录包文件", load PCAP without hardcoded paths | none | DONE |
| M2 | Real-time UDP Capture UI & Pipeline | Add UI controls to Network Settings panel, link `UdpReceiver` to `DecoderManager` with mutex, update live stats | M1 | DONE |
| M3 | Automated Radar PTC 9347 Parameter Acquisition | Implement `ptc_client.hpp/.cpp`, async query in Report Studio, auto-populate `report_data_` | M2 | DONE |
| M4 | Viewport Screenshot & Report Embedding | Implement `viewport_capture.hpp/.cpp`, vendor `stb_image_write.h`, embed Base64 PNG in HTML report | M3 | DONE |
| M5 | Cross-Platform Build & Verification | Harden CMakeLists.txt and Zig scripts, verify macOS native build, Windows cross-compilation, and headless test | M1, M2, M3, M4 | DONE |

## Interface Contracts

### M1: File Dialog Contract
```cpp
namespace ulv {
// Shows native file picker dialog. Returns absolute path or empty string if cancelled.
std::string show_open_pcap_dialog();

// Opens file in OS default viewer.
void open_file_in_system(const std::string& filepath);
}
```

### M2: UDP Capture & Decoder Contract
```cpp
// In DecoderManager:
class DecoderManager {
public:
    void feed_packet(const uint8_t* data, size_t size,
                     const std::string& src_ip, uint16_t src_port,
                     uint16_t dst_port, uint64_t timestamp_ns);
    void reset();
private:
    std::mutex mutex_; // Protects active_plugin_ and active_instance_
};
```

### M3: PTC Diagnostic Client Contract
```cpp
namespace ulv {
struct LidarDiagnosticParams {
    bool success = false;
    std::string error_msg;
    std::string model;
    std::string serial_number;
    std::string product_number;
    std::string date_of_manufacture;
    std::string mac_address;
    std::string ip_address;
    std::string firmware_version;
    float motor_rpm = 0.0f;
    float temperature_c = 0.0f;
    float operating_voltage = 0.0f;
    float operating_current_ma = 0.0f;
};

class HesaiPtcClient {
public:
    static LidarDiagnosticParams query(const std::string& ip, uint16_t port = 9347, int timeout_ms = 1500);
};
}
```

### M4: Viewport Capture Contract
```cpp
namespace ulv {
// Captures 3D viewport rendered frame using offscreen FBO or screen read pixels,
// returns "data:image/png;base64,..." URI string.
std::string capture_viewport_png_base64(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    int width = 1280,
    int height = 720
);
}
```
