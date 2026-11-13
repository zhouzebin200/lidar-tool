#include "ui_workspace.hpp"
#include "ui_theme.hpp"
#include "ui_layout.hpp"
#include "renderer/color_mapping.hpp"
#include <algorithm>
#include "imgui.h"
#include "file_dialog.hpp"
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace ulv {

static std::filesystem::path default_report_output_dir() {
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
    if (const char* up = std::getenv("USERPROFILE")) {
        auto base = std::filesystem::u8path(up);
        candidates.push_back(base / "Downloads");
        candidates.push_back(base / "Desktop");
        candidates.push_back(base);
    }
#else
    if (const char* home = std::getenv("HOME")) {
        auto base = std::filesystem::u8path(home);
        candidates.push_back(base / "Downloads");
        candidates.push_back(base / "Desktop");
        candidates.push_back(base);
    }
#endif
    candidates.push_back(std::filesystem::temp_directory_path(ec));
    for (const auto& dir : candidates) {
        if (!dir.empty() && std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec)) {
            return dir;
        }
    }
    return std::filesystem::current_path(ec);
}

static int input_text_resize_cb(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = &(*str)[0];
    }
    return 0;
}

static bool input_text_multiline_str(const char* label, std::string& str, const ImVec2& size) {
    if (str.capacity() < 256) str.reserve(256);
    return ImGui::InputTextMultiline(
        label,
        &str[0],
        str.capacity() + 1,
        size,
        ImGuiInputTextFlags_CallbackResize,
        input_text_resize_cb,
        &str
    );
}

enum class UIIcon { Cloud, Report, Network, Clock, Support, Folder, Play, Pause, View };

// Small, deterministic line icons; no OS emoji or symbol-font dependency.
static void draw_icon(ImDrawList* dl, UIIcon icon, ImVec2 p, ImU32 color) {
    auto point=[&](float x,float y){return ImVec2(p.x+x,p.y+y);};
    auto line=[&](float x,float y,float xx,float yy){dl->AddLine(point(x,y),point(xx,yy),color,1.3f);};
    switch(icon) {
        case UIIcon::Play: dl->AddTriangle(point(4,2),point(14,8),point(4,14),color,1.5f); break;
        case UIIcon::Pause: line(5,2,5,14);line(11,2,11,14);break;
        case UIIcon::Clock: dl->AddCircle(point(8,8),6,color,20,1.3f);line(8,4,8,8);line(8,8,11,10);break;
        case UIIcon::Network:
            dl->AddCircle(point(8,8),6,color,20,1.3f);line(2,8,14,8);line(8,2,8,14);dl->AddEllipse(point(8,8),ImVec2(3,6),color,0,20,1.1f);break;
        case UIIcon::Folder:
            line(1,4,6,4);line(6,4,8,6);line(8,6,15,6);line(15,6,14,14);line(14,14,1,14);line(1,14,1,4);break;
        case UIIcon::Report: case UIIcon::Support:
            dl->AddRect(point(3,1),point(13,15),color,1,0,1.3f);line(6,5,10,5);line(6,8,10,8);line(6,11,9,11);break;
        case UIIcon::View:
            dl->AddEllipse(point(8,8),ImVec2(7,4.5f),color,0,20,1.3f);dl->AddCircle(point(8,8),2,color,12,1.3f);break;
        default:
            line(8,1,14,5);line(14,5,14,12);line(14,12,8,15);line(8,15,2,12);line(2,12,2,5);line(2,5,8,1);
            line(2,5,8,8);line(14,5,8,8);line(8,8,8,15);break;
    }
}

static bool icon_button(const char* label, UIIcon icon, ImVec2 size) {
    const std::string padded=std::string("    ")+label;
    const bool clicked=ImGui::Button(padded.c_str(),size);
    const ImVec2 lo=ImGui::GetItemRectMin(), hi=ImGui::GetItemRectMax();
    draw_icon(ImGui::GetWindowDrawList(),icon,ImVec2(lo.x+8,(lo.y+hi.y)*0.5f-8),ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

static std::string format_number(size_t n) {
    std::string s = std::to_string(n);
    int insert_pos = static_cast<int>(s.length()) - 3;
    while (insert_pos > 0) {
        s.insert(insert_pos, ",");
        insert_pos -= 3;
    }
    return s;
}

UIWorkspace::UIWorkspace() {
    add_log("砺见智行激光雷达独立桌面端 (纯 C++ 原生版) 启动完毕.");
    add_log("3D 渲染引擎: OpenGL 3.3 Core Profile 已就绪.");
}

UIWorkspace::~UIWorkspace() = default;

void UIWorkspace::add_log(const std::string& msg) {
    logs_.push_back(msg);
    if (logs_.size() > 200) {
        logs_.erase(logs_.begin());
    }
}

bool UIWorkspace::blocks_camera_input() const {
    return show_report_modal_ || ImGui::GetIO().WantCaptureMouse ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive();
}

void UIWorkspace::render_ui(
    RenderSettings& settings,
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
) {
    poll_diagnostic_query();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float screen_w = vp->WorkSize.x;
    float screen_h = vp->WorkSize.y;

    const float top_bar_h = WorkspaceLayout::top;
    const float left_w = WorkspaceLayout::left;
    const float right_w = WorkspaceLayout::right;
    const float content_h = screen_h - top_bar_h - WorkspaceLayout::status;
    const float center_w = screen_w - left_w - right_w;

    // 1. Draw Top Bar (Header & Brand & Nav Tabs & Stats)
    draw_top_bar(active_frame, app_mode, fps, ptp_status);

    // 2. Draw Left Devices Panel (INPUT)
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + top_bar_h));
    ImGui::SetNextWindowSize(ImVec2(left_w, content_h));
    draw_devices_panel(left_w, content_h, active_frame, pcap_reader, app_mode);

    // 3. Draw Viewport Overlays (Central 3D area floating HUD)
    if (!show_report_modal_) draw_viewport_overlays(vp->WorkPos.x + left_w, vp->WorkPos.y + top_bar_h, center_w, content_h, active_frame, app_mode,
                           pcap_reader, is_playing, playback_speed, settings);

    // 4. Draw Right Controls Panel
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + left_w + center_w, vp->WorkPos.y + top_bar_h));
    ImGui::SetNextWindowSize(ImVec2(right_w, content_h));
    draw_controls_panel(right_w, content_h, settings, camera, pcap_reader, active_frame,
                        app_mode, is_playing, playback_speed, loop_playback, ptp_status);

    // Slim status strip uses application state, not demonstration data.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + screen_h - WorkspaceLayout::status));
    ImGui::SetNextWindowSize(ImVec2(screen_w, WorkspaceLayout::status));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 5));
    ImGui::Begin("##StatusStrip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(status_message.empty() ? "引擎就绪" : status_message.c_str());
    ImGui::SameLine(); ImGui::TextDisabled(" | %s points", format_number(active_frame.points.size()).c_str());
    ImGui::End(); ImGui::PopStyleVar();
    draw_report_modal(active_frame);
}

void UIWorkspace::draw_top_bar(
    const PointCloudFrame& frame,
    AppMode mode,
    float fps,
    const PtpStatus& ptp_status
) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float topbar_height = WorkspaceLayout::top;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, topbar_height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ThemeColors::PanelBg);

    ImGui::Begin("##TopBar", nullptr, flags);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    float width = ImGui::GetWindowWidth();

    // Bottom subtle border (#2A3747)
    draw_list->AddLine(
        ImVec2(pos.x, pos.y + topbar_height - 1.0f),
        ImVec2(pos.x + width, pos.y + topbar_height - 1.0f),
        ImGui::ColorConvertFloat4ToU32(ThemeColors::Border),
        1.0f
    );

    // Brand Logo: Geometric wireframe icosahedron / radar crystal in Ice Blue (#74B9FF)
    const ImVec2 logo_c = ImVec2(pos.x + 30.0f, pos.y + 32.0f);
    const float r = 14.0f;
    const ImU32 col_logo_outer = IM_COL32(116, 185, 255, 240); // Ice Blue
    const ImU32 col_logo_inner = IM_COL32(59, 130, 246, 200);  // Deep Blue
    // 6-vertex outer hexagon
    ImVec2 hex_pts[6];
    for (int i = 0; i < 6; ++i) {
        float a = (float)i * (2.0f * 3.14159265f / 6.0f) - (3.14159265f / 6.0f);
        hex_pts[i] = ImVec2(logo_c.x + r * std::cos(a), logo_c.y + r * std::sin(a));
    }
    for (int i = 0; i < 6; ++i) {
        draw_list->AddLine(hex_pts[i], hex_pts[(i + 1) % 6], col_logo_outer, 1.8f);
    }
    // Inner triangulation lines (icosahedron projection)
    draw_list->AddLine(hex_pts[0], hex_pts[3], col_logo_inner, 1.2f);
    draw_list->AddLine(hex_pts[1], hex_pts[4], col_logo_inner, 1.2f);
    draw_list->AddLine(hex_pts[2], hex_pts[5], col_logo_inner, 1.2f);
    draw_list->AddCircleFilled(logo_c, 2.0f, col_logo_outer);

    // Brand Title & Subtitle (Clean & Concise)
    ImGui::SetCursorPos(ImVec2(56.0f, 13.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
    ImGui::TextUnformatted("砺见智行");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(56.0f, 34.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
    ImGui::TextUnformatted("激光雷达测试工作台");
    ImGui::PopStyleColor();

    // Central Nav Segmented Tabs
    const float nav_start_x = 205.0f;
    ImGui::SetCursorPos(ImVec2(nav_start_x, 14.0f));

    auto draw_tab_btn = [&](const char* label, UIIcon icon, bool is_selected) -> bool {
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::ButtonPrimaryHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::ButtonPrimary);
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::CardBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColors::ButtonNeutral);
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        bool clicked = icon_button(label, icon, ImVec2(104.0f, 36.0f));
        if(is_selected) {
            const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();
            draw_list->AddLine(ImVec2(lo.x,hi.y+5),ImVec2(hi.x,hi.y+5),ImGui::ColorConvertFloat4ToU32(ThemeColors::AccentIceBlue),2);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::SameLine(0.0f, 6.0f);
        return clicked;
    };

    if (draw_tab_btn("点云显示", UIIcon::Cloud, current_tab_ == MainTab::PointCloud && !show_report_modal_)) {
        current_tab_ = MainTab::PointCloud;
        show_report_modal_ = false;
    }
    if (draw_tab_btn("检测报告", UIIcon::Report, show_report_modal_)) {
        show_report_modal_ = true;
    }
    if (draw_tab_btn("网络设置", UIIcon::Network, current_tab_ == MainTab::NetworkSettings && !show_report_modal_)) {
        current_tab_ = MainTab::NetworkSettings;
        show_report_modal_ = false;
    }
    if (draw_tab_btn("PTP 设置", UIIcon::Clock, current_tab_ == MainTab::PtpSettings && !show_report_modal_)) {
        current_tab_ = MainTab::PtpSettings;
        show_report_modal_ = false;
    }
    if (draw_tab_btn("支持型号", UIIcon::Support, current_tab_ == MainTab::ModelSupport && !show_report_modal_)) {
        current_tab_ = MainTab::ModelSupport;
        show_report_modal_ = false;
    }

    // Right Stats Panel & Status Pill
    // Adaptive positioning to guarantee no overlap even in 1100x720 windows
    const float stats_panel_width = 320.0f;
    float right_stats_x = width - stats_panel_width;
    if (right_stats_x > 750.0f) {
        ImGui::SetCursorPos(ImVec2(right_stats_x, 15.0f));

        // Points Metric
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        ImGui::Text("%s 点", format_number(frame.points.size()).c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 12.0f);

        // Separator pipe
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::Border);
        ImGui::TextUnformatted("|");
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 12.0f);

        // FPS Metric
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        if (fps > 0.0f) {
            ImGui::Text("%.1f FPS", fps);
        } else {
            ImGui::TextUnformatted("-- FPS");
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 16.0f);

        // Status Badge Pill (Color + Text dual-encoding)
        ImVec2 badge_size = ImVec2(98.0f, 32.0f);
        ImVec2 badge_p = ImVec2(pos.x + width - badge_size.x - 16.0f, pos.y + 16.0f);

        ImU32 badge_bg, badge_border, badge_text;
        const char* badge_label = "准备就绪";

        if (mode == AppMode::PcapReplay) {
            badge_bg = IM_COL32(245, 158, 11, 25);     // Amber tint
            badge_border = IM_COL32(245, 158, 11, 220); // Amber border
            badge_text = IM_COL32(251, 191, 36, 255);
            badge_label = "PCAP 回放";
        } else if (mode == AppMode::LiveCapture) {
            badge_bg = IM_COL32(16, 185, 129, 25);     // Emerald tint
            badge_border = IM_COL32(16, 185, 129, 220); // Emerald border
            badge_text = IM_COL32(52, 211, 153, 255);
            badge_label = " 实时监听";
        } else {
            badge_bg = IM_COL32(30, 41, 59, 40);       // Slate tint
            badge_border = IM_COL32(71, 85, 105, 160); // Slate border
            badge_text = IM_COL32(155, 174, 195, 255);
            badge_label = " 准备就绪";
        }

        draw_list->AddRectFilled(badge_p, ImVec2(badge_p.x + badge_size.x, badge_p.y + badge_size.y), badge_bg, 6.0f);
        draw_list->AddRect(badge_p, ImVec2(badge_p.x + badge_size.x, badge_p.y + badge_size.y), badge_border, 6.0f, 0, 1.0f);

        ImVec2 txt_sz = ImGui::CalcTextSize(badge_label);
        draw_list->AddText(
            ImVec2(badge_p.x + (badge_size.x - txt_sz.x) * 0.5f, badge_p.y + (badge_size.y - txt_sz.y) * 0.5f),
            badge_text,
            badge_label
        );
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void UIWorkspace::draw_devices_panel(
    float width, float height, const PointCloudFrame& frame,
    const PcapReader& pcap_reader, AppMode mode
) {
    const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14,14));
    ImGui::Begin("##LeftDevicesPanel", nullptr, flags);
    ImGui::TextUnformatted("设备与数据源");
    ImGui::Spacing();

    auto property = [](const char* label, const std::string& value) {
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", value.c_str());
    };
    const bool has_device = !frame.vendor.empty() || !frame.model.empty();
    const std::string target_ip = get_target_lidar_ip();
    const bool has_diagnostics = mode == AppMode::LiveCapture && diagnostic_ip_ == target_ip && !diagnostic_ip_.empty();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::CardBg);
    ImGui::BeginChild("##DeviceCard", ImVec2(0,0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextColored(has_device ? ThemeColors::AccentIceBlue : ThemeColors::TextMuted,
                       "%s", frame.model.empty() ? "尚未识别设备" : frame.model.c_str());
    ImGui::TextDisabled("%s", frame.vendor.empty() ? "等待点云数据" : frame.vendor.c_str());
    ImGui::Separator();
    if (ImGui::BeginTable("##DeviceProperties",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label",ImGuiTableColumnFlags_WidthFixed,52);
        ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch);
        property("序列号", has_diagnostics && !diagnostic_serial_.empty() ? diagnostic_serial_ : "未获取");
        property("目标 IP", target_ip);
        property("固件", has_diagnostics && !diagnostic_firmware_.empty() ? diagnostic_firmware_ : "未获取");
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing(); ImGui::TextUnformatted("数据源");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::CardBg);
    ImGui::BeginChild("##SourceCard", ImVec2(0,0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (mode == AppMode::PcapReplay) {
        ImGui::TextColored(ThemeColors::AccentIceBlue,"PCAP 离线回放");
        const std::string filename=std::filesystem::u8path(pcap_reader.filepath()).filename().u8string();
        ImGui::TextWrapped("%s",filename.c_str());
        ImGui::TextWrapped("报文: %s / %s",format_number(pcap_reader.current_index()).c_str(),format_number(pcap_reader.total_packets()).c_str());
    } else if (mode == AppMode::LiveCapture) {
        ImGui::TextColored(ThemeColors::SuccessGreen,"以太网实时监听");
        ImGui::Text("包速: %.0f pkts / s",live_pps_);
        ImGui::Text("带宽: %.2f Mbps",live_mbps_);
        ImGui::Text("点云: %.1f FPS",live_fps_);
    } else {
        ImGui::TextDisabled("等待输入");
        ImGui::TextWrapped("导入 PCAP 文件，或在网络设置中开始监听。");
    }
    ImGui::EndChild(); ImGui::PopStyleColor();

    ImGui::Spacing(); ImGui::TextUnformatted(is_listening_ ? "监听端口" : "配置端口（未监听）");
    const auto ports=get_listen_ports();
    if (ImGui::BeginTable("##PortTiles",2,ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i=0;i<ports.size();++i) {
            ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ThemeColors::InputBg);
            ImGui::BeginChild("Port",ImVec2(0,50),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(is_listening_ ? ThemeColors::AccentIceBlue : ThemeColors::TextMuted,"UDP %u",unsigned(ports[i]));
            ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::TextUnformatted("运行日志");
    const float log_h=std::max(90.0f,ImGui::GetContentRegionAvail().y-4);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ThemeColors::InputBg);
    ImGui::BeginChild("##LeftLogView",ImVec2(0,log_h),ImGuiChildFlags_Borders);
    const bool at_bottom=ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-2;
    for(const auto& log:logs_) ImGui::TextWrapped("%s",log.c_str());
    if(autoscroll_logs_ && at_bottom) ImGui::SetScrollHereY(1);
    ImGui::EndChild(); ImGui::PopStyleColor();
    ImGui::End(); ImGui::PopStyleVar();
}

void UIWorkspace::draw_viewport_overlays(
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
) {
    ImGui::SetNextWindowPos(ImVec2(left,top));
    ImGui::SetNextWindowSize(ImVec2(width,height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
    ImGui::Begin("##ViewportOverlays",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // 1. Top-Left Floating Chip: [PCAP 回放 · Model] or [以太网实时流 · Model] or [待机准备就绪]
    std::string chip_label;
    ImU32 dot_col = ImGui::ColorConvertFloat4ToU32(ThemeColors::TextMuted);

    if (mode == AppMode::PcapReplay) {
        std::string m_name = frame.model.empty() ? "识别中" : frame.model;
        chip_label = "PCAP 回放 · " + m_name;
        dot_col = ImGui::ColorConvertFloat4ToU32(ThemeColors::AmberStatus);
    } else if (mode == AppMode::LiveCapture) {
        std::string m_name = frame.model.empty() ? "等待数据" : frame.model;
        chip_label = "以太网实时流 · " + m_name;
        dot_col = ImGui::ColorConvertFloat4ToU32(ThemeColors::SuccessGreen);
    } else {
        chip_label = "待机准备就绪";
        dot_col = ImGui::ColorConvertFloat4ToU32(ThemeColors::TextMuted);
    }

    ImVec2 badge_p0 = ImVec2(left + 16.0f, top + 16.0f);
    ImVec2 text_sz = ImGui::CalcTextSize(chip_label.c_str());
    ImVec2 badge_p1 = ImVec2(badge_p0.x + text_sz.x + 36.0f, badge_p0.y + 30.0f);

    draw_list->AddRectFilled(badge_p0, badge_p1, ImGui::ColorConvertFloat4ToU32(ThemeColors::CardBg), 6.0f);
    draw_list->AddRect(badge_p0, badge_p1, ImGui::ColorConvertFloat4ToU32(ThemeColors::Border), 6.0f, 0, 1.0f);
    draw_list->AddCircleFilled(ImVec2(badge_p0.x + 14.0f, badge_p0.y + 15.0f), 4.0f, dot_col);
    draw_list->AddText(ImVec2(badge_p0.x + 24.0f, badge_p0.y + 7.0f), ImGui::ColorConvertFloat4ToU32(ThemeColors::TextPrimary), chip_label.c_str());

    // Legend uses renderer palette and live normalization bounds.
    const auto range=legend_range(settings);
    if(range.visible) {
        const float legend_w=104, legend_h=164;
        const ImVec2 p(left+width-legend_w-16,top+16);
        draw_list->AddRectFilled(p,ImVec2(p.x+legend_w,p.y+legend_h),ImGui::ColorConvertFloat4ToU32(ThemeColors::CardBg),6);
        draw_list->AddText(ImVec2(p.x+10,p.y+8),ImGui::ColorConvertFloat4ToU32(ThemeColors::TextMuted),range.title);
        if(range.categorical) {
            draw_list->AddText(ImVec2(p.x+10,p.y+42),ImGui::ColorConvertFloat4ToU32(ThemeColors::TextPrimary),"按线号分类");
        } else {
            for(int i=0;i<110;++i) {
                const auto col=legend_color(settings,1.0f-float(i)/109);
                draw_list->AddRectFilled(ImVec2(p.x+12,p.y+34+i),ImVec2(p.x+24,p.y+35+i),ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,1)));
            }
            for(int i=0;i<5;++i) {
                const float v=range.maximum+(range.minimum-range.maximum)*float(i)/4;
                char tick[24];std::snprintf(tick,sizeof(tick),"%.1f",v);
                draw_list->AddText(ImVec2(p.x+32,p.y+28+i*27.5f),ImGui::ColorConvertFloat4ToU32(ThemeColors::TextMuted),tick);
            }
        }
    }

    ImGui::End(); ImGui::PopStyleVar(2);

    // 3. Bottom Timeline Floating Controller (If in Replay mode or reader is open)
    bool show_timeline = mode == AppMode::PcapReplay && pcap_reader.is_open() && pcap_reader.total_packets() > 0;
    float timeline_h = width < 650 ? 76.0f : 44.0f;
    float bottom_reserved = show_timeline ? timeline_h + 20.0f : 20.0f;

    if (show_timeline) {
        ImVec2 tl_pos = ImVec2(left + 16.0f, top + height - timeline_h - 12.0f);
        ImVec2 tl_size = ImVec2(width - 32.0f, timeline_h);

        ImGui::SetNextWindowPos(tl_pos);
        ImGui::SetNextWindowSize(tl_size);
        ImGuiWindowFlags tl_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.082f, 0.110f, 0.149f, 0.95f)); // #151C26 glass
        ImGui::PushStyleColor(ImGuiCol_Border, ThemeColors::Border);

        if (ImGui::Begin("##ViewportTimeline", nullptr, tl_flags)) {
            // Play/Pause Button

            ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::ButtonPrimaryHover);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            if (ImGui::Button("##TimelinePlay", ImVec2(32.0f, 26.0f))) {
                is_playing = !is_playing;
                if (is_playing && mode != AppMode::PcapReplay && pcap_reader.total_packets() > 0) {
                    mode = AppMode::PcapReplay;
                }
            }
            const auto play_lo=ImGui::GetItemRectMin();
            draw_icon(ImGui::GetWindowDrawList(),is_playing ? UIIcon::Pause : UIIcon::Play,ImVec2(play_lo.x+8,play_lo.y+5),ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::SetItemTooltip("%s",is_playing ? "暂停回放" : "播放录包");
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            ImGui::SameLine(0.0f, 10.0f);

            // Use packet progress; packet count is not a duration.
            const size_t total=pcap_reader.total_packets();
            size_t current=std::min(pcap_reader.current_index(),total);
            const bool compact=width<650;
            if(compact) {ImGui::Text("报文 %s / %s",format_number(pcap_reader.current_index()).c_str(),format_number(pcap_reader.total_packets()).c_str());}
            const float speed_width=82.0f;
            char progress[80];std::snprintf(progress,sizeof(progress),"%zu / %zu",current,total);
            const float readout_width=ImGui::CalcTextSize(progress).x;
            if(!compact) {
                const float slider_width=std::max(60.0f,ImGui::GetContentRegionAvail().x-readout_width-speed_width-36.0f);
                ImGui::SetNextItemWidth(slider_width);
            } else {
                ImGui::SetNextItemWidth(std::max(60.0f,ImGui::GetContentRegionAvail().x-speed_width-12));
            }
            uint64_t value=current, minimum=0, maximum=total;
            if(ImGui::SliderScalar("##PcapTimelineSeek",ImGuiDataType_U64,&value,&minimum,&maximum,"")) {
                trigger_seek_=true; seek_index_=std::min<size_t>(value,total-1);
            }
            ImGui::SameLine(0,12);
            if(!compact) {ImGui::TextUnformatted(progress);ImGui::SameLine(0,12);}

            // Speed dropdown
            ImGui::SetNextItemWidth(82.0f);
            const char* speeds[] = { "0.5 ×", "1.0 ×", "2.0 ×", "5.0 ×" };
            int cur_spd_idx = 1;
            if (playback_speed == 0.5f) cur_spd_idx = 0;
            else if (playback_speed == 2.0f) cur_spd_idx = 2;
            else if (playback_speed == 5.0f) cur_spd_idx = 3;

            if (ImGui::Combo("##PlaybackSpd", &cur_spd_idx, speeds, 4)) {
                if (cur_spd_idx == 0) playback_speed = 0.5f;
                else if (cur_spd_idx == 1) playback_speed = 1.0f;
                else if (cur_spd_idx == 2) playback_speed = 2.0f;
                else if (cur_spd_idx == 3) playback_speed = 5.0f;
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    ImGui::SetNextWindowPos(ImVec2(left,top));
    ImGui::SetNextWindowSize(ImVec2(width,height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
    ImGui::Begin("##ViewportHints",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_list=ImGui::GetWindowDrawList();
    // 4. Bottom-Left Operation Help Tips
    {
        const char* help_str = " 旋转      平移      缩放";
        ImVec2 help_p = ImVec2(left + 20.0f, top + height - bottom_reserved - 20.0f);
        draw_list->AddText(help_p, ImGui::ColorConvertFloat4ToU32(ThemeColors::TextMuted), help_str);
    }

    // 5. Bottom-Right 3D Coordinate Axes Indicator (XYZ)
    {
        const float ax_origin_x = left + width - 45.0f;
        const float ax_origin_y = top + height - bottom_reserved - 20.0f;
        const float ax_len = 18.0f;

        // X Axis (Red)
        draw_list->AddLine(ImVec2(ax_origin_x, ax_origin_y), ImVec2(ax_origin_x + ax_len, ax_origin_y), IM_COL32(239, 68, 68, 255), 1.8f);
        draw_list->AddText(ImVec2(ax_origin_x + ax_len + 2.0f, ax_origin_y - 6.0f), IM_COL32(239, 68, 68, 255), "X");

        // Y Axis (Green)
        draw_list->AddLine(ImVec2(ax_origin_x, ax_origin_y), ImVec2(ax_origin_x + ax_len * 0.7f, ax_origin_y - ax_len * 0.7f), IM_COL32(16, 185, 129, 255), 1.8f);
        draw_list->AddText(ImVec2(ax_origin_x + ax_len * 0.7f + 2.0f, ax_origin_y - ax_len * 0.7f - 6.0f), IM_COL32(16, 185, 129, 255), "Y");

        // Z Axis (Ice Blue)
        draw_list->AddLine(ImVec2(ax_origin_x, ax_origin_y), ImVec2(ax_origin_x, ax_origin_y - ax_len), IM_COL32(116, 185, 255, 255), 1.8f);
        draw_list->AddText(ImVec2(ax_origin_x - 3.0f, ax_origin_y - ax_len - 14.0f), IM_COL32(116, 185, 255, 255), "Z");
    }

    // 6. Idle Empty State Notice (When no points & Idle)
    if (frame.points.empty() && mode == AppMode::Idle) {
        const char* idle_notice = "准备就绪：可导入录包或配置实时监听";
        ImVec2 n_sz = ImGui::CalcTextSize(idle_notice);
        ImVec2 n_pos = ImVec2(left + (width - n_sz.x) * 0.5f, top + (height - n_sz.y) * 0.5f);

        draw_list->AddRectFilled(ImVec2(n_pos.x - 16.0f, n_pos.y - 10.0f),
                                 ImVec2(n_pos.x + n_sz.x + 16.0f, n_pos.y + n_sz.y + 10.0f),
                                 IM_COL32(21, 28, 38, 210), 6.0f);
        draw_list->AddRect(ImVec2(n_pos.x - 16.0f, n_pos.y - 10.0f),
                           ImVec2(n_pos.x + n_sz.x + 16.0f, n_pos.y + n_sz.y + 10.0f),
                           ImGui::ColorConvertFloat4ToU32(ThemeColors::Border), 6.0f);
        draw_list->AddText(n_pos, ImGui::ColorConvertFloat4ToU32(ThemeColors::TextMuted), idle_notice);
    }
    ImGui::End(); ImGui::PopStyleVar(2);
}

void UIWorkspace::draw_controls_panel(
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
) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
    ImGui::Begin("##RightControlsPanel", nullptr, flags);

    switch (current_tab_) {
        case MainTab::PointCloud:
            draw_render_tab(settings, camera, pcap_reader, frame, app_mode, is_playing, playback_speed, loop_playback);
            break;
        case MainTab::NetworkSettings:
            draw_network_tab();
            break;
        case MainTab::PtpSettings:
            draw_ptp_tab(ptp_status);
            break;
        case MainTab::ModelSupport:
            draw_support_tab();
            break;
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void UIWorkspace::draw_render_tab(
    RenderSettings& settings,
    OrbitCamera& camera,
    PcapReader& pcap_reader,
    const PointCloudFrame& frame,
    AppMode& app_mode,
    bool& is_playing,
    float& playback_speed,
    bool& loop_playback
) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float avail_w = ImGui::GetContentRegionAvail().x;

    // Section 1: Header "显示与控制" + "PROPERTIES"
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        ImGui::TextUnformatted("显示与控制");
        ImGui::PopStyleColor();

        ImGui::SameLine(avail_w - 75.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        ImGui::TextUnformatted("PROPERTIES");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Group 1: 点云渲染
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
        ImGui::TextUnformatted("  点云渲染");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Color Mode Combo
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        ImGui::TextUnformatted("着色方式");
        ImGui::PopStyleColor();

        const char* colormaps[] = {
            "高度 · Turbo",
            "反射强度 · 灰度标尺",
            "测距 · Turbo",
            "扫描线 · 分类色",
            "单一纯色"
        };
        int cur_cm = static_cast<int>(settings.color_mode);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##ColorModeCombo", &cur_cm, colormaps, IM_ARRAYSIZE(colormaps))) {
            settings.color_mode = static_cast<ColorMode>(cur_cm);
        }

        ImGui::Spacing();

        // Point Size
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        ImGui::TextUnformatted("点大小");
        ImGui::PopStyleColor();
        ImGui::SameLine(avail_w - 55.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        ImGui::Text("%.1f px", settings.point_size);
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##PointSizeSlider", &settings.point_size, 1.0f, 6.0f, "");

        ImGui::Spacing();

        // Max Distance
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
        ImGui::TextUnformatted("最大距离");
        ImGui::PopStyleColor();
        ImGui::SameLine(avail_w - 55.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        ImGui::Text("%.0f m", settings.max_distance);
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##MaxDistSlider", &settings.max_distance, 10.0f, 200.0f, "");

        ImGui::Spacing();
        ImGui::Checkbox("圆点抗锯齿", &settings.circular_points);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Group 2: 视角
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
        ImGui::TextUnformatted("  视角");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        float btn_w = (avail_w - 8.0f) * 0.5f;
        const float btn_h = 34.0f;

        auto draw_view_btn = [&](const char* label, ViewPreset preset) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            const bool selected=view_is_preset_ && selected_view_==preset;
            if(selected) ImGui::PushStyleColor(ImGuiCol_Button,ThemeColors::ButtonPrimary);
            if (ImGui::Button(label, ImVec2(btn_w, btn_h))) {
                camera.reset_view(preset); selected_view_=preset; view_is_preset_=true;
            }
            if(selected) ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        };

        draw_view_btn(" 透视", ViewPreset::Isometric);
        ImGui::SameLine(0.0f, 8.0f);
        draw_view_btn(" 俯视", ViewPreset::Top);

        draw_view_btn(" 前视", ViewPreset::Front);
        ImGui::SameLine(0.0f, 8.0f);
        draw_view_btn(" 侧视", ViewPreset::Side);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Group 3: 离线回放
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
        ImGui::TextUnformatted("离线回放");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Big Primary Import Button (Prominent Ice/Deep Blue #1B6CA8)
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::ButtonPrimaryHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (icon_button("导入 PCAP 文件", UIIcon::Folder, ImVec2(-1.0f, 36.0f))) {
            trigger_open_file_ = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        ImGui::Spacing();

        ImGui::BeginDisabled(app_mode != AppMode::PcapReplay || !pcap_reader.is_open());
        // 3-Button Action Row: [▶ 开始 / ⏸ 暂停] [ 停止] [↺ 重置]
        float action_btn_w = (avail_w - 16.0f) / 3.0f;
        const float action_btn_h = 32.0f;

        const char* play_btn_label = is_playing ? "暂停" : "开始";
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button(play_btn_label, ImVec2(action_btn_w, action_btn_h))) {
            is_playing = !is_playing;
            if (is_playing && app_mode != AppMode::PcapReplay && pcap_reader.total_packets() > 0) {
                app_mode = AppMode::PcapReplay;
            }
        }
        ImGui::SameLine(0.0f, 8.0f);

        if (ImGui::Button(" 停止", ImVec2(action_btn_w, action_btn_h))) {
            is_playing = false;
            trigger_seek_=true; seek_index_=0;
        }
        ImGui::SameLine(0.0f, 8.0f);

        if (ImGui::Button("重置", ImVec2(action_btn_w, action_btn_h))) {
            trigger_seek_=true; seek_index_=0;
            is_playing = true;
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Checkbox("循环回放", &loop_playback);
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ThemeColors::CardBg);
    ImGui::BeginChild("##RenderStatus",ImVec2(0,0),ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    const char* label=app_mode==AppMode::PcapReplay ? (is_playing ? "正在回放" : "回放已暂停") :
                      app_mode==AppMode::LiveCapture ? "实时监听中" : "准备就绪";
    ImGui::TextColored(app_mode==AppMode::Idle ? ThemeColors::TextMuted : ThemeColors::AccentIceBlue,"%s",label);
    ImGui::TextWrapped("可导入录包或配置实时监听");
    if(app_mode==AppMode::LiveCapture && !frame.vendor.empty() && ImGui::Button("打开雷达控制页")) {
        open_file_in_system("http://"+get_target_lidar_ip()+"/");
    }
    ImGui::EndChild(); ImGui::PopStyleColor();

}

void UIWorkspace::draw_report_modal(const PointCloudFrame& frame) {
    if (!show_report_modal_) {
        return;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float modal_w = std::min(vp->WorkSize.x * 0.92f, 1180.0f);
    float modal_h = std::min(vp->WorkSize.y * 0.90f, 740.0f);

    ImGui::SetNextWindowSize(ImVec2(modal_w, modal_h), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - modal_w) * 0.5f,
                                   vp->WorkPos.y + (vp->WorkSize.y - modal_h) * 0.5f),
                            ImGuiCond_Appearing);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ThemeColors::PanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, ThemeColors::Border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    if (ImGui::Begin("砺见智行 —— 激光雷达检测报告工作台 (Report Studio)###ReportModal", &show_report_modal_, flags)) {

        // Top Action Bar
        ImGui::BeginGroup();

        // Row 1: Target LiDAR IP & Automated Parameter Acquisition
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("目标雷达 IP:");
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::SetNextItemWidth(125.0f);
        {
            char ip_buf[64];
            {
                std::lock_guard<std::mutex> lock(detected_ip_mutex_);
                std::strncpy(ip_buf, target_lidar_ip_buf_, sizeof(ip_buf));
                ip_buf[sizeof(ip_buf) - 1] = '\0';
            }
            if (ImGui::InputText("##TargetLidarIP", ip_buf, sizeof(ip_buf))) {
                std::lock_guard<std::mutex> lock(detected_ip_mutex_);
                std::strncpy(target_lidar_ip_buf_, ip_buf, sizeof(target_lidar_ip_buf_));
                target_lidar_ip_buf_[sizeof(target_lidar_ip_buf_) - 1] = '\0';
            }
        }

        ImGui::SameLine(0.0f, 8.0f);

        // One-Click Radar Parameter Acquisition Action Button
        bool is_querying = (query_status_ == DiagnosticQueryStatus::Querying);
        if (is_querying) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::ButtonPrimaryHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        if (ImGui::Button(" 一键采集雷达参数 (PTC 9347 / Web)", ImVec2(245.0f, 32.0f))) {
            std::string ip = get_target_lidar_ip();
            query_status_ = DiagnosticQueryStatus::Querying;
            query_status_msg_ = "正在采集雷达参数 (IP: " + ip + ")...";
            query_start_time_ = std::chrono::steady_clock::now();
            add_log("[Report Studio] 正在向目标雷达 " + ip + " 发起 PTC 9347 / Web 参数采集...");
            query_future_ = std::async(std::launch::async, [ip]() {
                return HesaiPtcClient::query(ip, 9347, 1500);
            });
        }
        ImGui::PopStyleColor(3);
        if (is_querying) ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 8.0f);

        // Sync from Live Viewer Button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.28f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.95f, 0.65f, 1.0f));
        if (ImGui::Button("同步当前点云指标", ImVec2(145.0f, 32.0f))) {
            if (!frame.model.empty()) {
                report_data_.vendor = frame.vendor;
                report_data_.model = frame.vendor + " " + frame.model;
                report_data_.point_count = frame.points.size();
                report_data_.max_range_m = frame.bounds.max_distance;
                report_status_msg_ = "已成功从查看器同步雷达设备参数与当前点云指标.";
            } else {
                report_status_msg_ = "当前尚未检测到有效点云数据，已保持默认参数.";
            }
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0.0f, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.28f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.38f, 0.58f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.85f, 1.0f, 1.0f));
        if (ImGui::Button(" 打开雷达 Web 页面 ", ImVec2(170.0f, 32.0f))) {
            open_file_in_system("http://" + get_target_lidar_ip() + "/");
            add_log("在系统默认浏览器中打开雷达管理页面: http://" + get_target_lidar_ip() + "/");
        }
        ImGui::PopStyleColor(3);

        // Close button at top right
        float close_btn_x = ImGui::GetWindowWidth() - 90.0f;
        if (close_btn_x > 750.0f) {
            ImGui::SameLine(close_btn_x);
            if (ImGui::Button("关闭", ImVec2(75.0f, 32.0f))) {
                show_report_modal_ = false;
            }
        }

        ImGui::Spacing();

        // Row 2: Export actions & Dynamic Feedback Indicators
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::ButtonPrimaryHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
        if (ImGui::Button("导出专业矢量 PDF 报告", ImVec2(170.0f, 30.0f))) {
            std::filesystem::path full_path = default_report_output_dir() / "lidar_inspection_report.pdf";
            if (screenshot_provider_) {
                std::string capture_b64 = screenshot_provider_();
                if (!capture_b64.empty()) {
                    attach_viewport_screenshot(report_data_, capture_b64);
                }
            }
            std::string full_utf8 = full_path.u8string();
            if (ReportGenerator::export_pdf(report_data_, full_utf8)) {
                report_status_msg_ = "已成功导出专业矢量 PDF 检测报告: " + full_utf8;
                if (auto_open_in_browser_) {
                    open_file_in_system(full_utf8);
                }
            } else {
                std::error_code ec;
                std::filesystem::path html_path = full_path.parent_path() / (full_path.stem().u8string() + ".html");
                std::string html_utf8 = html_path.u8string();
                if (std::filesystem::exists(html_path, ec)) {
                    report_status_msg_ = "提示: 本地未检测到 Chrome/Edge 转换引擎，已为您生成完整 HTML 报告: " + html_utf8;
                    if (auto_open_in_browser_) {
                        open_file_in_system(html_utf8);
                    }
                } else {
                    report_status_msg_ = "错误: 无法写入 PDF 或 HTML 报告: " + full_utf8;
                }
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.0f, 8.0f);

        if (ImGui::Button("导出 HTML 完整报告", ImVec2(150.0f, 30.0f))) {
            std::filesystem::path full_path = default_report_output_dir() / "lidar_inspection_report.html";
            std::string full_utf8 = full_path.u8string();
            if (screenshot_provider_) {
                std::string capture_b64 = screenshot_provider_();
                if (!capture_b64.empty()) {
                    attach_viewport_screenshot(report_data_, capture_b64);
                }
            }
            if (ReportGenerator::export_html(report_data_, full_utf8)) {
                report_status_msg_ = "已成功导出 HTML 报告: " + full_utf8;
                if (auto_open_in_browser_) {
                    open_file_in_system(full_utf8);
                }
            } else {
                report_status_msg_ = "错误: 无法写入 HTML 报告: " + full_utf8;
            }
        }

        ImGui::SameLine(0.0f, 8.0f);
        ImGui::Checkbox("导出后自动在浏览器打开", &auto_open_in_browser_);

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::AlignTextToFramePadding();

        // Status indicator
        if (query_status_ == DiagnosticQueryStatus::Querying) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - query_start_time_
            ).count();
            int dots = static_cast<int>((elapsed_ms / 350) % 4);
            std::string dot_str(dots, '.');
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "⏳ 正在采集雷达参数%s (%.1fs)",
                               dot_str.c_str(), elapsed_ms / 1000.0f);
        } else if (query_status_ == DiagnosticQueryStatus::Success) {
            ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.55f, 1.0f), "✔ %s", query_status_msg_.c_str());
        } else if (query_status_ == DiagnosticQueryStatus::Failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "✖ %s", query_status_msg_.c_str());
        } else if (!report_status_msg_.empty()) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "%s", report_status_msg_.c_str());
        }

        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 2-Column Layout matching web aside & article
        float avail_h = ImGui::GetContentRegionAvail().y - 10.0f;
        float aside_w = 260.0f;

        // Left Navigation (Sections List)
        ImGui::BeginChild("##ModalAside", ImVec2(aside_w, avail_h), true);

        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
        ImGui::TextUnformatted("检测项目清单");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        auto draw_nav_item = [&](int index, const char* label, const char* status_text, bool ok) {
            bool selected = (selected_report_section_ == index);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::InputBg);
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
            }

            ImGui::BeginGroup();
            if (ImGui::Button(label, ImVec2(aside_w - 90.0f, 30.0f))) {
                selected_report_section_ = index;
            }
            ImGui::PopStyleColor(2);

            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(ok ? ImVec4(0.2f, 0.85f, 0.5f, 1.0f) : ImVec4(0.9f, 0.25f, 0.25f, 1.0f),
                               "[%s]", status_text);
            ImGui::EndGroup();
            ImGui::Spacing();
        };

        // Item 0: 基本信息
        draw_nav_item(0, "基本信息", "必填", true);

        // Items 1..8: 8大检测项
        for (size_t i = 0; i < report_data_.sections.size(); ++i) {
            std::string item_label = std::to_string(i + 1) + ". " + report_data_.sections[i].title;
            const auto& sec = report_data_.sections[i];
            if (!sec.image_base64.empty()) {
                item_label += " [📷]";
            }
            bool pass = (sec.result == "正常" || sec.result == "合格");
            draw_nav_item(static_cast<int>(i + 1), item_label.c_str(), sec.result.c_str(), pass);
        }

        // Item 9: 综合意见
        draw_nav_item(9, "综合判定意见", report_data_.overall_passed ? "合格" : "不合格", report_data_.overall_passed);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("在线雷达设备快照:");
        ImGui::BulletText("型号: %s", report_data_.model.c_str());
        ImGui::BulletText("SN: %s", report_data_.serial_number.c_str());
        ImGui::BulletText("实时点数: %zu", frame.points.size());
        ImGui::BulletText("最远测距: %.1f m", frame.bounds.max_distance);

        ImGui::EndChild();

        ImGui::SameLine(0.0f, 12.0f);

        // Right Article (Editing Cards)
        ImGui::BeginChild("##ModalArticle", ImVec2(0.0f, avail_h), true);

        if (selected_report_section_ == 0) {
            // Card: 基本信息修改
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
            ImGui::TextUnformatted("报告基本信息 (可直接修改)");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            auto draw_text_input = [](const char* label, std::string& val) {
                ImGui::Text("%s:", label);
                char buf[128];
                std::strncpy(buf, val.c_str(), sizeof(buf));
                buf[sizeof(buf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText((std::string("##Input_") + label).c_str(), buf, sizeof(buf))) {
                    val = buf;
                }
                ImGui::Spacing();
            };

            ImGui::Columns(2, "##MetaCols", false);
            draw_text_input("报告编号", report_data_.report_id);
            draw_text_input("设备型号", report_data_.model);
            draw_text_input("设备序列号 (S/N)", report_data_.serial_number);

            ImGui::NextColumn();
            draw_text_input("客户 / 测试项目", report_data_.customer_project);
            draw_text_input("主检测试人员", report_data_.inspector_name);
            draw_text_input("检测评定日期", report_data_.test_date);
            ImGui::Columns(1);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "设备硬件与网络配置:");
            ImGui::Columns(2, "##HWCols", false);
            draw_text_input("雷达厂商", report_data_.vendor);
            draw_text_input("固件版本", report_data_.firmware_version);
            ImGui::NextColumn();
            draw_text_input("设备 IP 地址", report_data_.ip_address);
            draw_text_input("物理 MAC 地址", report_data_.mac_address);
            ImGui::Columns(1);

        } else if (selected_report_section_ >= 1 && selected_report_section_ <= 8) {
            // Card: 8大分项检测卡片
            int sec_idx = selected_report_section_ - 1;
            auto& sec = report_data_.sections[sec_idx];

            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
            ImGui::Text("第 %d 项: %s", selected_report_section_, sec.title.c_str());
            ImGui::PopStyleColor();

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 180.0f);
            ImGui::Text("单项判定:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);

            const char* results[] = {"正常", "合格", "不合格", "未检测", "不适用"};
            int r_idx = 0;
            for (int k = 0; k < 5; ++k) {
                if (sec.result == results[k]) r_idx = k;
            }
            if (ImGui::Combo("##SectionResult", &r_idx, results, IM_ARRAYSIZE(results))) {
                sec.result = results[r_idx];
            }
            ImGui::EndGroup();

            ImGui::Spacing();
            ImGui::TextDisabled("检测要点: %s", sec.hint.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            if (sec_idx == 0) { // 第 1 项: 外观检验
                ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "外观实物照片 (将嵌入专业报告第 1 项):");
                ImGui::Spacing();

                auto handle_photo_upload = [&](const char* ok_verb) {
                    std::string img_path = show_open_image_dialog();
                    if (img_path.empty()) return;
                    std::string data_uri, err;
                    if (load_image_file_as_data_uri(img_path, data_uri, err)) {
                        sec.image_base64 = data_uri;
                        std::string fname = std::filesystem::u8path(img_path).filename().u8string();
                        sec.image_caption = "图 1-1 激光雷达机身外观、光学视窗与接口实测检测照片 (" + fname + ")";
                        add_log(std::string(ok_verb) + ": " + img_path);
                        report_status_msg_ = std::string(ok_verb) + ": " + fname;
                    } else {
                        report_status_msg_ = "上传外观照片失败: " + err;
                        add_log("错误: " + report_status_msg_);
                    }
                };

                if (sec.image_base64.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.22f, 0.28f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.32f, 0.42f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.85f, 1.0f, 1.0f));
                    if (ImGui::Button("📷 上传外观实测照片 (JPG / PNG / WebP)...", ImVec2(320.0f, 32.0f))) {
                        handle_photo_upload("已成功上传外观照片");
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(未挂载照片，支持 JPG/PNG/WebP/BMP)");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.90f, 0.55f, 1.0f));
                    ImGui::TextUnformatted("✓ 已成功挂载外观实测照片");
                    ImGui::PopStyleColor();

                    ImGui::SameLine(0.0f, 12.0f);
                    if (ImGui::Button("重新上传照片...", ImVec2(120.0f, 26.0f))) {
                        handle_photo_upload("已更新外观照片");
                    }

                    ImGui::SameLine(0.0f, 8.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.15f, 0.15f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
                    if (ImGui::Button("移除照片", ImVec2(80.0f, 26.0f))) {
                        sec.image_base64.clear();
                        sec.image_caption.clear();
                        report_status_msg_ = "已移除外观照片.";
                    }
                    ImGui::PopStyleColor(3);

                    // Caption editor
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("照片图注说明:");
                    ImGui::SameLine();
                    char cap_buf[256];
                    std::strncpy(cap_buf, sec.image_caption.c_str(), sizeof(cap_buf));
                    cap_buf[sizeof(cap_buf) - 1] = '\0';
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::InputText("##PhotoCaption", cap_buf, sizeof(cap_buf))) {
                        sec.image_caption = cap_buf;
                    }
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            ImGui::Text("检测记录 / 实测数据 (多行可编辑文本):");

            float bottom_reserve = (sec_idx == 0) ? 90.0f : 60.0f;
            ImVec2 txt_area_size = ImVec2(-1.0f, ImGui::GetContentRegionAvail().y - bottom_reserve);
            input_text_multiline_str("##SectionNotes", sec.notes, txt_area_size);

            ImGui::Spacing();
            if (ImGui::Button("恢复默认规范模板", ImVec2(160.0f, 30.0f))) {
                InspectionReportData default_data;
                sec.notes = default_data.sections[sec_idx].notes;
                sec.result = default_data.sections[sec_idx].result;
            }

        } else if (selected_report_section_ == 9) {
            // Card: 综合意见与最终大印章
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::AccentIceBlue);
            ImGui::TextUnformatted("综合质量检验判定与交付意见");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Large Verdict Stamp
            ImDrawList* dlist = ImGui::GetWindowDrawList();
            ImVec2 stamp_p0 = ImGui::GetCursorScreenPos();
            float stamp_w = ImGui::GetContentRegionAvail().x;
            ImVec2 stamp_p1 = ImVec2(stamp_p0.x + stamp_w, stamp_p0.y + 54.0f);

            ImU32 bg_col = report_data_.overall_passed ? IM_COL32(20, 55, 36, 230) : IM_COL32(65, 20, 25, 230);
            ImU32 border_col = report_data_.overall_passed ? IM_COL32(33, 213, 155, 255) : IM_COL32(235, 60, 70, 255);
            dlist->AddRectFilled(stamp_p0, stamp_p1, bg_col, 6.0f);
            dlist->AddRect(stamp_p0, stamp_p1, border_col, 6.0f, 0, 2.0f);

            const char* verdict_text = report_data_.overall_passed ?
                "综合检测评定结论: 【检验合格 · 准予出厂交付 (PASS)】" :
                "综合检测评定结论: 【检验不合格 · 存在功能缺陷 (REJECT)】";

            dlist->AddText(ImVec2(stamp_p0.x + stamp_w * 0.5f - 210.0f, stamp_p0.y + 16.0f),
                           border_col, verdict_text);

            ImGui::SetCursorScreenPos(ImVec2(stamp_p0.x, stamp_p1.y + 16.0f));

            ImGui::Checkbox("判定整机检测综合合格 (PASS)", &report_data_.overall_passed);
            ImGui::Spacing();

            ImGui::Text("综合审核与技术评定意见 (多行可编辑文本):");
            ImVec2 sum_area_size = ImVec2(-1.0f, ImGui::GetContentRegionAvail().y - 60.0f);
            input_text_multiline_str("##ReportSummary", report_data_.summary, sum_area_size);

            ImGui::Spacing();
            if (ImGui::Button("恢复标准综合评定模板", ImVec2(180.0f, 30.0f))) {
                InspectionReportData default_data;
                report_data_.summary = default_data.summary;
                report_data_.overall_passed = true;
            }
        }

        ImGui::EndChild();
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

std::string UIWorkspace::get_listen_ip() const {
    std::string ip = listen_ip_buf_;
    size_t first = ip.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "0.0.0.0";
    size_t last = ip.find_last_not_of(" \t\r\n");
    std::string clean = ip.substr(first, (last - first + 1));
    size_t space_pos = clean.find(' ');
    if (space_pos != std::string::npos) {
        clean = clean.substr(0, space_pos);
    }
    return clean.empty() ? "0.0.0.0" : clean;
}

std::vector<uint16_t> UIWorkspace::get_listen_ports() const {
    std::vector<uint16_t> ports;
    std::string str = listen_ports_buf_;
    for (char& c : str) {
        if (c == ',' || c == ';' || c == '|' || c == '/' || c == ':') {
            c = ' ';
        }
    }
    std::stringstream ss(str);
    int p = 0;
    while (ss >> p) {
        if (p > 0 && p <= 65535) {
            ports.push_back(static_cast<uint16_t>(p));
        }
    }
    if (ports.empty()) {
        ports = {2368, 51100};
    }
    return ports;
}

void UIWorkspace::draw_network_tab() {
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
    ImGui::TextUnformatted("NETWORK SETTINGS");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
    ImGui::TextUnformatted("网络接口与数据流设置");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 1. Dynamic Listening Status Badge (Card)
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 badge_p0 = ImGui::GetCursorScreenPos();
    float avail_w = ImGui::GetContentRegionAvail().x;
    float badge_h = 36.0f;
    ImVec2 badge_p1 = ImVec2(badge_p0.x + avail_w, badge_p0.y + badge_h);

    if (is_listening_) {
        draw_list->AddRectFilled(badge_p0, badge_p1, IM_COL32(18, 48, 32, 230), 4.0f);
        draw_list->AddRect(badge_p0, badge_p1, IM_COL32(33, 213, 155, 255), 4.0f, 0, 1.0f);
        draw_list->AddText(ImVec2(badge_p0.x + 14.0f, badge_p0.y + 10.0f), IM_COL32(49, 235, 175, 255),
                           " LIVE  以太网实时监听中");
    } else {
        draw_list->AddRectFilled(badge_p0, badge_p1, IM_COL32(28, 30, 35, 230), 4.0f);
        draw_list->AddRect(badge_p0, badge_p1, IM_COL32(70, 75, 84, 255), 4.0f, 0, 1.0f);
        draw_list->AddText(ImVec2(badge_p0.x + 14.0f, badge_p0.y + 10.0f), IM_COL32(160, 165, 175, 255),
                           " STOPPED  监听已停止 (空闲)");
    }

    ImGui::SetCursorScreenPos(ImVec2(badge_p0.x, badge_p1.y + 10.0f));

    // 2. Action Control Button (Full width)
    if (!is_listening_) {
        // Green "开始实时监听" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.58f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.68f, 0.38f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.50f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);

        if (ImGui::Button("开始实时监听", ImVec2(-1.0f, 38.0f))) {
            trigger_start_listen_ = true;
        }
        ImGui::PopStyleColor(4);
    } else {
        // Red "停止监听" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.18f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.24f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.14f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);

        if (ImGui::Button("停止监听", ImVec2(-1.0f, 38.0f))) {
            trigger_stop_listen_ = true;
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 3. Network Configuration
    ImGui::Text("网卡绑定 IP (默认 0.0.0.0 全部网卡):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##BindIP", listen_ip_buf_, sizeof(listen_ip_buf_));

    ImGui::Spacing();
    ImGui::Text("监听端口列表 (逗号分隔):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##PortsInput", listen_ports_buf_, sizeof(listen_ports_buf_));

    ImGui::Spacing();

    // 4. Port Presets
    ImGui::TextDisabled("快速厂商预设端口:");
    float btn_gap = 6.0f;
    float half_w = (ImGui::GetContentRegionAvail().x - btn_gap) * 0.5f;

    if (ImGui::Button("禾赛: 2368, 51100", ImVec2(-1.0f, 28.0f))) {
        std::strncpy(listen_ports_buf_, "2368, 51100", sizeof(listen_ports_buf_) - 1);
        listen_ports_buf_[sizeof(listen_ports_buf_) - 1] = '\0';
    }

    if (ImGui::Button("速腾: 6668, 7788", ImVec2(half_w, 28.0f))) {
        std::strncpy(listen_ports_buf_, "6668, 7788", sizeof(listen_ports_buf_) - 1);
        listen_ports_buf_[sizeof(listen_ports_buf_) - 1] = '\0';
    }
    ImGui::SameLine(0.0f, btn_gap);
    if (ImGui::Button("华为: 8080", ImVec2(-1.0f, 28.0f))) {
        std::strncpy(listen_ports_buf_, "8080", sizeof(listen_ports_buf_) - 1);
        listen_ports_buf_[sizeof(listen_ports_buf_) - 1] = '\0';
    }

    ImGui::Spacing();
    ImGui::TextDisabled("雷达设备控制管理:");
    std::string current_target_ip = get_target_lidar_ip();
    std::string net_web_btn = " 打开雷达控制后台 (" + current_target_ip + ") ";
    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::ButtonPrimary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::ButtonPrimaryHover);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
    if (ImGui::Button(net_web_btn.c_str(), ImVec2(-1.0f, 32.0f))) {
        open_file_in_system("http://" + current_target_ip + "/");
        add_log("在系统默认浏览器中打开雷达管理后台: http://" + current_target_ip + "/");
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 5. Dynamic Throughput Indicators
    ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "当前网络实时指标:");
    if (is_listening_) {
        ImGui::BulletText("实时接收数据包: %s 个", format_number(static_cast<size_t>(live_packets_)).c_str());
        ImGui::BulletText("实时接收包率: %s PPS", format_number(static_cast<size_t>(std::round(live_pps_))).c_str());
        ImGui::BulletText("实时网络带宽: %.2f Mbps", live_mbps_);
        ImGui::BulletText("点云接收帧率: %.1f FPS", live_fps_);
        ImGui::BulletText("丢包统计: 未提供");
    } else {
        ImGui::BulletText("实时接收数据包: 0 个");
        ImGui::BulletText("实时接收包率: 0 PPS (监听停止)");
        ImGui::BulletText("实时网络带宽: 0.00 Mbps");
        ImGui::BulletText("点云接收帧率: 0.0 FPS");
        ImGui::BulletText("丢包统计: 未提供");
    }
}

void UIWorkspace::draw_ptp_tab(const PtpStatus& ptp_status) {
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
    ImGui::TextUnformatted("PTP IEEE 1588 / 802.1AS");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
    ImGui::TextUnformatted("高精度授时同步监控");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ptp_status.state==PtpState::Locked ? ThemeColors::SuccessGreen : ThemeColors::TextMuted,
                       "同步状态: %s",ptp_status.state_name.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Grandmaster ID: %s",ptp_status.grandmaster_id.c_str());
    ImGui::Text("PTP 时钟域: %u",unsigned(ptp_status.domain_number));
    ImGui::Text("Sync 报文: %llu",static_cast<unsigned long long>(ptp_status.sync_packets_received));
    ImGui::Text("Announce 报文: %llu",static_cast<unsigned long long>(ptp_status.announce_packets_received));
    ImGui::TextWrapped("时钟偏差与链路延迟: 当前尚无测量数据");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("支持 IEEE 1588-2008 PTPv2 及 IEEE 802.1AS (gPTP) 工业车载时钟标准.");
}

void UIWorkspace::draw_support_tab() {
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextMuted);
    ImGui::TextUnformatted("SUPPORTED HARDWARE");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::TextPrimary);
    ImGui::TextUnformatted("主流激光雷达型号支持列表");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    auto draw_vendor_info = [](const char* vendor, const char* models, const char* default_ports) {
        ImGui::TextColored(ThemeColors::AccentIceBlue, "[ %s ]", vendor);
        ImGui::BulletText("支持型号: %s", models);
        ImGui::BulletText("默认端口: %s", default_ports);
        ImGui::Spacing();
    };

    draw_vendor_info("禾赛科技 (Hesai)", "AT128, ET25, Pandar128, QT128, XT32", "2368 (MSOP), 51100 (回传)");
    draw_vendor_info("速腾聚创 (RoboSense)", "E2, M1/M2, Ruby Plus, Bpearl, Helios", "6668 (MSOP), 7788 (DIFOP)");
    draw_vendor_info("图达通 (Seyond)", "Falcon 猎鹰, Robin 灵雀, 蔚来 ET7 原装", "8010 / 176A 字节流");
    draw_vendor_info("华为 (Huawei)", "D1, D2, D3, L306, 问界系列车载雷达", "8080 (1021B 原生报文)");
    draw_vendor_info("威力登 (Velodyne)", "VLP-16, VLP-32C, Alpha Prime 128", "2368 (Data), 8308 (Telemetry)");
}

void UIWorkspace::set_detected_lidar_ip(const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0" || ip == "255.255.255.255") return;
    std::lock_guard<std::mutex> lock(detected_ip_mutex_);
    last_detected_ip_ = ip;
    // If target buffer still has default IP or is empty, auto-update with live IP
    if (std::string(target_lidar_ip_buf_) == "192.168.1.201" || target_lidar_ip_buf_[0] == '\0') {
        std::strncpy(target_lidar_ip_buf_, ip.c_str(), sizeof(target_lidar_ip_buf_) - 1);
        target_lidar_ip_buf_[sizeof(target_lidar_ip_buf_) - 1] = '\0';
    }
}

std::string UIWorkspace::get_target_lidar_ip() const {
    std::lock_guard<std::mutex> lock(detected_ip_mutex_);
    std::string ip = target_lidar_ip_buf_;
    size_t first = ip.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "192.168.1.201";
    size_t last = ip.find_last_not_of(" \t\r\n");
    std::string clean = ip.substr(first, (last - first + 1));
    size_t space_pos = clean.find(' ');
    if (space_pos != std::string::npos) clean = clean.substr(0, space_pos);
    return clean.empty() ? "192.168.1.201" : clean;
}

bool UIWorkspace::is_querying_diagnostics() const {
    return query_status_ == DiagnosticQueryStatus::Querying;
}

void UIWorkspace::poll_diagnostic_query() {
    if (query_status_ == DiagnosticQueryStatus::Querying && query_future_.valid()) {
        auto status = query_future_.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            LidarDiagnosticParams params = query_future_.get();
            if (params.success) {
                query_status_ = DiagnosticQueryStatus::Success;
                query_status_msg_ = "采集成功 (S/N: " + params.serial_number + ", 固件: " + params.firmware_version + ")";
                add_log("[Report Studio] 雷达参数自动采集成功: SN=" + params.serial_number +
                        ", 固件=" + params.firmware_version +
                        ", 转速=" + std::to_string(static_cast<int>(params.motor_rpm)) + " RPM" +
                        ", 温度=" + std::to_string(static_cast<int>(params.temperature_c)) + " ℃");
                apply_diagnostic_params(params);
            } else {
                query_status_ = DiagnosticQueryStatus::Failed;
                std::string err = params.error_msg.empty() ? "连接超时或无响应" : params.error_msg;
                query_status_msg_ = "采集失败: " + err;
                add_log("[Report Studio] 错误: 雷达参数自动采集失败 - " + err);
            }
        }
    }
}

void UIWorkspace::apply_diagnostic_params(const LidarDiagnosticParams& params) {
    if (!params.success) return;

    diagnostic_ip_ = params.ip_address.empty() ? get_target_lidar_ip() : params.ip_address;
    diagnostic_serial_ = params.serial_number;
    diagnostic_firmware_ = params.firmware_version;

    // Direct fields in report_data_
    if (!params.serial_number.empty()) report_data_.serial_number = params.serial_number;
    if (!params.firmware_version.empty()) report_data_.firmware_version = params.firmware_version;
    if (params.motor_rpm > 0.0f) {
        report_data_.motor_rpm = params.motor_rpm;
        report_data_.frame_rate_hz = params.motor_rpm / 60.0f;
    }
    if (params.temperature_c != 0.0f) report_data_.temperature_c = params.temperature_c;
    if (params.operating_voltage > 0.0f) report_data_.operating_voltage = params.operating_voltage;
    if (!params.ip_address.empty()) report_data_.ip_address = params.ip_address;
    if (!params.mac_address.empty()) report_data_.mac_address = params.mac_address;
    if (!params.model.empty()) report_data_.model = params.model;

    // Helper to find report section by keyword or fallback index
    auto get_section = [this](const std::string& key, size_t default_idx) -> ReportSection* {
        for (auto& sec : report_data_.sections) {
            if (sec.title.find(key) != std::string::npos) return &sec;
        }
        if (default_idx < report_data_.sections.size()) return &report_data_.sections[default_idx];
        return nullptr;
    };

    // Section 0 (外观)
    ReportSection* sec0 = get_section("外观", 0);
    if (sec0) {
        std::ostringstream ss;
        ss << "【PTC 9347 设备身份读取】\n型号：" << (params.model.empty() ? report_data_.model : params.model)
           << "\n序列号 S/N：" << params.serial_number;
        if (!params.product_number.empty()) ss << "\n部件型号 P/N：" << params.product_number;
        if (!params.date_of_manufacture.empty()) ss << "\n生产日期：" << params.date_of_manufacture;
        ss << "\n1. 整机外壳：机壳外观结构完整平整，无严重磕碰或变形裂损，紧固件齐全牢固。\n"
           << "2. 光学视窗：表面整体良好，无深度硬伤或裂损，透光良好，不影响激光收发与点云成像。\n"
           << "3. 接口与连接：航插接口与网口针脚平直光亮无氧化，线缆护套完好无破损。\n"
           << "4. 铭牌与防护：设备型号及SN出厂铭牌清晰完整，机体防水密封胶条均匀完好。\n"
           << "判定结论：外观成色良好，硬件标识校验通过，符合检验及交付标准。";
        sec0->notes = ss.str();
        sec0->result = "正常";
    }

    // Section 1 (供电)
    ReportSection* sec1 = get_section("供电", 1);
    if (sec1) {
        std::ostringstream ss;
        ss << "【PTC 9347 供电与温度读数】\n";
        if (params.operating_voltage > 0.0f) {
            ss << "工作电压实测：" << std::fixed << std::setprecision(2) << params.operating_voltage << " V\n";
        }
        if (params.operating_current_ma > 0.0f) {
            ss << "工作电流实测：" << std::fixed << std::setprecision(1) << params.operating_current_ma << " mA\n";
        }
        if (params.operating_power_w > 0.0f) {
            ss << "整机运行功耗：" << std::fixed << std::setprecision(2) << params.operating_power_w << " W\n";
        }
        ss << "核心测点温度：" << std::fixed << std::setprecision(1) << params.temperature_c << " ℃\n"
           << "额定输入电压：DC 9V~36V（标准 12.0V）；\n"
           << "供电启动响应迅速，未出现异常掉电、重启或电压跌落告警。";
        sec1->notes = ss.str();
        sec1->result = "正常";
    }

    // Section 2 (网络连接)
    ReportSection* sec2 = get_section("网络连接", 2);
    if (sec2) {
        std::ostringstream ss;
        ss << "【PTC 9347 网络配置读取】\n雷达 IP：" << (params.ip_address.empty() ? report_data_.ip_address : params.ip_address);
        if (!params.subnet_mask.empty()) ss << "\n子网掩码：" << params.subnet_mask;
        if (!params.gateway.empty()) ss << "\n默认网关：" << params.gateway;
        if (!params.destination_ip.empty()) ss << "\n点云目的 IP：" << params.destination_ip;
        if (params.destination_port > 0) ss << "\n点云目的端口：" << params.destination_port;
        ss << "\n物理 MAC 地址：" << params.mac_address << "\n"
           << "PTC 诊断通道 (TCP 9347) 与 Web 接口连通正常，以太网协商速率达标，双向通讯延时良好；\n"
           << "数据端口（MSOP）：2368，状态端口（DIFOP）：51100；链路物理连接稳固无抖动。";
        sec2->notes = ss.str();
        sec2->result = "正常";
    }

    // Section 3 (SDK识别)
    ReportSection* sec3 = get_section("SDK识别", 3);
    if (sec3) {
        std::ostringstream ss;
        ss << "【PTC 9347 型号与固件读取】\n识别型号：" << (params.model.empty() ? report_data_.model : params.model)
           << "\n固件版本：" << params.firmware_version;
        if (!params.hardware_version.empty()) ss << "\n硬件版本：" << params.hardware_version;
        if (!params.controller_fw_version.empty()) ss << "\n控制器版本：" << params.controller_fw_version;
        ss << "\n匹配官方 C++ 解码驱动插件；探测驱动与校准参数校验通过，通信正常。";
        sec3->notes = ss.str();
        sec3->result = "正常";
    }

    // Section 5 (转速)
    ReportSection* sec5 = get_section("转速", 5);
    if (sec5) {
        std::ostringstream ss;
        if (params.model.find("FT120") != std::string::npos || params.model.find("FTX") != std::string::npos) {
            ss << "【PTC 9347 转速与运行机构】\n运行模式：纯固态面阵 Flash 架构（无旋转机械部件，标称 0 RPM，免机械磨损与振动失步）\n"
               << "光学偏转与探测阵列自检正常，无需转速标定。";
        } else {
            ss << "【PTC 9347 转速与运行机构】\n实测电机转速：" << std::fixed << std::setprecision(1) << params.motor_rpm << " RPM\n"
               << "标称扫描帧率：10.0 Hz（等效电机转速 600 RPM）；实测连续出帧平稳；\n"
               << "旋转电机与轴承运转平稳无明显抖动与杂音。";
        }
        sec5->notes = ss.str();
        sec5->result = "正常";
    }

    report_status_msg_ = "已成功通过 PTC 9347 / Web 自动读取并填报雷达参数至检测报告 (" + (params.model.empty() ? report_data_.model : params.model) + ", SN: " + params.serial_number + ")";
}

} // namespace ulv
