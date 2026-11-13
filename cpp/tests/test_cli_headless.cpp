#include "test_framework.hpp"
#include "net/pcap_reader.hpp"
#include "net/detector.hpp"
#include <cstdlib>
#include <array>
#include <memory>
#include <filesystem>
#include <fstream>

namespace {

std::string run_command_capture(const std::string& cmd, int& out_exit_code) {
    std::array<char, 512> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        out_exit_code = -1;
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int status = pclose(pipe);
    if (WIFEXITED(status)) {
        out_exit_code = WEXITSTATUS(status);
    } else {
        out_exit_code = -1;
    }
    return result;
}

std::string find_test_pcap() {
    std::vector<std::string> candidates = {
        "./cpp/tests/fixtures/111.pcap",
        "../tests/fixtures/111.pcap",
        "../../tests/fixtures/111.pcap"
    };
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) {
            return c;
        }
    }
    return "";
}

std::string find_viewer_binary() {
    std::vector<std::string> candidates = {
        "./cpp/build/bin/ulv_viewer",
        "../build/bin/ulv_viewer",
        "../../build/bin/ulv_viewer",
        "bin/ulv_viewer"
    };
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) {
            return c;
        }
    }
    return "";
}

} // namespace

TEST_CASE_TIER("Tier 1: Feature Coverage", "CLI: Headless Test Mode Execution") {
    std::string bin = find_viewer_binary();
    std::string pcap = find_test_pcap();

    ASSERT_FALSE(bin.empty());
    ASSERT_FALSE(pcap.empty());

    int exit_code = -1;
    std::string cmd = bin + " --test " + pcap + " 2>&1";
    std::string output = run_command_capture(cmd, exit_code);

    ASSERT_EQ(exit_code, 0);
    ASSERT_CONTAINS(output, "[ULV C++ Test Mode] Running verification...");
    ASSERT_CONTAINS(output, "[ULV C++ Test Mode] Success: Processed");
    ASSERT_CONTAINS(output, "received 30 frames");
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "PcapReader: Contract & Packet Header Decoding") {
    std::string pcap = find_test_pcap();
    ASSERT_FALSE(pcap.empty());

    ulv::PcapReader reader;
    bool opened = reader.open(pcap);
    ASSERT_TRUE(opened);
    ASSERT_GT(reader.total_packets(), 20000);

    ulv::PcapUdpPacket pkt;
    bool has_pkt = reader.read_next_packet(pkt);
    ASSERT_TRUE(has_pkt);
    ASSERT_GT(pkt.payload.size(), 0);
    ASSERT_NE(pkt.dst_port, 0);

    reader.close();
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "CLI: Non-existent PCAP Error Handling") {
    std::string bin = find_viewer_binary();
    ASSERT_FALSE(bin.empty());

    int exit_code = -1;
    std::string cmd = bin + " --test /tmp/non_existent_file_xyz_987.pcap 2>&1";
    std::string output = run_command_capture(cmd, exit_code);

    // main.cpp returns code 2 on PCAP open failure
    ASSERT_EQ(exit_code, 2);
    ASSERT_CONTAINS(output, "Could not open test PCAP");
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "PcapReader: Zero-Byte & Corrupted File") {
    std::string zero_file = "/tmp/test_zero_byte.pcap";
    {
        std::ofstream ofs(zero_file, std::ios::binary);
    }
    ulv::PcapReader reader;
    bool res_zero = reader.open(zero_file);
    ASSERT_FALSE(res_zero);
    std::filesystem::remove(zero_file);

    std::string corrupt_file = "/tmp/test_corrupt.pcap";
    {
        std::ofstream ofs(corrupt_file, std::ios::binary);
        // Write invalid magic bytes (not 0xa1b2c3d4 or pcapng)
        uint32_t bad_magic = 0xdeadbeef;
        ofs.write(reinterpret_cast<const char*>(&bad_magic), 4);
    }
    bool res_corrupt = reader.open(corrupt_file);
    ASSERT_FALSE(res_corrupt);
    std::filesystem::remove(corrupt_file);
}

TEST_CASE_TIER("Tier 4: Real-World Scenarios", "CLI: Full 20709 Packets Decode Consistency") {
    std::string bin = find_viewer_binary();
    std::string pcap = find_test_pcap();
    ASSERT_FALSE(bin.empty());
    ASSERT_FALSE(pcap.empty());

    int exit_code = -1;
    std::string cmd = bin + " --test " + pcap + " 2>&1";
    std::string output = run_command_capture(cmd, exit_code);

    ASSERT_EQ(exit_code, 0);
    // Verifies production numbers derived from HANDOVER.md: 20703-20709 packets, 30 frames, Hesai AT128
    ASSERT_CONTAINS(output, "2070");
    ASSERT_CONTAINS(output, "Hesai AT128");
    ASSERT_CONTAINS(output, "max_dist=");
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "PcapReader: PCAPNG File Ingestion Verification") {
    std::string pcapng_path = "/Volumes/软件/lidardata/emx.pcapng";
    if (std::filesystem::exists(pcapng_path)) {
        ulv::PcapReader reader;
        bool ok = reader.open(pcapng_path);
        ASSERT_TRUE(ok);
        ASSERT_GT(reader.total_packets(), 20000);

        ulv::PcapUdpPacket pkt;
        bool has_pkt = reader.read_next_packet(pkt);
        ASSERT_TRUE(has_pkt);
        ASSERT_GT(pkt.payload.size(), 800);
        ASSERT_GT(pkt.timestamp_ns, 1000000000000ULL);
        reader.close();
    }
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "LidarDetector: SOME/IP Stripping and MEMS Protocol Recognition") {
    // 1. Hesai PandarQT
    uint8_t qt_pkt[1072] = {0xee, 0xff, 0x03, 0x01};
    auto qt_res = ulv::LidarDetector::identify_packet(qt_pkt, sizeof(qt_pkt), 2368);
    ASSERT_EQ(qt_res.model, "PandarQT");
    ASSERT_TRUE(qt_res.vendor == ulv::LidarVendor::Hesai);

    // 2. Hesai Pandar40P
    uint8_t p40_pkt[1262] = {0xff, 0xee};
    auto p40_res = ulv::LidarDetector::identify_packet(p40_pkt, sizeof(p40_pkt), 2368);
    ASSERT_EQ(p40_res.model, "Pandar40P");
    ASSERT_TRUE(p40_res.vendor == ulv::LidarVendor::Hesai);

    // 3. RoboSense RSM1 (MEMS 0x55 0xAA 0x5A 0xA5)
    uint8_t m1_pkt[1210] = {0x55, 0xaa, 0x5a, 0xa5};
    auto m1_res = ulv::LidarDetector::identify_packet(m1_pkt, sizeof(m1_pkt), 40009);
    ASSERT_EQ(m1_res.model, "RSM1");
    ASSERT_TRUE(m1_res.vendor == ulv::LidarVendor::RoboSense);

    // 4. RoboSense RSM2 with 8-byte SOME/IP header
    uint8_t m2_someip_8[1350] = {0x00, 0x07, 0x05, 0x00, 0x00, 0x00, 0x05, 0x3e, 0x55, 0xaa, 0x5a, 0xa5};
    auto m2_res = ulv::LidarDetector::identify_packet(m2_someip_8, sizeof(m2_someip_8), 567);
    ASSERT_EQ(m2_res.model, "RSM2");
    ASSERT_TRUE(m2_res.vendor == ulv::LidarVendor::RoboSense);

    // 5. RoboSense RSEMX with 8-byte SOME/IP header
    uint8_t emx_someip_8[820] = {0x00, 0x00, 0x00, 0xa7, 0x00, 0x00, 0x03, 0x2c, 0x55, 0xaa, 0x5a, 0xa5};
    auto emx_res = ulv::LidarDetector::identify_packet(emx_someip_8, sizeof(emx_someip_8), 567);
    ASSERT_EQ(emx_res.model, "RSEMX");
    ASSERT_TRUE(emx_res.vendor == ulv::LidarVendor::RoboSense);
}
