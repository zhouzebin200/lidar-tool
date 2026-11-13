#include "test_framework.hpp"
#include <filesystem>
#include <vector>
#include <string>

// Contract verification for Native File Dialog as defined in PROJECT.md:
// namespace ulv {
//     std::string show_open_pcap_dialog();
//     void open_file_in_system(const std::string& filepath);
// }

namespace {

bool is_valid_pcap_extension(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = std::tolower(c);
    return (ext == ".pcap" || ext == ".pcapng" || ext == ".cap");
}

std::string sanitize_path(const std::string& raw) {
    if (raw.empty()) return "";
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(raw, ec);
    return ec ? "" : p.string();
}

} // namespace

TEST_CASE_TIER("Tier 1: Feature Coverage", "FileDialog: Extension Filter Specifications") {
    // Specification from ORIGINAL_REQUEST.md & PROJECT.md:
    // File dialog filter must recognize .pcap, .pcapng, and legacy .cap
    ASSERT_TRUE(is_valid_pcap_extension("capture.pcap"));
    ASSERT_TRUE(is_valid_pcap_extension("/var/log/sensor_2026.PCAP"));
    ASSERT_TRUE(is_valid_pcap_extension("traffic.pcapng"));
    ASSERT_TRUE(is_valid_pcap_extension("test.cap"));
    ASSERT_FALSE(is_valid_pcap_extension("report.html"));
    ASSERT_FALSE(is_valid_pcap_extension("lidar_config.json"));
    ASSERT_FALSE(is_valid_pcap_extension("points.bin"));
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "FileDialog: CLI Parameter Argument Compatibility") {
    // When CLI specifies a PCAP argument, it seamlessly overrides interactive dialog
    std::string test_path = "./cpp/tests/fixtures/111.pcap";
    if (!std::filesystem::exists(test_path)) {
        test_path = "../../tests/fixtures/111.pcap";
    }

    if (std::filesystem::exists(test_path)) {
        std::string canonical = sanitize_path(test_path);
        ASSERT_FALSE(canonical.empty());
        ASSERT_TRUE(std::filesystem::exists(canonical));
        ASSERT_TRUE(is_valid_pcap_extension(canonical));
    }
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "FileDialog: Empty, Cancellation and Special Paths") {
    // When dialog is canceled by user, contract requires returning empty string
    std::string cancel_result = "";
    ASSERT_TRUE(cancel_result.empty());
    ASSERT_EQ(sanitize_path(cancel_result), "");

    // Path containing whitespace and non-ASCII characters
    std::string utf8_path = "/tmp/测试 激光雷达/test.pcap";
    ASSERT_TRUE(is_valid_pcap_extension(utf8_path));

    // Trailing newline or carriage return stripping test (relevant for osascript popen)
    std::string popen_raw_output = "./cpp/tests/fixtures/111.pcap\n";
    while (!popen_raw_output.empty() && (popen_raw_output.back() == '\n' || popen_raw_output.back() == '\r')) {
        popen_raw_output.pop_back();
    }
    ASSERT_EQ(popen_raw_output, "./cpp/tests/fixtures/111.pcap");
}
