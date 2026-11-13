#pragma once

#include "pointcloud_renderer.hpp"
#include "camera.hpp"
#include "pcap_reader.hpp"
#include "frame_buffer.hpp"
#include "detector.hpp"
#include "ptp_monitor.hpp"
#include "report_generator.hpp"
#include "ptc_client.hpp"

#include <string>
#include <vector>
#include <memory>
#include <future>
#include <chrono>
#include <mutex>
#include <functional>

namespace ulv {

enum class AppMode {
    Idle,
    PcapReplay,
    LiveCapture
};

enum class MainTab {
    PointCloud,
    NetworkSettings,
    PtpSettings,
    ModelSupport
};

class UIWorkspace {
public:
    UIWorkspace();
    ~UIWorkspace();

    void render_ui(
        RenderSettings& render_settings,
        OrbitCamera& camera,
        PcapReader& pcap_reader,
        const PointCloudFrame& active_frame,
        AppMode& app_mode,
        bool& is_playing,
        float& playback_speed,
        bool& loop_playback,
        std::string& status_message,
        const PtpStatus& ptp_status,
        float fps
    );

    bool should_open_file_dialog() const { return trigger_open_file_; }
    void clear_file_dialog_trigger() { trigger_open_file_ = false; }

    // UDP Network Listening triggers and getters/setters
    bool should_start_listening() const { return trigger_start_listen_; }
    bool should_stop_listening() const { return trigger_stop_listen_; }
    void clear_listen_triggers() {
        trigger_start_listen_ = false;
        trigger_stop_listen_ = false;
    }

    std::string get_listen_ip() const;
    std::vector<uint16_t> get_listen_ports() const;

    void set_is_listening(bool listening) { is_listening_ = listening; }
    bool is_listening() const { return is_listening_; }

    void set_live_network_stats(bool is_listening, uint64_t packets, uint64_t bytes, double pps, double mbps, float fps) {
        is_listening_ = is_listening;
        live_packets_ = packets;
        live_bytes_ = bytes;
        live_pps_ = pps;
        live_mbps_ = mbps;
        live_fps_ = fps;
    }

    void set_live_stats(bool is_listening, double pps, double mbps, float fps) {
        is_listening_ = is_listening;
        live_pps_ = pps;
        live_mbps_ = mbps;
        live_fps_ = fps;
    }

    void add_log(const std::string& msg);

    // Target LiDAR IP for diagnostic telemetry & automated parameter acquisition
    void set_detected_lidar_ip(const std::string& ip);
    std::string get_target_lidar_ip() const;
    bool is_querying_diagnostics() const;

    void apply_diagnostic_params(const LidarDiagnosticParams& params);
    const InspectionReportData& get_report_data() const { return report_data_; }
    InspectionReportData& get_report_data_mut() { return report_data_; }

    bool blocks_camera_input() const;

    void notify_camera_moved() { view_is_preset_ = false; }

    bool take_seek_request(size_t& index) {
        if (!trigger_seek_) return false;
        index = seek_index_; trigger_seek_ = false; return true;
    }

    bool is_report_modal_open() const { return show_report_modal_; }

    using ScreenshotProvider = std::function<std::string()>;
    void set_screenshot_provider(ScreenshotProvider provider) {
        screenshot_provider_ = std::move(provider);
    }

private:
    MainTab current_tab_ = MainTab::PointCloud;
    bool trigger_open_file_ = false;
    bool trigger_start_listen_ = false;
    bool trigger_stop_listen_ = false;
    bool is_listening_ = false;
    bool trigger_seek_ = false;
    size_t seek_index_ = 0;
    std::string diagnostic_ip_, diagnostic_serial_, diagnostic_firmware_;
    bool view_is_preset_ = true;
    ViewPreset selected_view_ = ViewPreset::Isometric;

    char listen_ip_buf_[64] = "0.0.0.0";
    char listen_ports_buf_[128] = "2368, 51100, 6668, 8080";

    uint64_t live_packets_ = 0;
    uint64_t live_bytes_ = 0;
    double live_pps_ = 0.0;
    double live_mbps_ = 0.0;
    float live_fps_ = 0.0;

    bool show_report_modal_ = false;
    int selected_report_section_ = 0; // 0=基本信息, 1..8=八大检测项, 9=综合意见
    std::vector<std::string> logs_;
    bool autoscroll_logs_ = true;

    InspectionReportData report_data_;
    std::string report_status_msg_;
    ScreenshotProvider screenshot_provider_;
    bool auto_open_in_browser_ = true;

    // Target LiDAR IP and Async Parameter Acquisition State
    char target_lidar_ip_buf_[64] = "192.168.1.201";
    std::string last_detected_ip_ = "192.168.1.201";
    mutable std::mutex detected_ip_mutex_;

    enum class DiagnosticQueryStatus {
        Idle,
        Querying,
        Success,
        Failed
    };
    DiagnosticQueryStatus query_status_ = DiagnosticQueryStatus::Idle;
    std::string query_status_msg_;
    std::future<LidarDiagnosticParams> query_future_;
    std::chrono::steady_clock::time_point query_start_time_;

    void poll_diagnostic_query();

    // UI Components
    void draw_top_bar(
        const PointCloudFrame& frame,
        AppMode mode,
        float fps,
        const PtpStatus& ptp_status
    );

    void draw_devices_panel(
        float width,
        float height,
        const PointCloudFrame& frame,
        const PcapReader& pcap_reader,
        AppMode mode
    );

    void draw_viewport_overlays(
        float left,
        float top,
        float width,
        float height,
        const PointCloudFrame& frame,
        AppMode& mode,
        PcapReader& pcap_reader,
        bool& is_playing,
        float& playback_speed,
        const RenderSettings& settings
    );

    void draw_controls_panel(
        float width,
        float height,
        RenderSettings& settings,
        OrbitCamera& camera,
        PcapReader& pcap_reader,
        const PointCloudFrame& frame,
        AppMode& app_mode,
        bool& is_playing,
        float& playback_speed,
        bool& loop_playback,
        const PtpStatus& ptp_status
    );

    void draw_report_modal(const PointCloudFrame& frame);

    // Right sidebar subtabs
    void draw_render_tab(
        RenderSettings& settings,
        OrbitCamera& camera,
        PcapReader& pcap_reader,
        const PointCloudFrame& frame,
        AppMode& app_mode,
        bool& is_playing,
        float& playback_speed,
        bool& loop_playback
    );

    void draw_network_tab();
    void draw_ptp_tab(const PtpStatus& ptp_status);
    void draw_support_tab();
};

} // namespace ulv
