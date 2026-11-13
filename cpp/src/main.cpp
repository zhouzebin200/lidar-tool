#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "ulv_gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "ui/ui_workspace.hpp"
#include "ui/ui_theme.hpp"
#include "ui/ui_layout.hpp"
#include "ui/ui_dpi.hpp"
#include "ui/file_dialog.hpp"
#include "renderer/camera.hpp"
#include "renderer/pointcloud_renderer.hpp"
#include "renderer/grid_renderer.hpp"
#include "renderer/viewport_capture.hpp"
#include "net/pcap_reader.hpp"
#include "net/udp_receiver.hpp"
#include "core/frame_buffer.hpp"
#include "core/replay_preview.hpp"
#include "core/decoder_manager.hpp"
#include "core/ptp_monitor.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <vector>

static std::filesystem::path get_executable_directory() {
#if defined(__APPLE__)
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::error_code ec;
        auto canonical = std::filesystem::canonical(path, ec);
        if (!ec) return canonical.parent_path();
    }
#elif defined(_WIN32)
    wchar_t path[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH * 4);
    if (len > 0 && len < MAX_PATH * 4) {
        std::error_code ec;
        auto canonical = std::filesystem::canonical(std::filesystem::path(path), ec);
        if (!ec) return canonical.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

static float g_ui_coordinate_scale=1.0f;
static GLFWscrollfun g_imgui_scroll_callback = nullptr;
static double g_accumulated_scroll_y = 0.0;

static void glfw_scroll_callback_hook(GLFWwindow* window, double xoffset, double yoffset) {
    g_accumulated_scroll_y += yoffset;
    if (g_imgui_scroll_callback) {
        g_imgui_scroll_callback(window, xoffset, yoffset);
    }
}

static double monotonic_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

static void locate_resource_dirs(const char* argv0, std::string& out_plugins, std::string& out_corrections) {
    auto try_dir = [&](const std::filesystem::path& base) -> bool {
        auto p = (base / "plugins").lexically_normal();
        auto c = (base / "corrections").lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
            out_plugins = p.u8string();
            out_corrections = c.u8string();
            return true;
        }
        return false;
    };

    auto exe_dir = get_executable_directory();
    if (!exe_dir.empty()) {
        if (try_dir(exe_dir)) return;
        if (try_dir(exe_dir.parent_path() / "Resources")) return;
    }

    std::vector<std::filesystem::path> bases;
    bases.push_back(std::filesystem::current_path());
    if (!exe_dir.empty()) bases.push_back(exe_dir);
    if (argv0 && argv0[0] != '\0') {
        std::error_code ec;
        auto exe_path = std::filesystem::weakly_canonical(std::filesystem::u8path(argv0), ec);
        if (!ec && exe_path.has_parent_path()) {
            if (try_dir(exe_path.parent_path().parent_path() / "Resources")) return;
            bases.push_back(exe_path.parent_path());
        }
    }
    for (const auto& base : bases) {
        for (const char* rel : {".", "..", "../..", "../../.."}) {
            if (try_dir(base / rel)) return;
        }
    }
    out_plugins = (std::filesystem::current_path() / "plugins").u8string();
    out_corrections = (std::filesystem::current_path() / "corrections").u8string();
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    // If launched from an existing console (e.g. cmd / powershell for --test), attach to it so CLI output works.
    // If double-clicked from Explorer/Desktop, no console is attached and NO black terminal window opens.
    HANDLE stdout_handle=GetStdHandle(STD_OUTPUT_HANDLE);
    const bool has_redirect=stdout_handle && stdout_handle!=INVALID_HANDLE_VALUE && GetFileType(stdout_handle)!=FILE_TYPE_UNKNOWN;
    if (!has_redirect && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
#endif

    // Check for headless test mode
    bool test_mode = false;
    bool test_live = false;
    std::string test_pcap;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test") {
            test_mode = true;
        } else if (std::string(argv[i]) == "--test-live") {
            test_live = true;
        } else if (argv[i][0] != '-') {
            test_pcap = argv[i];
        }
    }

    if (test_live) {
        std::cout << "[ULV C++ Test Live Mode] Starting live capture verification..." << std::endl;
        ulv::FrameBufferQueue queue;
        ulv::DecoderManager dec(queue);

        std::string p_dir, c_dir;
        locate_resource_dirs(argv[0], p_dir, c_dir);

        if (!dec.load_plugins(p_dir, c_dir)) {
            std::cerr << "[Test Error] Failed to load any plugins from " << p_dir << std::endl;
            return 1;
        }

        ulv::UdpReceiver udp;
        std::atomic<size_t> pkts{0};
        std::atomic<size_t> frames_received{0};
        std::atomic<size_t> max_points{0};
        std::string detected_model;

        bool started = udp.start("0.0.0.0", {2368, 51100, 6668, 8080}, [&](const uint8_t* data, size_t sz, const std::string& src_ip, uint16_t src_p, uint16_t dst_p, uint64_t ts) {
            pkts.fetch_add(1);
            dec.feed_packet(data, sz, src_ip, src_p, dst_p, ts);
        });

        if (!started) {
            std::cerr << "[Test Error] Could not start UDP receiver" << std::endl;
            return 2;
        }

        std::cout << "[Test Live] Listening on 0.0.0.0:[2368, 51100, 6668, 8080] for 3 seconds..." << std::endl;
        auto start_t = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_t).count() < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ulv::PointCloudFrame frame;
            while (queue.pop_latest_frame(frame)) {
                frames_received.fetch_add(1);
                if (frame.points.size() > max_points.load()) {
                    max_points.store(frame.points.size());
                }
                detected_model = frame.model;
                std::cout << "  [Live Frame #" << frame.sequence << "] " << frame.points.size()
                          << " pts, vendor=" << frame.vendor << " " << frame.model
                          << ", max_dist=" << frame.bounds.max_distance << "m" << std::endl;
            }
        }
        udp.stop();

        std::cout << "[ULV C++ Test Live Mode] Success: Received " << pkts.load()
                  << " UDP packets, decoded " << frames_received.load()
                  << " frames, max frame points=" << max_points.load()
                  << ", model=" << detected_model << std::endl;
        return (frames_received.load() > 0) ? 0 : 4;
    }

    if (test_mode) {
        std::cout << "[ULV C++ Test Mode] Running verification..." << std::endl;
        ulv::FrameBufferQueue queue;
        ulv::DecoderManager dec(queue);

        std::string p_dir, c_dir;
        locate_resource_dirs(argv[0], p_dir, c_dir);

        if (!dec.load_plugins(p_dir, c_dir)) {
            std::cerr << "[Test Error] Failed to load any plugins from " << p_dir << std::endl;
            return 1;
        }

        if (test_pcap.empty()) {
            for (const char* candidate : {
                "./cpp/tests/fixtures/111.pcap",
                "./111.pcap"
            }) {
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec)) {
                    test_pcap = candidate;
                    break;
                }
            }
        }

        ulv::PcapReader reader;
        if (!reader.open(test_pcap)) {
            std::cerr << "[Test Error] Could not open test PCAP: " << test_pcap << std::endl;
            return 2;
        }

        size_t packet_count = 0;
        size_t frames_received = 0;
        size_t max_points_in_frame = 0;

        ulv::PcapUdpPacket pkt;
        while (reader.read_next_packet(pkt)) {
            packet_count++;
            dec.feed_packet(pkt.payload.data(), pkt.payload.size(), pkt.src_ip, pkt.src_port, pkt.dst_port, pkt.timestamp_ns);

            ulv::PointCloudFrame frame;
            if (queue.pop_latest_frame(frame)) {
                frames_received++;
                if (frame.points.size() > max_points_in_frame) {
                    max_points_in_frame = frame.points.size();
                }
                if (frames_received <= 3 || frames_received % 10 == 0) {
                    std::cout << "  Frame #" << frame.sequence << ": " << frame.points.size()
                              << " points, vendor=" << frame.vendor << " " << frame.model
                              << ", max_dist=" << frame.bounds.max_distance << "m" << std::endl;
                }
            }
        }

        std::cout << "[ULV C++ Test Mode] Success: Processed " << packet_count
                  << " packets, received " << frames_received << " frames, max frame points="
                  << max_points_in_frame << std::endl;
        return (frames_received > 0) ? 0 : 3;
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW." << std::endl;
        return 1;
    }

    // OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1600, 960, "砺见智行 · 激光雷达测试工作台", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window." << std::endl;
        glfwTerminate();
        return 1;
    }

#if defined(_WIN32)
    float content_x=1,content_y=1;
    glfwGetWindowContentScale(window,&content_x,&content_y);
    g_ui_coordinate_scale=std::max(1.0f,content_x);
#endif
    glfwSetWindowSizeLimits(window, int(1100*g_ui_coordinate_scale), int(720*g_ui_coordinate_scale), GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwMaximizeWindow(window);
    glfwSwapInterval(1); // Enable VSync

#if !defined(__APPLE__)
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD (OpenGL 3.3 Core)." << std::endl;
        return 1;
    }
#endif

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Load native Chinese font (Multi-tier resolution: bundled -> relative -> system fallback)
    std::string font_path;
    auto exe_dir = get_executable_directory();
    std::vector<std::filesystem::path> font_candidates = {
        exe_dir / "resources" / "fonts" / "NotoSansSC-Regular.otf",
        exe_dir / "resources" / "fonts" / "NotoSansSC-Regular.ttf",
        exe_dir.parent_path() / "Resources" / "fonts" / "NotoSansSC-Regular.otf",
        exe_dir.parent_path() / "Resources" / "fonts" / "NotoSansSC-Regular.ttf",
        std::filesystem::current_path() / "resources" / "fonts" / "NotoSansSC-Regular.otf",
        std::filesystem::current_path() / "cpp" / "resources" / "fonts" / "NotoSansSC-Regular.otf"
    };

    for (const auto& candidate : font_candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            font_path = candidate.u8string();
            break;
        }
    }

    if (font_path.empty()) {
#if defined(_WIN32)
        const char* win_fonts[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:\\Windows\\Fonts\\simhei.ttf"
        };
        for (const char* f : win_fonts) {
            if (std::filesystem::exists(f)) { font_path = f; break; }
        }
#elif defined(__APPLE__)
        const char* mac_fonts[] = {
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/STHeiti Medium.ttc",
            "/System/Library/Fonts/PingFang.ttc",
            "/Library/Fonts/Arial Unicode.ttf"
        };
        for (const char* f : mac_fonts) {
            if (std::filesystem::exists(f)) { font_path = f; break; }
        }
#else
        const char* linux_fonts[] = {
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc"
        };
        for (const char* f : linux_fonts) {
            if (std::filesystem::exists(f)) { font_path = f; break; }
        }
#endif
    }

    int font_fb_w=0,font_fb_h=0,font_win_w=0,font_win_h=0;
    glfwGetFramebufferSize(window,&font_fb_w,&font_fb_h);
    glfwGetWindowSize(window,&font_win_w,&font_win_h);
    const float raster_scale=font_win_w>0 ? std::max(g_ui_coordinate_scale,float(font_fb_w)/font_win_w) : 1.0f;
    io.FontGlobalScale=1.0f/raster_scale;
    if (!font_path.empty()) {
        ImFont* loaded_font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f*raster_scale, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (loaded_font) {
            std::cout << "Successfully loaded native Chinese font: " << font_path << std::endl;
        } else {
            std::cerr << "Warning: Failed to load font: " << font_path << ", falling back to default." << std::endl;
            io.Fonts->AddFontDefault();
        }
    } else {
        io.Fonts->AddFontDefault();
    }

    // Apply dark industrial styling
    ulv::UITheme::apply_dark_industrial_theme();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    std::cout << "UI coordinate scale: " << g_ui_coordinate_scale << ", font raster scale: " << raster_scale << std::endl;
    g_imgui_scroll_callback = glfwSetScrollCallback(window, glfw_scroll_callback_hook);
    ImGui_ImplOpenGL3_Init("#version 330");

    ulv::UIDpiAdapter ui_dpi;

    // Initialize 3D Renderers
    ulv::PointCloudRenderer pc_renderer;
    if (!pc_renderer.init()) {
        std::cerr << "Failed to initialize PointCloudRenderer." << std::endl;
        return 1;
    }

    ulv::GridRenderer grid_renderer;
    if (!grid_renderer.init()) {
        std::cerr << "Failed to initialize GridRenderer." << std::endl;
        return 1;
    }

    ulv::OrbitCamera camera;
    ulv::RenderSettings render_settings;
    ulv::PointCloudFrame active_frame;
    ulv::UIWorkspace workspace;

    workspace.set_screenshot_provider([&]() -> std::string {
        return ulv::capture_viewport_png_base64(camera, pc_renderer, grid_renderer, render_settings, &active_frame);
    });

    // Core Frame Queue and Decoder Manager
    ulv::FrameBufferQueue frame_queue;
    ulv::DecoderManager decoder_mgr(frame_queue);

    // Locate plugins and corrections directories
    std::string plugins_dir, corrections_dir;
    locate_resource_dirs(argv[0], plugins_dir, corrections_dir);

    workspace.add_log("插件加载目录: " + plugins_dir);
    if (decoder_mgr.load_plugins(plugins_dir, corrections_dir)) {
        workspace.add_log("成功加载硬件厂商解码插件.");
    } else {
        workspace.add_log("警告: 未检测到可用厂商插件 (将仅显示原始数据包嗅探).");
    }

    // App state
    ulv::PcapReader pcap_reader;
    ulv::PtpMonitor ptp_monitor;
    ulv::UdpReceiver udp_receiver;
    ulv::AppMode app_mode = ulv::AppMode::Idle;
    bool is_playing = false;
    float playback_speed = 1.0f;
    ulv::ReplayPreview replay_preview;
    bool loop_playback = true;
    std::string status_message = "正在等待以太网数据流...";

    double last_time = glfwGetTime();
    double mouse_last_x = 0.0, mouse_last_y = 0.0;

    // Real-time network throughput and point cloud frame rate calculation state
    double last_stats_time = glfwGetTime();
    uint64_t last_packets_received = 0;
    uint64_t last_bytes_received = 0;
    double rolling_pps = 0.0;
    double rolling_mbps = 0.0;

    uint64_t total_pc_frames = 0;
    uint64_t last_pc_frames = 0;
    float rolling_pc_fps = 0.0f;
    double last_pcap_frame_time = 0.0;

    // Helper: start live Ethernet UDP listening
    auto start_live_listening = [&]() {
        replay_preview.reset();
        if (app_mode == ulv::AppMode::PcapReplay || is_playing) {
            is_playing = false;
            pcap_reader.close();
        }
        if (udp_receiver.is_running()) {
            udp_receiver.stop();
        }

        decoder_mgr.reset();
        frame_queue.clear();
        active_frame = ulv::PointCloudFrame{};
        pc_renderer.upload_points(nullptr, 0);
        ptp_monitor.reset();

        std::string listen_ip = workspace.get_listen_ip();
        std::vector<uint16_t> listen_ports = workspace.get_listen_ports();

        last_stats_time = glfwGetTime();
        last_packets_received = 0;
        last_bytes_received = 0;
        rolling_pps = 0.0;
        rolling_mbps = 0.0;
        rolling_pc_fps = 0.0f;
        total_pc_frames = 0;
        last_pc_frames = 0;

        bool started = udp_receiver.start(
            listen_ip,
            listen_ports,
            [&decoder_mgr, &ptp_monitor, &workspace](
                const uint8_t* data,
                size_t size,
                const std::string& src_ip,
                uint16_t src_port,
                uint16_t dst_port,
                uint64_t timestamp_ns
            ) {
                if (!src_ip.empty() && src_ip != "0.0.0.0") {
                    workspace.set_detected_lidar_ip(src_ip);
                }
                if (dst_port == 319 || dst_port == 320) {
                    // Use steady_clock instead of glfwGetTime() because this callback runs on the UDP thread.
                    ptp_monitor.parse_ptp_packet(data, size, dst_port, monotonic_seconds());
                }
                decoder_mgr.feed_packet(data, size, src_ip, src_port, dst_port, timestamp_ns);
            }
        );

        if (started) {
            app_mode = ulv::AppMode::LiveCapture;
            workspace.set_is_listening(true);
            status_message = "正在等待以太网数据流...";

            std::string ports_str;
            for (size_t i = 0; i < listen_ports.size(); ++i) {
                if (i > 0) ports_str += ", ";
                ports_str += std::to_string(listen_ports[i]);
            }
            workspace.add_log("默认启动以太网 UDP 监听: IP=" + listen_ip + ", 端口=[" + ports_str + "]，等待雷达实时数据流...");
        } else {
            app_mode = ulv::AppMode::Idle;
            workspace.set_is_listening(false);
            status_message = "UDP 监听启动失败";
            workspace.add_log("错误: 无法绑定 UDP 端口或启动套接字监听.");
        }
    };

    // If CLI argument specifies a PCAP file, replay that file; otherwise default to live Ethernet listening
    if (argc > 1 && argv[1][0] != '-') {
        std::string pcap_path = argv[1];
        if (pcap_reader.open(pcap_path)) {
            app_mode = ulv::AppMode::PcapReplay;
            is_playing = true;
            status_message = "正在回放 PCAP";
            workspace.add_log("加载命令行指定的 PCAP 文件: " + pcap_path);
        } else {
            start_live_listening();
        }
    } else {
        start_live_listening();
    }

    // Main event loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double current_time = glfwGetTime();
        float delta_time = static_cast<float>(current_time - last_time);
        last_time = current_time;

        // Check if user clicked "Open PCAP" button
        if (workspace.should_open_file_dialog()) {
            workspace.clear_file_dialog_trigger();

            std::string selected_pcap = ulv::show_open_pcap_dialog();
            last_time = glfwGetTime();
            glfwGetCursorPos(window, &mouse_last_x, &mouse_last_y);

            if (!selected_pcap.empty()) {
                // Stop real-time UDP listener if it was running
                if (udp_receiver.is_running()) {
                    udp_receiver.stop();
                    workspace.set_is_listening(false);
                    workspace.add_log("已停止实时 UDP 监听并切换至离线 PCAP 回放.");
                }

                if (pcap_reader.open(selected_pcap)) {
                    replay_preview.reset();
                    // Flush old decoder plugin instance and identification cache
                    decoder_mgr.reset();

                    // Flush any pending frames from previous playback
                    frame_queue.clear();

                    // Reset active point cloud frame and clear rendered viewport
                    active_frame = ulv::PointCloudFrame{};
                    pc_renderer.upload_points(nullptr, 0);

                    // Reset PTP sync statistics
                    ptp_monitor.reset();

                    app_mode = ulv::AppMode::PcapReplay;
                    if (pcap_reader.total_packets() > 0) {
                        is_playing = true;
                        status_message = "正在回放 PCAP";
                        workspace.add_log("成功导入 PCAP 文件: " + selected_pcap +
                                          " (共 " + std::to_string(pcap_reader.total_packets()) + " 个数据包)");
                    } else {
                        is_playing = false;
                        status_message = "PCAP 无有效报文";
                        workspace.add_log("警告: PCAP 文件未包含有效 UDP 数据包: " + selected_pcap);
                    }
                } else {
                    app_mode = ulv::AppMode::Idle;
                    is_playing = false;
                    status_message = "PCAP 打开失败";
                    workspace.add_log("错误: 无法解析 PCAP 文件 (文件损坏或非标准以太网 UDP PCAP 格式): " + selected_pcap);
                }
            }
        }

        // Check if user clicked "Start Listening" button
        if (workspace.should_start_listening()) {
            workspace.clear_listen_triggers();
            start_live_listening();
        }

        // Check if user clicked "Stop Listening" button
        if (workspace.should_stop_listening()) {
            workspace.clear_listen_triggers();

            if (udp_receiver.is_running()) {
                udp_receiver.stop();
            }

            app_mode = ulv::AppMode::Idle;
            workspace.set_is_listening(false);
            status_message = "就绪 (监听已停止)";

            rolling_pps = 0.0;
            rolling_mbps = 0.0;
            last_packets_received = 0;
            last_bytes_received = 0;
            rolling_pc_fps = 0.0f;
            workspace.set_live_network_stats(false, 0, 0, 0.0, 0.0, 0.0f);

            workspace.add_log("已停止以太网 UDP 实时监听.");
        }

        // PCAP Playback logic (accurately paced at 10.0 Hz LiDAR frame rate for 1.0x speed)
        if (app_mode == ulv::AppMode::PcapReplay && (is_playing || replay_preview.pending()) && pcap_reader.is_open()) {
            double target_frame_interval = 0.100 / static_cast<double>(std::max(0.1f, playback_speed));
            if (replay_preview.pending() || current_time - last_pcap_frame_time >= target_frame_interval) {
                last_pcap_frame_time = current_time;

                int safety_limit = 10000;
                while (safety_limit-- > 0) {
                    ulv::PcapUdpPacket packet;
                    if (pcap_reader.read_next_packet(packet)) {
                        if (!packet.src_ip.empty() && packet.src_ip != "0.0.0.0") {
                            workspace.set_detected_lidar_ip(packet.src_ip);
                        }
                        if (packet.dst_port == 319 || packet.dst_port == 320) {
                            ptp_monitor.parse_ptp_packet(packet.payload.data(), packet.payload.size(), packet.dst_port, monotonic_seconds());
                        }
                        decoder_mgr.feed_packet(
                            packet.payload.data(),
                            packet.payload.size(),
                            packet.src_ip,
                            packet.src_port,
                            packet.dst_port,
                            packet.timestamp_ns
                        );
                        if (frame_queue.has_new_frame()) {
                            replay_preview.observe(true,false);
                            break; // Successfully generated 1 full 360-degree point cloud frame
                        }
                    } else {
                        replay_preview.observe(false,true);
                        if(replay_preview.exhausted() && !is_playing) {
                            status_message="目标位置不足完整扫描帧，保留上一点云帧";
                            break;
                        }
                        if (loop_playback) {
                            pcap_reader.rewind();
                            workspace.add_log("PCAP 回放到达末尾，重新循环播放.");
                        } else {
                            is_playing = false;
                            workspace.add_log("PCAP 回放完毕.");
                        }
                        break;
                    }
                }
            }
        }

        // Check for new decoded point cloud frame
        if (frame_queue.pop_latest_frame(active_frame)) {
            total_pc_frames++;
            if (app_mode == ulv::AppMode::LiveCapture) {
                status_message = "实时接收中: " + active_frame.vendor + " " + active_frame.model;
            }
            // Adaptive dynamic range estimation (EMA smoothed, matching web app.js)
            if (render_settings.auto_range && !active_frame.points.empty()) {
                const auto& pts = active_frame.points;
                size_t count = pts.size();
                float z_min = 1e9f, z_max = -1e9f;
                float int_max = 0.0f;
                size_t stride = std::max<size_t>(1, count / 4096);
                for (size_t i = 0; i < count; i += stride) {
                    float z = pts[i].z;
                    float inten = pts[i].intensity;
                    if (std::isfinite(z)) {
                        if (z < z_min) z_min = z;
                        if (z > z_max) z_max = z;
                    }
                    if (std::isfinite(inten) && inten > int_max) {
                        int_max = inten;
                    }
                }
                if (z_max > z_min) {
                    float margin = std::max(0.05f, (z_max - z_min) * 0.03f);
                    render_settings.current_z_min = render_settings.current_z_min * 0.8f + (z_min - margin) * 0.2f;
                    render_settings.current_z_max = render_settings.current_z_max * 0.8f + (z_max + margin) * 0.2f;
                }
                if (int_max > 0.01f) {
                    render_settings.current_intensity_max = render_settings.current_intensity_max * 0.8f + int_max * 0.2f;
                }
            }

            pc_renderer.upload_points(active_frame.points.data(), active_frame.points.size());
        }

        // Periodic throughput and point cloud frame rate calculation (0.5-second rolling window)
        double stats_elapsed = current_time - last_stats_time;
        if (stats_elapsed >= 0.5) {
            uint64_t delta_pc = (total_pc_frames >= last_pc_frames) ? (total_pc_frames - last_pc_frames) : 0;
            if (app_mode == ulv::AppMode::PcapReplay && !is_playing) {
                rolling_pc_fps = 0.0f;
            } else {
                rolling_pc_fps = static_cast<float>(static_cast<double>(delta_pc) / stats_elapsed);
            }
            last_pc_frames = total_pc_frames;

            if (udp_receiver.is_running()) {
                uint64_t cur_packets = udp_receiver.packets_received();
                uint64_t cur_bytes = udp_receiver.bytes_received();

                uint64_t delta_packets = (cur_packets >= last_packets_received) ? (cur_packets - last_packets_received) : 0;
                uint64_t delta_bytes = (cur_bytes >= last_bytes_received) ? (cur_bytes - last_bytes_received) : 0;

                rolling_pps = static_cast<double>(delta_packets) / stats_elapsed;
                rolling_mbps = (static_cast<double>(delta_bytes) * 8.0) / (stats_elapsed * 1000000.0);

                last_packets_received = cur_packets;
                last_bytes_received = cur_bytes;
            } else {
                rolling_pps = 0.0;
                rolling_mbps = 0.0;
                last_packets_received = 0;
                last_bytes_received = 0;
            }
            last_stats_time = current_time;
        }

        // Push real-time network throughput and point cloud FPS into workspace
        uint64_t cur_pkts = udp_receiver.is_running() ? udp_receiver.packets_received() : 0;
        uint64_t cur_bytes = udp_receiver.is_running() ? udp_receiver.bytes_received() : 0;
        workspace.set_live_network_stats(udp_receiver.is_running(), cur_pkts, cur_bytes, rolling_pps, rolling_mbps, rolling_pc_fps);

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ui_dpi.before_frame(g_ui_coordinate_scale);
        ImGui::NewFrame();


        // Render UI workspace
        workspace.render_ui(
            render_settings,
            camera,
            pcap_reader,
            active_frame,
            app_mode,
            is_playing,
            playback_speed,
            loop_playback,
            status_message,
            ptp_monitor.status(monotonic_seconds()),
            rolling_pc_fps
        );

        // UI consumes mouse events before camera routing.
        // Mouse Camera Controls
        double mouse_x, mouse_y;
        glfwGetCursorPos(window, &mouse_x, &mouse_y);
        mouse_x/=g_ui_coordinate_scale; mouse_y/=g_ui_coordinate_scale;
        float mouse_dx = static_cast<float>(mouse_x - mouse_last_x);
        float mouse_dy = static_cast<float>(mouse_y - mouse_last_y);
        mouse_last_x = mouse_x;
        mouse_last_y = mouse_y;

        int win_w = 0, win_h = 0;
        glfwGetWindowSize(window, &win_w, &win_h);
        win_w=int(win_w/g_ui_coordinate_scale); win_h=int(win_h/g_ui_coordinate_scale);

        // 3D Viewport bounding rectangle (between left panel 265px and right panel 320px, below top bar 62px)
        bool in_3d_viewport = (mouse_x >= ulv::WorkspaceLayout::left && mouse_x <= (win_w - ulv::WorkspaceLayout::right) && mouse_y >= ulv::WorkspaceLayout::top && mouse_y < win_h - ulv::WorkspaceLayout::status);
        bool modal_active = workspace.is_report_modal_open();

        if (in_3d_viewport && !modal_active && !workspace.blocks_camera_input()) {
            // High-precision smooth zoom via scroll wheel or trackpad
            if (std::abs(g_accumulated_scroll_y) > 0.0001) {
                workspace.notify_camera_moved();
                camera.zoom(static_cast<float>(g_accumulated_scroll_y));
            }

            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                workspace.notify_camera_moved();
                if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
                    camera.pan(mouse_dx, mouse_dy);
                } else {
                    camera.rotate(mouse_dx, mouse_dy);
                }
            } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                       glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS) {
                workspace.notify_camera_moved();
                camera.pan(mouse_dx, mouse_dy);
            }
        }
        g_accumulated_scroll_y = 0.0;


        size_t seek_index=0;
        if (workspace.take_seek_request(seek_index) && app_mode == ulv::AppMode::PcapReplay && pcap_reader.seek_to_index(seek_index)) {
            decoder_mgr.reset(); frame_queue.clear(); ptp_monitor.reset();
            replay_preview.request();
        }

        // Rendering
        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);

        // Graphite & Ice Blue 3D viewport background (#0C1118)
        glClearColor(0.047f, 0.067f, 0.094f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render 3D Scene
        float aspect = (display_h > 0) ? (static_cast<float>(display_w) / static_cast<float>(display_h)) : 1.0f;
        ulv::Mat4 mvp = camera.get_projection_matrix(aspect) * camera.get_view_matrix();

        grid_renderer.render(mvp);
        pc_renderer.render(mvp, render_settings);

        // Render ImGui over 3D scene
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    if (udp_receiver.is_running()) {
        udp_receiver.stop();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    pc_renderer.cleanup();
    grid_renderer.cleanup();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
