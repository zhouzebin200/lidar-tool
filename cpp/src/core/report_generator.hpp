#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace ulv {

struct ReportSection {
    std::string title;
    std::string hint;
    std::string result = "合格"; // "正常", "合格", "不合格", "未检测", "不适用"
    std::string notes;
    std::string image_base64;   // Optional section-specific Base64 image
    std::string image_caption;  // Optional custom caption for the image
};

struct InspectionReportData {
    // 6 大基本信息 (fields)
    std::string report_id;
    std::string model = "Hesai AT128";
    std::string serial_number = "HS-AT128-20261102";
    std::string customer_project = "砺见测试工位";
    std::string inspector_name = "测试工程师";
    std::string test_date;

    // 设备网络与运行参数快照
    std::string vendor = "Hesai";
    std::string firmware_version = "v3.1.2_prod";
    std::string ip_address = "192.168.1.201";
    std::string mac_address = "00:1C:42:2B:06:FC";
    float temperature_c = 42.5f;
    float motor_rpm = 600.0f;
    float operating_voltage = 12.0f;
    size_t point_count = 141275;
    float max_range_m = 138.2f;
    float frame_rate_hz = 10.0f;

    // 8 大检测项 (sections)
    std::vector<ReportSection> sections;

    // 3D 点云视口截屏 (Data URI 或 Base64 编码 PNG 字符串)
    std::string point_cloud_image_base64;
    std::string point_cloud_image_caption;

    // 综合判定与意见
    bool overall_passed = true;
    std::string summary;

    InspectionReportData();
};

/**
 * @brief Attaches a 3D viewport screenshot PNG to an InspectionReportData structure.
 *        Automatically normalizes the Data URI prefix and synchronizes Section 5 ("点云").
 * @param report Target report data.
 * @param base64_png Raw Base64 string or "data:image/png;base64,..." URI.
 * @param caption Optional caption for the screenshot.
 */
void attach_viewport_screenshot(InspectionReportData& report,
                                const std::string& base64_png,
                                const std::string& caption = "");

/**
 * @brief Reads an image file from disk and encodes it as a Base64 Data URI (e.g. data:image/jpeg;base64,...).
 * @param filepath Path to the image file on disk.
 * @param out_data_uri Output Data URI string.
 * @param out_error Output error message on failure.
 * @return True if read and encoded successfully, false otherwise.
 */
bool load_image_file_as_data_uri(const std::string& filepath, std::string& out_data_uri, std::string& out_error);

/**
 * @brief Attaches a physical appearance photo to an InspectionReportData structure (Section 1 "外观").
 * @param report Target report data.
 * @param base64_image Raw Base64 string or "data:image/...;base64,..." URI.
 * @param caption Optional caption for the photo.
 */
void attach_appearance_photo(InspectionReportData& report,
                             const std::string& base64_image,
                             const std::string& caption = "");

class ReportGenerator {
public:
    static bool export_pdf(const InspectionReportData& data, const std::string& out_path);
    static bool export_html(const InspectionReportData& data, const std::string& out_path);

    static void attach_viewport_screenshot(InspectionReportData& report,
                                           const std::string& base64_png,
                                           const std::string& caption = "") {
        ulv::attach_viewport_screenshot(report, base64_png, caption);
    }

    static void attach_appearance_photo(InspectionReportData& report,
                                        const std::string& base64_image,
                                        const std::string& caption = "") {
        ulv::attach_appearance_photo(report, base64_image, caption);
    }
};

} // namespace ulv
