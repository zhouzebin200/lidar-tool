#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

namespace ulv {

enum class LidarVendor {
    Unknown,
    Hesai,
    RoboSense,
    Velodyne,
    Seyond,
    Huawei
};

struct DetectedLidar {
    LidarVendor vendor = LidarVendor::Unknown;
    std::string vendor_name = "Unknown";
    std::string model = "Unknown";
    bool is_point_cloud = false;
    bool is_status_or_difop = false;
};

class LidarDetector {
public:
    static DetectedLidar identify_packet(const uint8_t* data, size_t size, uint16_t dst_port);
};

} // namespace ulv
