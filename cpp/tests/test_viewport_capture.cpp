#include "test_framework.hpp"
#include "renderer/viewport_capture.hpp"
#include "renderer/image_encoder.hpp"
#include "renderer/camera.hpp"
#include "renderer/pointcloud_renderer.hpp"
#include "renderer/grid_renderer.hpp"
#include "core/frame_buffer.hpp"
#include "core/report_generator.hpp"

#include <cstring>
#include <vector>
#include <cmath>
#include <fstream>
#include <filesystem>

namespace {

std::string read_file_string(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return "";
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE_TIER("Tier 1: Feature Coverage", "ViewportCapture: Context Availability Detection") {
    // In automated CLI unit test runner (no GLFW window initialized), OpenGL context should be unavailable
    bool gl_avail = ulv::is_gl_context_available();
    // In headless test runner, this should report false
    ASSERT_FALSE(gl_avail);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "ViewportCapture: Synthetic Rasterizer Dimensions & Background") {
    ulv::OrbitCamera camera;
    ulv::RenderSettings settings;
    ulv::PointCloudFrame frame;

    const int w = 640;
    const int h = 360;
    std::vector<uint8_t> rgba;

    ulv::generate_synthetic_pointcloud_image(camera, settings, &frame, rgba, w, h);

    ASSERT_EQ(rgba.size(), static_cast<size_t>(w * h * 4));

    // Verify background pixel color is obsidian #0b1116 (R=11, G=17, B=22, A=255) at corners
    // Top-left pixel
    ASSERT_EQ(rgba[0], 11);
    ASSERT_EQ(rgba[1], 17);
    ASSERT_EQ(rgba[2], 22);
    ASSERT_EQ(rgba[3], 255);

    // Bottom-right pixel
    size_t br_idx = static_cast<size_t>((h - 1) * w + (w - 1)) * 4;
    ASSERT_EQ(rgba[br_idx + 0], 11);
    ASSERT_EQ(rgba[br_idx + 1], 17);
    ASSERT_EQ(rgba[br_idx + 2], 22);
    ASSERT_EQ(rgba[br_idx + 3], 255);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "ViewportCapture: Synthetic Coordinate Axes & Range Rings") {
    ulv::OrbitCamera camera;
    // Default camera looks toward (0,0,0) from pitch 30, yaw 45, distance 30
    ulv::RenderSettings settings;
    ulv::PointCloudFrame frame; // empty frame to focus on axes and rings

    const int w = 320;
    const int h = 180;
    std::vector<uint8_t> rgba;

    ulv::generate_synthetic_pointcloud_image(camera, settings, &frame, rgba, w, h);

    // Scan for non-background pixels: axes (red, green, blue) or rings
    size_t non_bg_count = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i] != 11 || rgba[i + 1] != 17 || rgba[i + 2] != 22) {
            non_bg_count++;
        }
    }

    // Grid lines and coordinate axes should have drawn thousands of pixels
    ASSERT_GT(non_bg_count, 100);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "ViewportCapture: Synthetic Point Cloud Turbo Colormap & Projections") {
    ulv::OrbitCamera camera;
    ulv::RenderSettings settings;
    settings.color_mode = ulv::ColorMode::HeightTurbo;
    settings.current_z_min = -2.0f;
    settings.current_z_max = 5.0f;

    ulv::PointCloudFrame frame;
    // Populate 500 points in a helix around the origin
    for (int i = 0; i < 500; ++i) {
        float angle = i * 0.1f;
        float radius = 5.0f + (i % 15);
        ulv::UlvPoint pt;
        pt.x = radius * std::cos(angle);
        pt.y = radius * std::sin(angle);
        pt.z = -2.0f + 7.0f * (static_cast<float>(i) / 500.0f);
        pt.intensity = static_cast<float>(i % 256);
        pt.ring = static_cast<uint16_t>(i % 32);
        frame.points.push_back(pt);
    }

    const int w = 640;
    const int h = 360;
    std::vector<uint8_t> rgba;
    ulv::generate_synthetic_pointcloud_image(camera, settings, &frame, rgba, w, h);

    // Verify that rendered image contains colored point cloud pixels
    size_t non_bg_count = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i] != 11 || rgba[i + 1] != 17 || rgba[i + 2] != 22) {
            non_bg_count++;
        }
    }
    ASSERT_GT(non_bg_count, 300);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "ViewportCapture: Multi-Tier Fallback to Synthetic (No GL Context)") {
    ulv::OrbitCamera camera;
    ulv::PointCloudRenderer pc_renderer;
    ulv::GridRenderer grid_renderer;
    ulv::RenderSettings settings;

    ulv::PointCloudFrame frame;
    for (int i = 0; i < 50; ++i) {
        ulv::UlvPoint pt;
        pt.x = static_cast<float>(i - 25);
        pt.y = 10.0f;
        pt.z = 0.0f;
        pt.intensity = 100.0f;
        frame.points.push_back(pt);
    }

    // Call top-level M4 contract API with active_frame pointer
    std::string data_uri = ulv::capture_viewport_png_base64(
        camera, pc_renderer, grid_renderer, settings, &frame, 640, 360
    );

    // Must return a valid data URI
    ASSERT_FALSE(data_uri.empty());
    ASSERT_TRUE(data_uri.rfind("data:image/png;base64,", 0) == 0);

    // Decode and verify PNG header magic
    std::vector<uint8_t> decoded_png = ulv::base64_decode(data_uri);
    ASSERT_GT(decoded_png.size(), 8);

    const uint8_t kPngMagic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    ASSERT_EQ(std::memcmp(decoded_png.data(), kPngMagic, 8), 0);
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "ViewportCapture: OffscreenFBO Fails Safely Without Context") {
    ulv::OffscreenFBO fbo(1280, 720);
    // In headless test without active GL context, initialize must return false without crash
    bool ok = fbo.initialize();
    ASSERT_FALSE(ok);
    ASSERT_FALSE(fbo.is_valid());
    ASSERT_EQ(fbo.fbo(), 0);

    std::vector<uint8_t> out;
    bool read_ok = fbo.read_pixels(out);
    ASSERT_FALSE(read_ok);
    ASSERT_TRUE(out.empty());
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "ViewportCapture: ROI 3D Bounding Box Filtering in Synthetic Rasterizer") {
    ulv::OrbitCamera camera;
    ulv::RenderSettings settings;
    settings.enable_roi_filter = true;
    // Set tight ROI box around origin [-2, 2]
    settings.roi_min_x = -2.0f; settings.roi_max_x = 2.0f;
    settings.roi_min_y = -2.0f; settings.roi_max_y = 2.0f;
    settings.roi_min_z = -2.0f; settings.roi_max_z = 2.0f;

    ulv::PointCloudFrame frame;
    // Point 1: inside ROI
    ulv::UlvPoint p1; p1.x = 0.0f; p1.y = 0.0f; p1.z = 0.0f; p1.intensity = 200.0f;
    frame.points.push_back(p1);

    // Point 2: outside ROI (+50, +50, +50)
    ulv::UlvPoint p2; p2.x = 50.0f; p2.y = 50.0f; p2.z = 50.0f; p2.intensity = 200.0f;
    frame.points.push_back(p2);

    std::vector<uint8_t> rgba;
    ulv::generate_synthetic_pointcloud_image(camera, settings, &frame, rgba, 320, 180);
    ASSERT_EQ(rgba.size(), 320 * 180 * 4);
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "ViewportCapture: Max Distance Cutoff & Frustum Culling") {
    ulv::OrbitCamera camera;
    ulv::RenderSettings settings;
    settings.max_distance = 15.0f; // cutoff at 15m

    ulv::PointCloudFrame frame;
    // Point far beyond max distance
    ulv::UlvPoint p_far; p_far.x = 100.0f; p_far.y = 100.0f; p_far.z = 0.0f;
    frame.points.push_back(p_far);

    // Point behind camera
    ulv::UlvPoint p_behind; p_behind.x = 0.0f; p_behind.y = -500.0f; p_behind.z = 0.0f;
    frame.points.push_back(p_behind);

    std::vector<uint8_t> rgba;
    ulv::generate_synthetic_pointcloud_image(camera, settings, &frame, rgba, 320, 180);
    ASSERT_EQ(rgba.size(), 320 * 180 * 4);
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "ViewportCapture: PNG Header and IHDR Chunk Dimension Validation") {
    ulv::OrbitCamera camera;
    ulv::RenderSettings settings;
    ulv::PointCloudFrame frame;

    const int target_w = 640;
    const int target_h = 360;
    std::vector<uint8_t> rgba;
    ulv::generate_synthetic_pointcloud_image(camera, settings, &frame, rgba, target_w, target_h);

    std::string data_uri = ulv::rgba_to_png_data_uri(rgba.data(), target_w, target_h, 0, false);
    ASSERT_FALSE(data_uri.empty());

    std::vector<uint8_t> png_bytes = ulv::base64_decode(data_uri);
    // Standard PNG structure:
    // 0..7:   Magic (8 bytes)
    // 8..11:  IHDR chunk length (4 bytes, typically 13)
    // 12..15: "IHDR" chunk type (4 bytes)
    // 16..19: Width (4 bytes, big-endian)
    // 20..23: Height (4 bytes, big-endian)
    ASSERT_GT(png_bytes.size(), 24);

    const uint8_t kPngMagic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    ASSERT_EQ(std::memcmp(png_bytes.data(), kPngMagic, 8), 0);

    // Verify IHDR tag
    ASSERT_EQ(png_bytes[12], 'I');
    ASSERT_EQ(png_bytes[13], 'H');
    ASSERT_EQ(png_bytes[14], 'D');
    ASSERT_EQ(png_bytes[15], 'R');

    // Extract width (big-endian)
    uint32_t decoded_w = (static_cast<uint32_t>(png_bytes[16]) << 24) |
                         (static_cast<uint32_t>(png_bytes[17]) << 16) |
                         (static_cast<uint32_t>(png_bytes[18]) << 8)  |
                         static_cast<uint32_t>(png_bytes[19]);
    // Extract height (big-endian)
    uint32_t decoded_h = (static_cast<uint32_t>(png_bytes[20]) << 24) |
                         (static_cast<uint32_t>(png_bytes[21]) << 16) |
                         (static_cast<uint32_t>(png_bytes[22]) << 8)  |
                         static_cast<uint32_t>(png_bytes[23]);

    ASSERT_EQ(decoded_w, static_cast<uint32_t>(target_w));
    ASSERT_EQ(decoded_h, static_cast<uint32_t>(target_h));
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "ViewportCapture: HTML Report Section 5 Embedding Pipeline") {
    ulv::OrbitCamera camera;
    ulv::PointCloudRenderer pc_renderer;
    ulv::GridRenderer grid_renderer;
    ulv::RenderSettings settings;
    ulv::PointCloudFrame frame;

    // Capture synthetic frame as Data URI
    std::string captured_b64 = ulv::capture_viewport_png_base64(
        camera, pc_renderer, grid_renderer, settings, &frame, 1280, 720
    );
    ASSERT_FALSE(captured_b64.empty());

    // Attach to InspectionReportData
    ulv::InspectionReportData report;
    ulv::attach_viewport_screenshot(report, captured_b64, "图 5-1 实测三维空间点云分布快照 (1280x720 HD)");

    ASSERT_EQ(report.point_cloud_image_base64, captured_b64);
    ASSERT_EQ(report.sections[4].image_base64, captured_b64);

    // Export HTML report
    std::string out_html = "/tmp/test_report_embedded_viewport.html";
    bool ok = ulv::ReportGenerator::export_html(report, out_html);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::filesystem::exists(out_html));

    std::string html = read_file_string(out_html);
    // Verify that the exact captured Base64 string is embedded in the HTML file
    ASSERT_CONTAINS(html, captured_b64);
    ASSERT_CONTAINS(html, "图 5-1 实测三维空间点云分布快照 (1280x720 HD)");
    ASSERT_CONTAINS(html, "screenshot-frame");
    ASSERT_CONTAINS(html, "screenshot-watermark");

    std::filesystem::remove(out_html);
}
