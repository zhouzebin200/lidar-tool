#pragma once

#include "ulv_point_types.hpp"
#include "math3d.hpp"
#include <cstdint>

namespace ulv {

enum class ColorMode : int {
    HeightTurbo = 0,     // 高度 · Turbo 伪彩 (与网页端一致)
    IntensityGray = 1,   // 反射强度 · 灰度 (带 Gamma 矫正)
    DistanceTurbo = 2,   // 测距 · Turbo 伪彩
    RingChannel = 3,     // 扫描线 · 分类色
    Solid = 4            // 单一纯色
};

struct RenderSettings {
    ColorMode color_mode = ColorMode::HeightTurbo;
    float point_size = 2.5f;
    bool circular_points = true;

    // 自适应高度与强度归一化参数
    bool auto_range = true;
    float current_z_min = -2.0f;
    float current_z_max = 3.0f;
    float current_intensity_max = 255.0f;

    // 最大测距距离截断
    float max_distance = 120.0f;

    // ROI Box filter
    bool enable_roi_filter = false;
    float roi_min_x = -100.0f, roi_max_x = 100.0f;
    float roi_min_y = -100.0f, roi_max_y = 100.0f;
    float roi_min_z = -10.0f,  roi_max_z = 20.0f;
};

class PointCloudRenderer {
public:
    PointCloudRenderer();
    ~PointCloudRenderer();

    bool init();
    void cleanup();

    void upload_points(const UlvPoint* data, size_t count);
    void render(const Mat4& mvp, const RenderSettings& settings);

    size_t point_count() const { return current_point_count_; }

private:
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    uint32_t shader_program_ = 0;
    size_t current_point_count_ = 0;
    size_t vbo_capacity_ = 0;

    int u_mvp_loc_ = -1;
    int u_color_mode_loc_ = -1;
    int u_point_size_loc_ = -1;
    int u_circular_loc_ = -1;
    int u_max_distance_loc_ = -1;
    int u_z_min_loc_ = -1;
    int u_z_max_loc_ = -1;
    int u_intensity_max_loc_ = -1;
    int u_roi_min_loc_ = -1;
    int u_roi_max_loc_ = -1;
    int u_roi_enable_loc_ = -1;

    bool compile_shaders();
};

} // namespace ulv
