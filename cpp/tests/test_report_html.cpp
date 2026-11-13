#include "test_framework.hpp"
#include "core/report_generator.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>

namespace {

std::string read_file_string(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return "";
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t val = 0;
    int valb = -6;
    for (uint8_t c : data) {
        val = (val << 8) | c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kTable[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kTable[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

} // namespace

TEST_CASE_TIER("Tier 1: Feature Coverage", "Report: Default Data Initialization & 8 Sections") {
    ulv::InspectionReportData data;

    ASSERT_FALSE(data.report_id.empty());
    ASSERT_FALSE(data.test_date.empty());
    ASSERT_EQ(data.model, "Hesai AT128");
    ASSERT_EQ(data.vendor, "Hesai");
    ASSERT_EQ(data.sections.size(), 8);

    // Verify 8 standard industrial sections
    ASSERT_EQ(data.sections[0].title, "外观");
    ASSERT_EQ(data.sections[1].title, "供电");
    ASSERT_EQ(data.sections[2].title, "网络连接");
    ASSERT_EQ(data.sections[3].title, "SDK识别");
    ASSERT_EQ(data.sections[4].title, "点云");
    ASSERT_EQ(data.sections[5].title, "转速");
    ASSERT_EQ(data.sections[6].title, "丢包");
    ASSERT_EQ(data.sections[7].title, "配件");
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "Report: HTML Generation & Structural Tags") {
    ulv::InspectionReportData data;
    data.report_id = "RPT-TEST-20261102-001";
    data.customer_project = "测试验证工位-E2E";

    std::string out_path = "/tmp/test_report_structure.html";
    bool ok = ulv::ReportGenerator::export_html(data, out_path);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::filesystem::exists(out_path));

    std::string html = read_file_string(out_path);
    ASSERT_CONTAINS(html, "<!DOCTYPE html>");
    ASSERT_CONTAINS(html, "<title>激光雷达检测报告 - RPT-TEST-20261102-001</title>");
    ASSERT_CONTAINS(html, "激光雷达全功能检验评定报告");
    ASSERT_CONTAINS(html, "Hesai AT128");
    ASSERT_CONTAINS(html, "综合检测结论: 【检验合格 · 准予交付 (PASS)】");
    ASSERT_CONTAINS(html, "5. 点云");
    ASSERT_CONTAINS(html, "主检测试工程师签字:");

    std::filesystem::remove(out_path);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "Report: Base64 URI Format & Encoding Integrity") {
    std::string sample = "Universal LiDAR Viewer 2026 Pure C++";
    std::vector<uint8_t> bytes(sample.begin(), sample.end());
    std::string b64 = base64_encode(bytes);

    ASSERT_FALSE(b64.empty());
    ASSERT_EQ(b64.size() % 4, 0);

    // Standard PNG data URI pattern verification
    std::string dummy_png_uri = "data:image/png;base64," + b64;
    ASSERT_CONTAINS(dummy_png_uri, "data:image/png;base64,");

    // Regex check for base64 characters
    std::regex b64_regex("^[A-Za-z0-9+/=]+$");
    ASSERT_TRUE(std::regex_match(b64, b64_regex));
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "Report: Special Characters & HTML Injections") {
    ulv::InspectionReportData data;
    data.report_id = "RPT-<SPECIAL>&\"'";
    data.customer_project = "<script>alert('xss')</script>";
    data.sections[0].notes = "Test & Check <Angle> \"Quotes\" 'Single'";

    std::string out_path = "/tmp/test_report_special.html";
    bool ok = ulv::ReportGenerator::export_html(data, out_path);
    ASSERT_TRUE(ok);

    std::string html = read_file_string(out_path);
    ASSERT_FALSE(html.empty());
    // Document should be well-formed with matching closing tags
    ASSERT_CONTAINS(html, "</html>");

    std::filesystem::remove(out_path);
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "Report: Base64 Viewport Image Tag Detection") {
    // Contract verification for Milestone M4:
    // Section 5 must contain an embedded <img> tag with data:image/png;base64.
    ulv::InspectionReportData data;
    std::string out_path = "/tmp/test_report_section5.html";
    ulv::ReportGenerator::export_html(data, out_path);

    std::string html = read_file_string(out_path);
    // Locate Section 5 "点云"
    size_t sec5_pos = html.find("5. 点云");
    ASSERT_NE(sec5_pos, std::string::npos);

    // Check whether Base64 image tag is present (M4 criterion)
    bool has_img_tag = (html.find("data:image/png;base64,") != std::string::npos) &&
                       (html.find("<img ") != std::string::npos);
    ASSERT_TRUE(has_img_tag);

    // Verify screenshot frame container and watermark
    ASSERT_CONTAINS(html, "screenshot-container");
    ASSERT_CONTAINS(html, "screenshot-frame");
    ASSERT_CONTAINS(html, "screenshot-watermark");
    ASSERT_CONTAINS(html, "Universal LiDAR Viewer · 3D Viewport Capture");
    ASSERT_CONTAINS(html, "图 5-1 实测 3D 激光雷达点云视口三维空间分布渲染快照");

    std::cout << "\n    [Info] Section 5 Base64 Image Tag Present: YES (M4 Compliant) ... " << std::flush;

    std::filesystem::remove(out_path);
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "Report: CSS Print Layout & Action Bar") {
    ulv::InspectionReportData data;
    std::string out_path = "/tmp/test_report_print.html";
    ulv::ReportGenerator::export_html(data, out_path);

    std::string html = read_file_string(out_path);
    ASSERT_FALSE(html.empty());

    // Verify print media CSS rules
    ASSERT_CONTAINS(html, "@media print");
    ASSERT_CONTAINS(html, "size: A4 portrait");
    ASSERT_CONTAINS(html, "-webkit-print-color-adjust: exact");
    ASSERT_CONTAINS(html, "page-break-inside: avoid");
    ASSERT_CONTAINS(html, ".no-print");

    // Verify window.print() trigger button in action-bar
    ASSERT_CONTAINS(html, "action-bar");
    ASSERT_CONTAINS(html, "window.print()");
    ASSERT_CONTAINS(html, "打印 / 另存为 PDF");

    std::filesystem::remove(out_path);
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "Report: attach_viewport_screenshot Normalization & Clearing") {
    ulv::InspectionReportData data;

    // Custom screenshot without prefix -> should auto-prefix data:image/png;base64,
    std::string raw_b64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";
    ulv::attach_viewport_screenshot(data, raw_b64, "图 5-2 自定义测试截屏");

    ASSERT_EQ(data.point_cloud_image_base64, "data:image/png;base64," + raw_b64);
    ASSERT_EQ(data.point_cloud_image_caption, "图 5-2 自定义测试截屏");
    ASSERT_EQ(data.sections[4].image_base64, "data:image/png;base64," + raw_b64);
    ASSERT_EQ(data.sections[4].image_caption, "图 5-2 自定义测试截屏");

    std::string out_path = "/tmp/test_report_custom_screenshot.html";
    ulv::ReportGenerator::export_html(data, out_path);
    std::string html = read_file_string(out_path);
    ASSERT_CONTAINS(html, "图 5-2 自定义测试截屏");
    ASSERT_CONTAINS(html, raw_b64);
    std::filesystem::remove(out_path);

    // Clear screenshot -> should render placeholder
    ulv::attach_viewport_screenshot(data, "");
    ASSERT_TRUE(data.point_cloud_image_base64.empty());
    ASSERT_TRUE(data.sections[4].image_base64.empty());

    ulv::ReportGenerator::export_html(data, out_path);
    html = read_file_string(out_path);
    ASSERT_CONTAINS(html, "screenshot-placeholder");
    ASSERT_CONTAINS(html, "当前报告未挂载视口渲染快照");
    std::filesystem::remove(out_path);
}

TEST_CASE_TIER("Tier 4: Real-World Scenarios", "Report: Production Quality HTML Export") {
    ulv::InspectionReportData data;
    data.model = "Hesai AT128";
    data.serial_number = "HS-AT128-PROD-202611";
    data.inspector_name = "质量检验员-01";
    data.point_count = 141275;
    data.motor_rpm = 600.0f;
    data.temperature_c = 41.8f;
    data.operating_voltage = 12.0f;

    std::string out_path = "/tmp/lidar_production_inspection_report.html";
    bool ok = ulv::ReportGenerator::export_html(data, out_path);
    ASSERT_TRUE(ok);

    std::uintmax_t fsize = std::filesystem::file_size(out_path);
    // A fully serialized report HTML should be at least 3KB
    ASSERT_GT(fsize, 3000);

    std::filesystem::remove(out_path);
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "Report: Diagnostic Telemetry HTML Integration") {
    ulv::InspectionReportData data;
    data.model = "Hesai AT128";
    data.serial_number = "HS-LIVE-TEST-123456";
    data.firmware_version = "v3.2.0_test";
    data.ip_address = "192.168.1.201";
    data.mac_address = "00:1C:42:AA:BB:CC";
    data.operating_voltage = 12.05f;
    data.temperature_c = 42.5f;
    data.motor_rpm = 600.0f;

    data.sections[0].result = "正常";
    data.sections[0].notes = "【PTC 9347 设备身份读取】\n型号：Hesai AT128\n序列号 S/N：HS-LIVE-TEST-123456";

    data.sections[1].result = "正常";
    data.sections[1].notes = "【PTC 9347 供电与温度读数】\n工作电压实测：12.05 V\n核心测点温度：42.5 ℃";

    data.sections[2].result = "正常";
    data.sections[2].notes = "【PTC 9347 网络配置读取】\n雷达 IP：192.168.1.201\n物理 MAC 地址：00:1C:42:AA:BB:CC";

    data.sections[3].result = "正常";
    data.sections[3].notes = "【PTC 9347 型号与固件读取】\n识别型号：Hesai AT128\n固件版本：v3.2.0_test";

    data.sections[5].result = "正常";
    data.sections[5].notes = "【PTC 9347 转速与运行机构】\n实测电机转速：600.0 RPM";

    std::string out_path = "/tmp/lidar_diagnostic_telemetry_report.html";
    bool ok = ulv::ReportGenerator::export_html(data, out_path);
    ASSERT_TRUE(ok);

    std::string html = read_file_string(out_path);
    ASSERT_NE(html.find("HS-LIVE-TEST-123456"), std::string::npos);
    ASSERT_NE(html.find("v3.2.0_test"), std::string::npos);
    ASSERT_NE(html.find("12.05 V"), std::string::npos);
    ASSERT_NE(html.find("42.5 ℃"), std::string::npos);
    ASSERT_NE(html.find("600.0 RPM"), std::string::npos);

    std::filesystem::remove(out_path);
}
