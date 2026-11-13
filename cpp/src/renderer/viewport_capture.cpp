#include "viewport_capture.hpp"
#include "image_encoder.hpp"
#include "ulv_gl.hpp"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#endif

namespace ulv {

namespace {

// RAII Guard for preserving active OpenGL state across offscreen capture
struct GLStateBackup {
    GLint prev_draw_fbo = 0;
    GLint prev_read_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLboolean prev_scissor_test = GL_FALSE;
    GLint prev_scissor_box[4] = {0, 0, 0, 0};
    GLfloat prev_clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GLint prev_pack_alignment = 4;

    GLStateBackup() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_viewport);
        glGetBooleanv(GL_SCISSOR_TEST, &prev_scissor_test);
        glGetIntegerv(GL_SCISSOR_BOX, prev_scissor_box);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear_color);
        glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_alignment);
    }

    ~GLStateBackup() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
        if (prev_scissor_test) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(prev_scissor_box[0], prev_scissor_box[1], prev_scissor_box[2], prev_scissor_box[3]);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glClearColor(prev_clear_color[0], prev_clear_color[1], prev_clear_color[2], prev_clear_color[3]);
        glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_alignment);
    }
};

// Turbo colormap polynomial interpolation matching pointcloud_renderer.cpp
inline void eval_turbo_color(float x, uint8_t& out_r, uint8_t& out_g, uint8_t& out_b) {
    x = std::clamp(x, 0.0f, 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (x < 0.25f) {
        float t = x * 4.0f;
        r = 0.08f * (1.0f - t) + 0.0f * t;
        g = 0.08f * (1.0f - t) + 0.75f * t;
        b = 0.45f * (1.0f - t) + 1.0f * t;
    } else if (x < 0.50f) {
        float t = (x - 0.25f) * 4.0f;
        r = 0.0f * (1.0f - t) + 0.10f * t;
        g = 0.75f * (1.0f - t) + 0.95f * t;
        b = 1.0f * (1.0f - t) + 0.35f * t;
    } else if (x < 0.75f) {
        float t = (x - 0.50f) * 4.0f;
        r = 0.10f * (1.0f - t) + 1.0f * t;
        g = 0.95f * (1.0f - t) + 0.85f * t;
        b = 0.35f * (1.0f - t) + 0.05f * t;
    } else {
        float t = (x - 0.75f) * 4.0f;
        r = 1.0f * (1.0f - t) + 0.85f * t;
        g = 0.85f * (1.0f - t) + 0.05f * t;
        b = 0.05f * (1.0f - t) + 0.03f * t;
    }
    out_r = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
    out_g = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
    out_b = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
}

// Bresenham line drawer for CPU fallback
template <typename PixelFunc>
void draw_line_dda(PixelFunc&& put_pixel, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        put_pixel(x0, y0, r, g, b, a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

} // namespace

bool is_gl_context_available() {
#if defined(__APPLE__)
    if (CGLGetCurrentContext() == nullptr) {
        return false;
    }
#else
    // On Windows/Linux with GLAD:
    if (glGenFramebuffers == nullptr || glBindFramebuffer == nullptr ||
        glGenTextures == nullptr || glReadPixels == nullptr) {
        return false;
    }
#endif
    return true;
}

OffscreenFBO::OffscreenFBO(int width, int height)
    : width_(width), height_(height) {}

OffscreenFBO::~OffscreenFBO() {
    cleanup();
}

OffscreenFBO::OffscreenFBO(OffscreenFBO&& o) noexcept
    : width_(o.width_), height_(o.height_), fbo_(o.fbo_),
      color_tex_(o.color_tex_), depth_rbo_(o.depth_rbo_),
      is_complete_(o.is_complete_) {
    o.fbo_ = 0;
    o.color_tex_ = 0;
    o.depth_rbo_ = 0;
    o.is_complete_ = false;
}

OffscreenFBO& OffscreenFBO::operator=(OffscreenFBO&& o) noexcept {
    if (this != &o) {
        cleanup();
        width_ = o.width_;
        height_ = o.height_;
        fbo_ = o.fbo_;
        color_tex_ = o.color_tex_;
        depth_rbo_ = o.depth_rbo_;
        is_complete_ = o.is_complete_;
        o.fbo_ = 0;
        o.color_tex_ = 0;
        o.depth_rbo_ = 0;
        o.is_complete_ = false;
    }
    return *this;
}

bool OffscreenFBO::initialize() {
    if (!is_gl_context_available()) {
        return false;
    }

    cleanup();

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // 1. Color Texture Attachment (RGBA8)
    glGenTextures(1, &color_tex_);
    glBindTexture(GL_TEXTURE_2D, color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);

    // 2. Depth/Stencil Renderbuffer Attachment (DEPTH24_STENCIL8)
    glGenRenderbuffers(1, &depth_rbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rbo_);

    // 3. Completeness Verification
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[OffscreenFBO] Framebuffer incomplete! Status: " << status << std::endl;
        cleanup();
        return false;
    }

    is_complete_ = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void OffscreenFBO::cleanup() {
    if (depth_rbo_) {
        glDeleteRenderbuffers(1, &depth_rbo_);
        depth_rbo_ = 0;
    }
    if (color_tex_) {
        glDeleteTextures(1, &color_tex_);
        color_tex_ = 0;
    }
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    is_complete_ = false;
}

void OffscreenFBO::bind() {
    if (fbo_) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    }
}

void OffscreenFBO::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool OffscreenFBO::read_pixels(std::vector<uint8_t>& out_pixels, bool flip_vertical) {
    if (!is_complete_ || !fbo_) return false;

    out_pixels.resize(static_cast<size_t>(width_) * height_ * 4);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, out_pixels.data());

    if (flip_vertical) {
        flip_image_vertical(out_pixels.data(), width_, height_, 4);
    }
    return true;
}

void generate_synthetic_pointcloud_image(
    const OrbitCamera& camera,
    const RenderSettings& settings,
    const PointCloudFrame* frame,
    std::vector<uint8_t>& out_rgba_pixels,
    int width,
    int height
) {
    out_rgba_pixels.assign(static_cast<size_t>(width) * height * 4, 0);

    // 1. Fill background with deep obsidian #0b1116
    const uint8_t bg_r = 11, bg_g = 17, bg_b = 22, bg_a = 255;
    for (size_t i = 0; i < out_rgba_pixels.size(); i += 4) {
        out_rgba_pixels[i + 0] = bg_r;
        out_rgba_pixels[i + 1] = bg_g;
        out_rgba_pixels[i + 2] = bg_b;
        out_rgba_pixels[i + 3] = bg_a;
    }

    auto put_pixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        size_t idx = (static_cast<size_t>(y) * width + x) * 4;
        if (a == 255) {
            out_rgba_pixels[idx + 0] = r;
            out_rgba_pixels[idx + 1] = g;
            out_rgba_pixels[idx + 2] = b;
            out_rgba_pixels[idx + 3] = 255;
        } else {
            float alpha = a / 255.0f;
            out_rgba_pixels[idx + 0] = static_cast<uint8_t>(r * alpha + out_rgba_pixels[idx + 0] * (1.0f - alpha));
            out_rgba_pixels[idx + 1] = static_cast<uint8_t>(g * alpha + out_rgba_pixels[idx + 1] * (1.0f - alpha));
            out_rgba_pixels[idx + 2] = static_cast<uint8_t>(b * alpha + out_rgba_pixels[idx + 2] * (1.0f - alpha));
            out_rgba_pixels[idx + 3] = 255;
        }
    };

    float aspect = (height > 0) ? (static_cast<float>(width) / static_cast<float>(height)) : (16.0f / 9.0f);
    Mat4 mvp = camera.get_projection_matrix(aspect) * camera.get_view_matrix();

    auto project_point = [&](float x, float y, float z, int& out_px, int& out_py) -> bool {
        float clip_x = mvp.m[0] * x + mvp.m[4] * y + mvp.m[8]  * z + mvp.m[12];
        float clip_y = mvp.m[1] * x + mvp.m[5] * y + mvp.m[9]  * z + mvp.m[13];
        float clip_z = mvp.m[2] * x + mvp.m[6] * y + mvp.m[10] * z + mvp.m[14];
        float clip_w = mvp.m[3] * x + mvp.m[7] * y + mvp.m[11] * z + mvp.m[15];

        if (clip_w <= 0.001f) return false;
        float ndc_x = clip_x / clip_w;
        float ndc_y = clip_y / clip_w;
        float ndc_z = clip_z / clip_w;

        if (ndc_x < -1.0f || ndc_x > 1.0f || ndc_y < -1.0f || ndc_y > 1.0f || ndc_z < -1.0f || ndc_z > 1.0f) {
            return false;
        }

        out_px = static_cast<int>((ndc_x + 1.0f) * 0.5f * width);
        out_py = static_cast<int>((1.0f - ndc_y) * 0.5f * height);
        return true;
    };

    // 2. Coordinate Axes at (0,0,0)
    const float kAxisLen = 6.0f;
    int ox, oy, xx, xy, yx, yy, zx, zy;
    if (project_point(0, 0, 0, ox, oy)) {
        if (project_point(kAxisLen, 0, 0, xx, xy)) draw_line_dda(put_pixel, ox, oy, xx, xy, 240, 50, 50);  // X: Red
        if (project_point(0, kAxisLen, 0, yx, yy)) draw_line_dda(put_pixel, ox, oy, yx, yy, 50, 240, 50);  // Y: Green
        if (project_point(0, 0, kAxisLen, zx, zy)) draw_line_dda(put_pixel, ox, oy, zx, zy, 60, 130, 255); // Z: Blue
    }

    // 3. Concentric Range Rings (10m, 20m, 30m, 50m)
    const float radii[] = {10.0f, 20.0f, 30.0f, 50.0f};
    for (float r : radii) {
        const int kSegs = 90;
        int prev_px = 0, prev_py = 0;
        bool has_prev = false;
        for (int s = 0; s <= kSegs; ++s) {
            float th = (2.0f * 3.14159265f * (s % kSegs)) / kSegs;
            int px, py;
            if (project_point(r * std::cos(th), r * std::sin(th), 0.0f, px, py)) {
                if (has_prev) {
                    draw_line_dda(put_pixel, prev_px, prev_py, px, py, 75, 95, 120, 140);
                }
                prev_px = px; prev_py = py; has_prev = true;
            } else {
                has_prev = false;
            }
        }
    }

    // 4. Point Cloud Rasterization
    if (frame && !frame->points.empty()) {
        float z_min = settings.current_z_min;
        float z_max = settings.current_z_max;
        float z_range = std::max(0.01f, z_max - z_min);
        float int_max = std::max(0.01f, settings.current_intensity_max);

        for (const auto& pt : frame->points) {
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) continue;
            float range = std::sqrt(pt.x * pt.x + pt.y * pt.y + pt.z * pt.z);
            if (range > settings.max_distance) continue;

            if (settings.enable_roi_filter) {
                if (pt.x < settings.roi_min_x || pt.x > settings.roi_max_x ||
                    pt.y < settings.roi_min_y || pt.y > settings.roi_max_y ||
                    pt.z < settings.roi_min_z || pt.z > settings.roi_max_z) {
                    continue;
                }
            }

            int px, py;
            if (project_point(pt.x, pt.y, pt.z, px, py)) {
                uint8_t cr, cg, cb;
                if (settings.color_mode == ColorMode::HeightTurbo) {
                    float val = std::clamp((pt.z - z_min) / z_range, 0.0f, 1.0f);
                    eval_turbo_color(val, cr, cg, cb);
                } else if (settings.color_mode == ColorMode::IntensityGray) {
                    float val = std::clamp(pt.intensity / int_max, 0.0f, 1.0f);
                    uint8_t g = static_cast<uint8_t>(std::pow(val, 0.55f) * 255.0f);
                    cr = cg = cb = g;
                } else if (settings.color_mode == ColorMode::DistanceTurbo) {
                    float val = std::clamp(range / std::max(1.0f, settings.max_distance), 0.0f, 1.0f);
                    eval_turbo_color(val, cr, cg, cb);
                } else {
                    cr = 220; cg = 240; cb = 255;
                }

                put_pixel(px, py, cr, cg, cb);
                if (settings.point_size >= 2.0f) {
                    put_pixel(px + 1, py, cr, cg, cb);
                    put_pixel(px, py + 1, cr, cg, cb);
                    put_pixel(px + 1, py + 1, cr, cg, cb);
                }
            }
        }
    }
}

bool capture_viewport_rgba(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    std::vector<uint8_t>& out_rgba_pixels,
    const ViewportCaptureConfig& config,
    const PointCloudFrame* frame
) {
    if (!is_gl_context_available()) {
        if (config.fallback_to_synthetic) {
            generate_synthetic_pointcloud_image(camera, settings, frame, out_rgba_pixels, config.width, config.height);
            return true;
        }
        return false;
    }

    GLStateBackup state_guard;

    // Attempt Tier 1: Offscreen FBO
    OffscreenFBO fbo(config.width, config.height);
    if (fbo.initialize()) {
        fbo.bind();
        glViewport(0, 0, config.width, config.height);
        glDisable(GL_SCISSOR_TEST);

        glClearColor(config.clear_color_r, config.clear_color_g, config.clear_color_b, config.clear_color_a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float aspect = static_cast<float>(config.width) / static_cast<float>(config.height);
        Mat4 mvp = camera.get_projection_matrix(aspect) * camera.get_view_matrix();

        if (config.draw_grid) {
            grid_renderer.render(mvp);
        }
        if (config.draw_point_cloud) {
            pc_renderer.render(mvp, settings);
        }

        if (fbo.read_pixels(out_rgba_pixels, true)) {
            return true;
        }
    }

    // Attempt Tier 2: Screen Framebuffer read fallback
    if (config.fallback_to_screen) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        out_rgba_pixels.resize(config.width * config.height * 4);
        glReadPixels(0, 0, config.width, config.height, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba_pixels.data());
        flip_image_vertical(out_rgba_pixels.data(), config.width, config.height, 4);
        return true;
    }

    // Attempt Tier 3: Synthetic Rasterizer fallback
    if (config.fallback_to_synthetic) {
        generate_synthetic_pointcloud_image(camera, settings, frame, out_rgba_pixels, config.width, config.height);
        return true;
    }

    return false;
}

std::string capture_viewport_png_base64(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    int width,
    int height
) {
    return capture_viewport_png_base64(camera, pc_renderer, grid_renderer, settings, nullptr, width, height);
}

std::string capture_viewport_png_base64(
    OrbitCamera& camera,
    PointCloudRenderer& pc_renderer,
    GridRenderer& grid_renderer,
    const RenderSettings& settings,
    const PointCloudFrame* active_frame,
    int width,
    int height
) {
    ViewportCaptureConfig config;
    config.width = width;
    config.height = height;

    std::vector<uint8_t> rgba;
    if (!capture_viewport_rgba(camera, pc_renderer, grid_renderer, settings, rgba, config, active_frame)) {
        return "";
    }

    return rgba_to_png_data_uri(rgba.data(), width, height, 0, false);
}

} // namespace ulv
