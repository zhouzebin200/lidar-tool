#pragma once

#include "math3d.hpp"
#include <algorithm>

namespace ulv {

enum class ViewPreset {
    Isometric,
    Top,
    Front,
    Side
};

class OrbitCamera {
public:
    OrbitCamera() {
        reset_view(ViewPreset::Isometric);
    }

    void reset_view(ViewPreset preset) {
        pivot_ = {0.0f, 0.0f, 0.0f};
        switch (preset) {
            case ViewPreset::Isometric:
                yaw_deg_ = 45.0f;
                pitch_deg_ = 35.0f;
                distance_ = 40.0f;
                break;
            case ViewPreset::Top:
                yaw_deg_ = 0.0f;
                pitch_deg_ = 89.9f;
                distance_ = 50.0f;
                break;
            case ViewPreset::Front:
                yaw_deg_ = 0.0f;
                pitch_deg_ = 0.0f;
                distance_ = 40.0f;
                break;
            case ViewPreset::Side:
                yaw_deg_ = 90.0f;
                pitch_deg_ = 0.0f;
                distance_ = 40.0f;
                break;
        }
    }

    void rotate(float delta_x, float delta_y) {
        yaw_deg_ += delta_x * 0.4f;
        pitch_deg_ += delta_y * 0.4f;

        // Clamp pitch to avoid gimbal flipping
        pitch_deg_ = std::clamp(pitch_deg_, -89.9f, 89.9f);

        // Keep yaw in [-180, 180]
        while (yaw_deg_ > 180.0f) yaw_deg_ -= 360.0f;
        while (yaw_deg_ < -180.0f) yaw_deg_ += 360.0f;
    }

    void pan(float delta_x, float delta_y) {
        float factor = distance_ * 0.0015f;
        float yaw_rad = yaw_deg_ * (3.14159265f / 180.0f);
        float pitch_rad = pitch_deg_ * (3.14159265f / 180.0f);

        // Camera right and up vectors in world coordinates (assuming Z is up)
        Vec3 forward = {
            std::cos(pitch_rad) * std::sin(yaw_rad),
            std::cos(pitch_rad) * std::cos(yaw_rad),
            std::sin(pitch_rad)
        };
        Vec3 world_up = {0.0f, 0.0f, 1.0f};
        Vec3 right = Vec3::cross(forward, world_up).normalized();
        Vec3 up = Vec3::cross(right, forward).normalized();

        pivot_ = pivot_ - right * (delta_x * factor) + up * (delta_y * factor);
    }

    void zoom(float wheel_delta) {
        if (std::abs(wheel_delta) < 0.0001f) return;
        // Exponential scale guarantees factor is strictly > 0 and prevents camera inversion or collapsing
        float factor = std::exp(-wheel_delta * 0.08f);
        distance_ *= factor;
        distance_ = std::clamp(distance_, 0.5f, 1000.0f);
    }

    Mat4 get_view_matrix() const {
        float yaw_rad = yaw_deg_ * (3.14159265f / 180.0f);
        float pitch_rad = pitch_deg_ * (3.14159265f / 180.0f);

        Vec3 eye = {
            pivot_.x + distance_ * std::cos(pitch_rad) * std::sin(yaw_rad),
            pivot_.y + distance_ * std::cos(pitch_rad) * std::cos(yaw_rad),
            pivot_.z + distance_ * std::sin(pitch_rad)
        };

        Vec3 up = {0.0f, 0.0f, 1.0f};
        // Avoid singularity when looking straight up/down
        if (std::abs(pitch_deg_) > 89.0f) {
            up = {0.0f, 1.0f, 0.0f};
        }

        return Mat4::look_at(eye, pivot_, up);
    }

    Mat4 get_projection_matrix(float aspect_ratio) const {
        float fov = 45.0f * (3.14159265f / 180.0f);
        return Mat4::perspective(fov, aspect_ratio, 0.1f, 2000.0f);
    }

    Vec3 pivot() const { return pivot_; }
    float distance() const { return distance_; }
    float yaw() const { return yaw_deg_; }
    float pitch() const { return pitch_deg_; }

private:
    Vec3 pivot_{0.0f, 0.0f, 0.0f};
    float yaw_deg_ = 45.0f;
    float pitch_deg_ = 35.0f;
    float distance_ = 40.0f;
};

} // namespace ulv
