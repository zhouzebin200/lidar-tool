#include "detector.hpp"
#include <cstring>

namespace ulv {

DetectedLidar LidarDetector::identify_packet(const uint8_t* raw_data, size_t raw_size, uint16_t dst_port) {
    DetectedLidar result;
    if (!raw_data || raw_size < 4) return result;

    const uint8_t* data = raw_data;
    size_t size = raw_size;

    // Check for automotive SOME/IP or custom encapsulation (e.g. 8, 12, or 16-byte prepended headers)
    if (size >= 12) {
        if ((data[0] != 0x55 || data[1] != 0xAA) &&
            (data[0] != 0xEE || data[1] != 0xFF) &&
            (data[0] != 0xFF || data[1] != 0xEE)) {
            if ((data[8] == 0x55 && data[9] == 0xAA) ||
                (data[8] == 0xEE && data[9] == 0xFF) ||
                (data[8] == 0xFF && data[9] == 0xEE)) {
                data += 8;
                size -= 8;
            } else if (size >= 16 && ((data[12] == 0x55 && data[13] == 0xAA) ||
                                      (data[12] == 0xEE && data[13] == 0xFF) ||
                                      (data[12] == 0xFF && data[13] == 0xEE))) {
                data += 12;
                size -= 12;
            } else if (size >= 20 && ((data[16] == 0x55 && data[17] == 0xAA) ||
                                      (data[16] == 0xEE && data[17] == 0xFF) ||
                                      (data[16] == 0xFF && data[17] == 0xEE))) {
                data += 16;
                size -= 16;
            }
        }
    }

    // 1. Hesai: EE FF
    if (data[0] == 0xEE && data[1] == 0xFF) {
        result.vendor = LidarVendor::Hesai;
        result.vendor_name = "Hesai";
        result.is_point_cloud = true;

        uint8_t major = data[2];
        uint8_t minor = data[3];

        if (major == 0x01 && minor == 0x04) {
            uint8_t status_ver = (size > 4) ? data[4] : 0;
            if (status_ver >= 137) result.model = "JT128";
            else if (status_ver > 127) result.model = "OT128";
            else result.model = "Pandar128";
        } else if (major == 0x01 && minor == 0x08) {
            result.model = "JT16";
        } else if (major == 0x03 && minor == 0x01) {
            result.model = "PandarQT";
        } else if (major == 0x03 && minor == 0x02) {
            result.model = "PandarQT128";
        } else if (major == 0x04 && (minor == 0x01 || minor == 0x03)) {
            result.model = "AT128";
        } else if (major == 0x04 && minor == 0x07) {
            result.model = "ATX";
        } else if (major == 0x06 && minor == 0x01) {
            result.model = "PandarXT";
        } else if (major == 0x07 && minor == 0x02) {
            result.model = "FT120";
        } else if (major == 0x07 && minor == 0x03) {
            result.model = "FT2";
        } else {
            result.model = "Hesai-Gen(" + std::to_string(major) + "." + std::to_string(minor) + ")";
        }
        return result;
    }

    // Hesai Pandar40 / 40P (1262 or 1266 bytes starting with FF EE)
    if ((size == 1262 || size == 1266) && data[0] == 0xFF && data[1] == 0xEE) {
        result.vendor = LidarVendor::Hesai;
        result.vendor_name = "Hesai";
        result.model = "Pandar40P";
        result.is_point_cloud = true;
        return result;
    }

    // 2. RoboSense: 55 AA 05 5A (mechanical) or 55 AA 5A A5 (MEMS) or A5 FF 00 5A (DIFOP)
    if (data[0] == 0x55 && data[1] == 0xAA &&
        ((data[2] == 0x05 && data[3] == 0x5A) || (data[2] == 0x5A && data[3] == 0xA5))) {
        result.vendor = LidarVendor::RoboSense;
        result.vendor_name = "RoboSense";
        result.is_point_cloud = true;

        if (size == 1210) {
            result.model = "RSM1";
        } else if (size == 1342) {
            result.model = "RSM2";
        } else if (size == 812) {
            result.model = "RSEMX";
        } else if (size == 844) {
            result.model = "RSE2";
        } else if (size == 1200) {
            result.model = "RSE1";
        } else if (size == 1248) {
            // RSAIRY / RSFAIRY use 8 blocks of 148 bytes (FF EE headers at offsets 42, 190, 338).
            if (data[42] == 0xFF && data[43] == 0xEE &&
                data[190] == 0xFF && data[191] == 0xEE &&
                data[338] == 0xFF && data[339] == 0xEE) {
                result.model = "RSAIRY";
            } else if (data[42] == 0xFE && data[430] == 0xFE && data[818] == 0xFE) {
                result.model = "RS128";
            } else {
                result.model = "RSHELIOS";
            }
        } else {
            result.model = "RS-LiDAR";
        }
        return result;
    }

    if (data[0] == 0xA5 && data[1] == 0xFF && data[2] == 0x00 && data[3] == 0x5A) {
        result.vendor = LidarVendor::RoboSense;
        result.vendor_name = "RoboSense";
        result.model = "RoboSense-DIFOP";
        result.is_status_or_difop = true;
        return result;
    }

    // 3. Velodyne: 1206 bytes with FF EE firing blocks
    if (size == 1206 && data[0] == 0xFF && data[1] == 0xEE && data[100] == 0xFF && data[101] == 0xEE) {
        result.vendor = LidarVendor::Velodyne;
        result.vendor_name = "Velodyne";
        result.is_point_cloud = true;

        uint8_t product_id = data[1205];
        if (product_id == 0x21) result.model = "HDL-32E";
        else if (product_id == 0x22) result.model = "VLP-16";
        else if (product_id == 0x24) result.model = "Puck LITE";
        else if (product_id == 0x28) result.model = "VLP-32C";
        else if (product_id == 0x31 || product_id == 0x40) result.model = "HDL-64E S3";
        else if (product_id == 0xA0 || product_id == 0xA1) result.model = "VLS-128";
        else result.model = "VLP-16";
        return result;
    }

    // 4. Seyond / Innovusion: 0x176A (data) or 0x186B (status)
    if (size >= 8) {
        uint16_t magic_le = (data[1] << 8) | data[0];
        uint16_t magic_be = (data[0] << 8) | data[1];

        if (magic_le == 0x176A || magic_be == 0x176A) {
            result.vendor = LidarVendor::Seyond;
            result.vendor_name = "Seyond";
            result.model = "Falcon/Robin";
            result.is_point_cloud = true;
            return result;
        }
        if (magic_le == 0x186B || magic_be == 0x186B) {
            result.vendor = LidarVendor::Seyond;
            result.vendor_name = "Seyond";
            result.model = "Seyond-Status";
            result.is_status_or_difop = true;
            return result;
        }
    }

    // 5. Huawei L306: 1021 bytes with 0xFF 0xDD header block and zero-padded frame markers
    if (size == 1021 &&
        data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x00 &&
        data[23] < 4 && data[53] == 0xFF && data[54] == 0xDD &&
        data[1017] == 0x00 && data[1018] == 0x00 && data[1019] == 0x00 && data[1020] == 0x00) {
        result.vendor = LidarVendor::Huawei;
        result.vendor_name = "Huawei";
        result.model = "L306";
        result.is_point_cloud = true;
        return result;
    }

    return result;
}

} // namespace ulv
