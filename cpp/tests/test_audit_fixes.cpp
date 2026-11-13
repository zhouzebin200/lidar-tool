#include "test_framework.hpp"
#include "core/ptp_monitor.hpp"
#include "core/decoder_manager.hpp"
#include "core/report_generator.hpp"
#include "core/frame_buffer.hpp"
#include "net/detector.hpp"
#include "net/udp_receiver.hpp"
#include <array>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace {

std::string find_dir(const char* name) {
    for (const char* prefix : {"./", "../", "../../", "../../../"}) {
        std::filesystem::path p = std::filesystem::path(prefix) / name;
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
            return p.u8string();
        }
    }
    return "";
}

std::vector<uint8_t> make_ptp_packet(uint8_t msg_type, uint8_t domain = 0) {
    std::vector<uint8_t> pkt(56, 0);
    pkt[0] = msg_type & 0x0F;
    pkt[1] = 0x02; // PTPv2
    pkt[4] = domain;
    // Grandmaster identity bytes 44..51 for Announce messages
    for (int i = 0; i < 8; ++i) pkt[44 + i] = static_cast<uint8_t>(0x10 + i);
    return pkt;
}

} // namespace

TEST_CASE_TIER("Tier 1: Audit Regression", "PtpMonitor: Sync gating and FreeRun timeout") {
    auto announce = make_ptp_packet(0x0B, 24);
    auto sync_pkt = make_ptp_packet(0x00, 24);

    // 1. Announce alone before any Sync must NOT report Locked.
    ulv::PtpMonitor mon1;
    mon1.parse_ptp_packet(announce.data(), announce.size(), 320, 100.0);
    auto st = mon1.status(100.0);
    ASSERT_TRUE(st.state == ulv::PtpState::FreeRun);
    ASSERT_FALSE(mon1.is_synced());
    ASSERT_EQ(st.announce_packets_received, 1ULL);
    ASSERT_EQ(st.sync_packets_received, 0ULL);

    // Once Sync also arrives, Announce + Sync becomes Locked.
    mon1.parse_ptp_packet(sync_pkt.data(), sync_pkt.size(), 319, 100.5);
    st = mon1.status(100.5);
    ASSERT_TRUE(st.state == ulv::PtpState::Locked);
    ASSERT_TRUE(mon1.is_synced());

    // 2. Sync first enters Tracking, then Announce transitions to Locked, then 3s timeout -> FreeRun.
    ulv::PtpMonitor mon2;
    mon2.parse_ptp_packet(sync_pkt.data(), sync_pkt.size(), 319, 200.0);
    st = mon2.status(200.0);
    ASSERT_TRUE(st.state == ulv::PtpState::Tracking);
    ASSERT_FALSE(mon2.is_synced());

    mon2.parse_ptp_packet(announce.data(), announce.size(), 320, 200.5);
    st = mon2.status(200.5);
    ASSERT_TRUE(st.state == ulv::PtpState::Locked);
    ASSERT_TRUE(mon2.is_synced());

    // No packets for > 3.0 seconds must decay state to FreeRun via status(now).
    st = mon2.status(203.6);
    ASSERT_TRUE(st.state == ulv::PtpState::FreeRun);
    ASSERT_FALSE(mon2.is_synced());
}

TEST_CASE_TIER("Tier 1: Audit Regression", "LidarDetector: L306 framing check rejects arbitrary 1021B packets") {
    std::vector<uint8_t> noise(1021, 0xAA);
    auto d_noise = ulv::LidarDetector::identify_packet(noise.data(), noise.size(), 2368);
    ASSERT_TRUE(d_noise.vendor == ulv::LidarVendor::Unknown);
    ASSERT_FALSE(d_noise.is_point_cloud);

    std::vector<uint8_t> l306(1021, 0x00);
    l306[23] = 2;     // channel_type < 4
    l306[53] = 0xFF;  // first block magic
    l306[54] = 0xDD;
    auto d_valid = ulv::LidarDetector::identify_packet(l306.data(), l306.size(), 2368);
    ASSERT_TRUE(d_valid.vendor == ulv::LidarVendor::Huawei);
    ASSERT_EQ(d_valid.model, "L306");
    ASSERT_TRUE(d_valid.is_point_cloud);

    // RoboSense 1248B packet with 148-byte block stride must identify as RSAIRY rather than RSHELIOS.
    std::vector<uint8_t> airy(1248, 0x00);
    airy[0] = 0x55; airy[1] = 0xAA; airy[2] = 0x05; airy[3] = 0x5A;
    airy[42] = 0xFF; airy[43] = 0xEE;
    airy[190] = 0xFF; airy[191] = 0xEE;
    airy[338] = 0xFF; airy[339] = 0xEE;
    auto d_airy = ulv::LidarDetector::identify_packet(airy.data(), airy.size(), 6699);
    ASSERT_TRUE(d_airy.vendor == ulv::LidarVendor::RoboSense);
    ASSERT_EQ(d_airy.model, "RSAIRY");
}

TEST_CASE_TIER("Tier 1: Audit Regression", "DecoderManager: Loads plugins cleanly and ignores 1021B noise") {
    std::string p_dir = find_dir("plugins");
    std::string c_dir = find_dir("corrections");
    ASSERT_FALSE(p_dir.empty());

    ulv::FrameBufferQueue q;
    ulv::DecoderManager mgr(q);
    ASSERT_TRUE(mgr.load_plugins(p_dir, c_dir));

    std::vector<uint8_t> noise(1021, 0x55);
    mgr.feed_packet(noise.data(), noise.size(), "192.168.1.201", 50000, 2368, 1000);
    ASSERT_FALSE(mgr.has_active_decoder());
}

TEST_CASE_TIER("Tier 1: Audit Regression", "ReportGenerator: Escapes HTML special characters") {
    ulv::InspectionReportData d;
    d.report_id = "RPT-<1>&\"'";
    d.customer_project = "<script>alert(1)</script>";
    d.summary = "Line & <tag> \"quote\"";
    d.sections[0].notes = "<img src=x onerror=alert(1)>";

    std::filesystem::path out = std::filesystem::temp_directory_path() / "ulv_audit_escape_test.html";
    std::error_code ec;
    std::filesystem::remove(out, ec);

    ASSERT_TRUE(ulv::ReportGenerator::export_html(d, out.u8string()));

    std::ifstream in(out);
    ASSERT_TRUE(in.is_open());
    std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(out, ec);

    ASSERT_TRUE(html.find("<script>alert(1)</script>") == std::string::npos);
    ASSERT_TRUE(html.find("onerror=alert(1)>") == std::string::npos);
    ASSERT_CONTAINS(html, "&lt;script&gt;alert(1)&lt;/script&gt;");
    ASSERT_CONTAINS(html, "RPT-&lt;1&gt;&amp;&quot;&#39;");
}

TEST_CASE_TIER("Tier 1: Audit Regression", "UdpReceiver: Reports bind failure instead of silent true") {
    ulv::UdpReceiver rx;
    // RFC 5737 TEST-NET-1 non-local address cannot be bound on host interfaces.
    bool ok = rx.start("192.0.2.123", {19599},
        [](const uint8_t*, size_t, const std::string&, uint16_t, uint16_t, uint64_t) {});
    ASSERT_FALSE(ok);
    rx.stop();
}
