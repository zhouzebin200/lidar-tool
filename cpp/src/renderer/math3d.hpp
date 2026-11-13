#pragma once

#include <cmath>
#include <array>

namespace ulv {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        float l = length();
        if (l < 1e-6f) return {0, 0, 0};
        return {x / l, y / l, z / l};
    }

    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    static float dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
};

struct Mat4 {
    std::array<float, 16> m{};

    Mat4() {
        m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
    }

    static Mat4 identity() {
        return Mat4();
    }

    static Mat4 perspective(float fov_rad, float aspect, float z_near, float z_far) {
        Mat4 res;
        float tan_half = std::tan(fov_rad / 2.0f);
        res.m.fill(0.0f);
        res.m[0] = 1.0f / (aspect * tan_half);
        res.m[5] = 1.0f / tan_half;
        res.m[10] = -(z_far + z_near) / (z_far - z_near);
        res.m[11] = -1.0f;
        res.m[14] = -(2.0f * z_far * z_near) / (z_far - z_near);
        res.m[15] = 0.0f;
        return res;
    }

    static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = (target - eye).normalized();
        Vec3 s = Vec3::cross(f, up).normalized();
        Vec3 u = Vec3::cross(s, f);

        Mat4 res;
        res.m[0] = s.x;  res.m[4] = s.y;  res.m[8]  = s.z;  res.m[12] = -Vec3::dot(s, eye);
        res.m[1] = u.x;  res.m[5] = u.y;  res.m[9]  = u.z;  res.m[13] = -Vec3::dot(u, eye);
        res.m[2] = -f.x; res.m[6] = -f.y; res.m[10] = -f.z; res.m[14] = Vec3::dot(f, eye);
        res.m[3] = 0.0f; res.m[7] = 0.0f; res.m[11] = 0.0f; res.m[15] = 1.0f;
        return res;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 res;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                res.m[j * 4 + i] =
                    m[0 * 4 + i] * b.m[j * 4 + 0] +
                    m[1 * 4 + i] * b.m[j * 4 + 1] +
                    m[2 * 4 + i] * b.m[j * 4 + 2] +
                    m[3 * 4 + i] * b.m[j * 4 + 3];
            }
        }
        return res;
    }
};

} // namespace ulv
