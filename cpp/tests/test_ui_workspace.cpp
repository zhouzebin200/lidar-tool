#include "imgui.h"
#include "ui/ui_workspace.hpp"
#include "ui/ui_dpi.hpp"
#include "ui/ui_theme.hpp"
#include "core/replay_preview.hpp"
#include "renderer/color_mapping.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

static void test_dpi_adapter() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(3200.0f, 1920.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(1600.0f, 960.0f);

    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);

    ulv::UIDpiAdapter adapter;
    adapter.before_frame(2.0f);

    assert(std::abs(io.DisplaySize.x - 1600.0f) < 0.01f);
    assert(std::abs(io.DisplaySize.y - 960.0f) < 0.01f);
    assert(std::abs(io.DisplayFramebufferScale.x - 2.0f) < 0.01f);
    assert(std::abs(io.DisplayFramebufferScale.y - 2.0f) < 0.01f);

    ImGui::NewFrame();
    assert(std::abs(io.MousePos.x - 800.0f) < 0.01f);
    assert(std::abs(io.MousePos.y - 480.0f) < 0.01f);
    ImGui::EndFrame();

    // Scale <= 1.0f must be a no-op
    io.DisplaySize = ImVec2(1440.0f, 900.0f);
    io.DisplayFramebufferScale = ImVec2(2.0f, 2.0f);
    io.AddMousePosEvent(400.0f, 300.0f);
    adapter.before_frame(1.0f);
    assert(std::abs(io.DisplaySize.x - 1440.0f) < 0.01f);
    assert(std::abs(io.DisplayFramebufferScale.x - 2.0f) < 0.01f);

    ImGui::NewFrame();
    assert(std::abs(io.MousePos.x - 400.0f) < 0.01f);
    ImGui::EndFrame();

    ImGui::DestroyContext();
}

static void test_replay_preview() {
    ulv::ReplayPreview preview;
    assert(!preview.pending());
    assert(!preview.exhausted());

    preview.request();
    assert(preview.pending());
    assert(!preview.exhausted());

    preview.observe(true, false);
    assert(!preview.pending());
    assert(!preview.exhausted());

    preview.request();
    preview.observe(false, true);
    assert(!preview.pending());
    assert(preview.exhausted());

    preview.reset();
    assert(!preview.pending());
    assert(!preview.exhausted());
}

static void test_headless_workspace_render() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1100.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);

    ulv::UITheme::apply_dark_industrial_theme();
    ulv::UIWorkspace workspace;
    ulv::RenderSettings settings;
    ulv::OrbitCamera camera;
    ulv::PcapReader pcap_reader;
    ulv::PointCloudFrame frame;
    ulv::AppMode mode = ulv::AppMode::Idle;
    bool is_playing = false;
    float speed = 1.0f;
    bool loop = true;
    std::string status = "等待数据";
    ulv::PtpStatus ptp;

    ImGui::NewFrame();
    workspace.render_ui(settings, camera, pcap_reader, frame, mode, is_playing, speed, loop, status, ptp, 0.0f);
    ImGui::Render();

    assert(!workspace.is_report_modal_open());
    assert(ulv::legend_range(settings).visible);
    ImGui::DestroyContext();
}

int main() {
    test_dpi_adapter();
    test_replay_preview();
    test_headless_workspace_render();
    std::cout << "[ulv_ui_tests] All UI & DPI checks passed." << std::endl;
    return 0;
}
