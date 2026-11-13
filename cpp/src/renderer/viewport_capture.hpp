#pragma once

#include "camera.hpp"
#include "pointcloud_renderer.hpp"
#include "grid_renderer.hpp"
#include "core/frame_buffer.hpp"
#include "image_encoder.hpp"

#include <string>
#include <vector>
#include <cstdint>

namespace ulv {

/**
 * @brief Configuration parameters for offscreen 3D viewport capture.
 */
struct ViewportCaptureConfig {
    int width = 1280;                  // Target width (16:9 HD standard)
    int height = 720;                  // Target height (16:9 HD standard)
    float clear_color_r = 0.043f;      // Deep obsidian #0b1116
    float clear_color_g = 0.067f;
    float clear_color_b = 0.086f;
    float clear_color_a = 1.0f;
    bool draw_grid = true;             // Draw grid rings & axes
    bool draw_point_cloud = true;      // Draw 3D LiDAR point cloud
    bool fallback_to_screen = true;    // Fallback to FBO 0 if offscreen fails
    bool fallback_to_synthetic = true; // Fallback to CPU rasterizer if no GL context
};

/**
 * @brief RAII manager for an offscreen OpenGL Framebuffer Object (FBO),
 *        RGBA8 texture, and Depth/Stencil renderbuffer.
 */
class OffscreenFBO {
public:
    OffscreenFBO(int width, int height);
    ~OffscreenFBO();

    OffscreenFBO(const OffscreenFBO&) = delete;
    OffscreenFBO& operator=(const OffscreenFBO&) = delete;
    OffscreenFBO(OffscreenFBO&& other) noexcept;
    OffscreenFBO& operator=(OffscreenFBO&& other) noexcept;

    bool initialize();
    void cleanup();
    void bind();
    void unbind();

    bool read_pixels(std::vector<uint8_t>& out_pixels, bool flip_vertical = true);

    uint32_t fbo() const { return fbo_; }
    uint32_t texture() const { return color_tex_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool is_valid() const { return is_complete_; }

private:
    int width_ = 0;
    int height_ = 0;
    uint32_t fbo_ = 0;
    uint32_t color_tex_ = 0;
    uint32_t depth_rbo_ = 0;
    bool is_complete_ = false;
};

/**
 * @brief Checks if a valid, current OpenGL context is active.
 */
bool is_gl_context_available();

/**
 * @brief Captures offscreen 3D viewport into raw RGBA8 pixel buffer (width * height * 4).
 */
bool capture_viewport_rgba(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    std::vector<uint8_t>& out_rgba_pixels,
    const ViewportCaptureConfig& config = ViewportCaptureConfig{},
    const PointCloudFrame* frame = nullptr
);

/**
 * @brief Generates synthetic 3D point cloud visualization image purely in CPU memory.
 */
void generate_synthetic_pointcloud_image(
    const OrbitCamera& camera,
    const RenderSettings& settings,
    const PointCloudFrame* frame,
    std::vector<uint8_t>& out_rgba_pixels,
    int width = 1280,
    int height = 720
);

/**
 * @brief Milestone 4 Primary Contract:
 *        Captures 3D viewport rendered frame using offscreen FBO (1280x720 16:9),
 *        compresses to PNG in-memory, and returns "data:image/png;base64,..." URI string.
 */
std::string capture_viewport_png_base64(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    int width = 1280,
    int height = 720
);

/**
 * @brief Overload accepting PointCloudFrame pointer for synthetic/headless fallback.
 */
std::string capture_viewport_png_base64(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    const PointCloudFrame* active_frame,
    int width = 1280,
    int height = 720
);

} // namespace ulv
