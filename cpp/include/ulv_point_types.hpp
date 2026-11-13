#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace ulv {

#pragma pack(push, 1)
struct UlvPoint {
    float x;
    float y;
    float z;
    float intensity;
    uint16_t ring;
    uint8_t return_type;
    uint8_t flags;
};
#pragma pack(pop)

static_assert(sizeof(UlvPoint) == 20, "UlvPoint must be exactly 20 bytes for zero-copy VBO upload");

struct PointCloudBoundingBox {
    float min_x = 0.0f, max_x = 0.0f;
    float min_y = 0.0f, max_y = 0.0f;
    float min_z = 0.0f, max_z = 0.0f;
    float min_intensity = 0.0f, max_intensity = 255.0f;
    float min_distance = 0.0f, max_distance = 0.0f;
};

struct PointCloudFrame {
    uint32_t device_id = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    std::string vendor;
    std::string model;
    std::vector<UlvPoint> points;
    PointCloudBoundingBox bounds;

    void compute_bounds() {
        if (points.empty()) {
            bounds = {};
            return;
        }

        bounds.min_x = bounds.max_x = points[0].x;
        bounds.min_y = bounds.max_y = points[0].y;
        bounds.min_z = bounds.max_z = points[0].z;
        bounds.min_intensity = bounds.max_intensity = points[0].intensity;
        float d0 = std::sqrt(points[0].x * points[0].x + points[0].y * points[0].y + points[0].z * points[0].z);
        bounds.min_distance = bounds.max_distance = d0;

        for (size_t i = 1; i < points.size(); ++i) {
            const auto& pt = points[i];
            if (pt.x < bounds.min_x) bounds.min_x = pt.x;
            if (pt.x > bounds.max_x) bounds.max_x = pt.x;
            if (pt.y < bounds.min_y) bounds.min_y = pt.y;
            if (pt.y > bounds.max_y) bounds.max_y = pt.y;
            if (pt.z < bounds.min_z) bounds.min_z = pt.z;
            if (pt.z > bounds.max_z) bounds.max_z = pt.z;

            if (pt.intensity < bounds.min_intensity) bounds.min_intensity = pt.intensity;
            if (pt.intensity > bounds.max_intensity) bounds.max_intensity = pt.intensity;

            float dist = std::sqrt(pt.x * pt.x + pt.y * pt.y + pt.z * pt.z);
            if (dist < bounds.min_distance) bounds.min_distance = dist;
            if (dist > bounds.max_distance) bounds.max_distance = dist;
        }
    }
};

} // namespace ulv
