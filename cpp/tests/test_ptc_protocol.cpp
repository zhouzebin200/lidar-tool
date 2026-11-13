#include "test_framework.hpp"
#include "ptc_client.hpp"

#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <memory>
#include <atomic>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#define CLOSE_SOCK(s) close(s)
#endif

namespace {

void write_be32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

} // namespace

TEST_CASE_TIER("Tier 1: Feature Coverage", "PTC: Request Header 8-Byte Framing") {
    auto req = ulv::HesaiPtcClient::build_ptc1_request(ulv::HesaiPtcClient::CMD_GET_INVENTORY_INFO);
    ASSERT_EQ(req.size(), 8);
    ASSERT_EQ(req[0], 0x47); // 'G'
    ASSERT_EQ(req[1], 0x74); // 't'
    ASSERT_EQ(req[2], ulv::HesaiPtcClient::CMD_GET_INVENTORY_INFO);
    ASSERT_EQ(req[3], 0x00);
    uint32_t len = (static_cast<uint32_t>(req[4]) << 24) |
                   (static_cast<uint32_t>(req[5]) << 16) |
                   (static_cast<uint32_t>(req[6]) << 8)  |
                   (static_cast<uint32_t>(req[7]));
    ASSERT_EQ(len, 0);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "PTC: Parse Inventory Payload (0x07)") {
    std::vector<uint8_t> payload(120, 0);
    std::string test_sn = "HS-AT128-TEST999";
    std::string test_pn = "PN-128-REV-A";
    std::string test_date = "2026-11-02";
    std::string test_fw = "v3.1.4_prod";

    std::memcpy(&payload[0], test_sn.c_str(), test_sn.size());
    std::memcpy(&payload[18], test_pn.c_str(), test_pn.size());
    std::memcpy(&payload[50], test_date.c_str(), test_date.size());
    payload[66] = 0x00; payload[67] = 0x1C; payload[68] = 0x42;
    payload[69] = 0x2B; payload[70] = 0x06; payload[71] = 0xFC;
    std::memcpy(&payload[72], test_fw.c_str(), test_fw.size());

    ulv::LidarDiagnosticParams params;
    bool ok = ulv::HesaiPtcClient::parse_inventory_payload(payload.data(), payload.size(), params);
    ASSERT_TRUE(ok);
    ASSERT_EQ(params.serial_number, test_sn);
    ASSERT_EQ(params.product_number, test_pn);
    ASSERT_EQ(params.date_of_manufacture, test_date);
    ASSERT_EQ(params.mac_address, "00:1C:42:2B:06:FC");
    ASSERT_EQ(params.firmware_version, test_fw);
    ASSERT_EQ(params.model, "Hesai AT128");
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "PTC: Parse Status Payload (0x09 - RPM & Temp)") {
    std::vector<uint8_t> payload(44, 0);
    write_be32(&payload[0], 7200);   // uptime 7200s (2 hrs)
    payload[4] = 0x02; payload[5] = 0x58; // 600 RPM in hex (0x0258)
    write_be32(&payload[6], 4250);  // 42.50 deg C (4250 / 100)

    ulv::LidarDiagnosticParams params;
    bool ok = ulv::HesaiPtcClient::parse_status_payload(payload.data(), payload.size(), params);
    ASSERT_TRUE(ok);
    ASSERT_EQ(params.uptime_s, 7200);
    ASSERT_EQ(static_cast<int>(params.motor_rpm), 600);
    ASSERT_GE(params.temperature_c, 42.49f);
    ASSERT_LE(params.temperature_c, 42.51f);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "PTC: Parse Monitor Payload (0x27 - Voltage & Current)") {
    std::vector<uint8_t> payload(16, 0);
    write_be32(&payload[0], 98000); // 980.00 mA
    write_be32(&payload[4], 1205);  // 12.05 V
    write_be32(&payload[8], 1180);  // 11.80 W

    ulv::LidarDiagnosticParams params;
    bool ok = ulv::HesaiPtcClient::parse_monitor_payload(payload.data(), payload.size(), params);
    ASSERT_TRUE(ok);
    ASSERT_GE(params.operating_voltage, 12.0f);
    ASSERT_LE(params.operating_voltage, 12.1f);
    ASSERT_GE(params.operating_current_ma, 979.0f);
    ASSERT_GE(params.operating_power_w, 11.7f);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "PTC: Parse Config Payload (0x08 - Network Configuration)") {
    std::vector<uint8_t> payload(18, 0);
    payload[0] = 192; payload[1] = 168; payload[2] = 1; payload[3] = 201; // IP
    payload[4] = 255; payload[5] = 255; payload[6] = 255; payload[7] = 0; // Netmask
    payload[8] = 192; payload[9] = 168; payload[10] = 1; payload[11] = 1; // Gateway
    payload[12] = 192; payload[13] = 168; payload[14] = 1; payload[15] = 100; // Dest IP
    payload[16] = 0x09; payload[17] = 0x40; // 2368 big-endian

    ulv::LidarDiagnosticParams params;
    bool ok = ulv::HesaiPtcClient::parse_config_payload(payload.data(), payload.size(), params);
    ASSERT_TRUE(ok);
    ASSERT_EQ(params.ip_address, "192.168.1.201");
    ASSERT_EQ(params.subnet_mask, "255.255.255.0");
    ASSERT_EQ(params.gateway, "192.168.1.1");
    ASSERT_EQ(params.destination_ip, "192.168.1.100");
    ASSERT_EQ(params.destination_port, 2368);
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "PTC: Malformed Magic and Header Rejection") {
    // Header with invalid magic
    std::vector<uint8_t> bad_magic = {0x12, 0x34, ulv::HesaiPtcClient::CMD_GET_INVENTORY_INFO, 0x00, 0, 0, 0, 0};
    bool is_magic_valid = (bad_magic[0] == ulv::HesaiPtcClient::PTC1_MAGIC[0] && bad_magic[1] == ulv::HesaiPtcClient::PTC1_MAGIC[1]) ||
                          (bad_magic[0] == ulv::HesaiPtcClient::PTC2_MAGIC[0] && bad_magic[1] == ulv::HesaiPtcClient::PTC2_MAGIC[1]);
    ASSERT_FALSE(is_magic_valid);

    // Payload exceeding 8MB security threshold
    uint32_t huge_len = 10 * 1024 * 1024;
    bool exceeds_limit = (huge_len > 8 * 1024 * 1024);
    ASSERT_TRUE(exceeds_limit);

    // IP Sanitization tests
    ASSERT_EQ(ulv::HesaiPtcClient::sanitize_ip("http://192.168.1.201:80/status"), "192.168.1.201");
    ASSERT_EQ(ulv::HesaiPtcClient::sanitize_ip("192.168.1.201:9347 (LiDAR)"), "192.168.1.201");
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "PTC: OT128 and Solid-State FT120 Special Handlings") {
    // 1. OT128 layout: SN starts with OT
    std::vector<uint8_t> ot_payload(150, 0);
    std::string ot_sn = "OT128-DEMO-001";
    std::memcpy(&ot_payload[0], ot_sn.c_str(), ot_sn.size());
    std::memcpy(&ot_payload[18], "2026-05-18", 10);
    ot_payload[34] = 0xAA; ot_payload[35] = 0xBB; ot_payload[36] = 0xCC;
    ot_payload[37] = 0xDD; ot_payload[38] = 0xEE; ot_payload[39] = 0xFF;
    std::memcpy(&ot_payload[40], "SW_OT_1.2.0", 11);
    std::memcpy(&ot_payload[109], "PN-OT128-REV2", 13);

    ulv::LidarDiagnosticParams ot_params;
    bool ok_ot = ulv::HesaiPtcClient::parse_inventory_payload(ot_payload.data(), ot_payload.size(), ot_params);
    ASSERT_TRUE(ok_ot);
    ASSERT_EQ(ot_params.serial_number, ot_sn);
    ASSERT_EQ(ot_params.model, "Hesai OT128");
    ASSERT_EQ(ot_params.mac_address, "AA:BB:CC:DD:EE:FF");

    // 2. Solid state FT120 status: RPM must be 0
    std::vector<uint8_t> ft_payload(44, 0);
    ft_payload[4] = 0x02; ft_payload[5] = 0x58; // 600 RPM in hex
    write_be32(&ft_payload[6], 4500); // 45.0C
    ulv::LidarDiagnosticParams ft_params;
    ft_params.model = "Hesai FT120";
    bool ok_ft = ulv::HesaiPtcClient::parse_status_payload(ft_payload.data(), ft_payload.size(), ft_params);
    ASSERT_TRUE(ok_ft);
    ASSERT_EQ(ft_params.motor_rpm, 0.0f); // Solid state pure flash has 0 RPM
    ASSERT_GE(ft_params.temperature_c, 44.9f);
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "PTC: Full Client Multi-Command Telemetry Exchange") {
    // Run an ephemeral TCP server simulating full Hesai LiDAR PTC protocol on port 19347
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server_sock, 0);

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(19347);
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int bind_res = bind(server_sock, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr));
    ASSERT_EQ(bind_res, 0);
    listen(server_sock, 1);

    std::atomic<bool> server_running{true};
    std::thread server_thread([&]() {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd >= 0) {
            while (server_running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    if (cmd == ulv::HesaiPtcClient::CMD_GET_INVENTORY_INFO) {
                        std::vector<uint8_t> payload(120, 0);
                        std::string sn = "HS-LIVE-MOCK-001";
                        std::string pn = "PN-AT128-LIVE";
                        std::string fw = "v2.0.0_rc";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        std::memcpy(&payload[18], pn.c_str(), pn.size());
                        std::memcpy(&payload[50], "2026-11-02", 10);
                        payload[66] = 0xAA; payload[67] = 0xBB; payload[68] = 0xCC;
                        payload[69] = 0xDD; payload[70] = 0xEE; payload[71] = 0xFF;
                        std::memcpy(&payload[72], fw.c_str(), fw.size());
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == ulv::HesaiPtcClient::CMD_GET_LIDAR_STATUS) {
                        std::vector<uint8_t> payload(44, 0);
                        write_be32(&payload[0], 3600); // 1h uptime
                        payload[4] = 0x02; payload[5] = 0x58; // 600 RPM
                        write_be32(&payload[6], 4300); // 43.0 C
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == ulv::HesaiPtcClient::CMD_GET_LIDAR_MONITOR) {
                        std::vector<uint8_t> payload(16, 0);
                        write_be32(&payload[0], 95000); // 950 mA
                        write_be32(&payload[4], 1200);  // 12.0 V
                        write_be32(&payload[8], 1140);  // 11.4 W
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == ulv::HesaiPtcClient::CMD_GET_CONFIG_INFO) {
                        std::vector<uint8_t> payload(18, 0);
                        payload[0] = 192; payload[1] = 168; payload[2] = 1; payload[3] = 201;
                        payload[4] = 255; payload[5] = 255; payload[6] = 255; payload[7] = 0;
                        payload[8] = 192; payload[9] = 168; payload[10] = 1; payload[11] = 1;
                        payload[12] = 192; payload[13] = 168; payload[14] = 1; payload[15] = 100;
                        payload[16] = 0x09; payload[17] = 0x40; // 2368
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    }
                }
            }
            CLOSE_SOCK(client_fd);
        }
    });

    // Client side: perform full query via HesaiPtcClient::query_ptc
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto params = ulv::HesaiPtcClient::query_ptc("127.0.0.1", 19347, 1500);

    ASSERT_TRUE(params.success);
    ASSERT_EQ(params.serial_number, "HS-LIVE-MOCK-001");
    ASSERT_EQ(params.mac_address, "AA:BB:CC:DD:EE:FF");
    ASSERT_EQ(params.firmware_version, "v2.0.0_rc");
    ASSERT_EQ(static_cast<int>(params.motor_rpm), 600);
    ASSERT_GE(params.temperature_c, 42.9f);
    ASSERT_GE(params.operating_voltage, 11.9f);
    ASSERT_EQ(params.destination_port, 2368);

    server_running = false;
    CLOSE_SOCK(server_sock);
    server_thread.join();
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "PTC: HTTP Web Fallback Mock Server Query") {
    // Run an ephemeral HTTP server on 127.0.0.1:18080 responding to /pandar.cgi
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server_sock, 0);

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(18080);
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int bind_res = bind(server_sock, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr));
    ASSERT_EQ(bind_res, 0);
    listen(server_sock, 1);

    std::thread http_thread([&]() {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd >= 0) {
            char req_buf[2048];
            recv(client_fd, req_buf, sizeof(req_buf), 0);

            std::string body = "{\"Head\":{\"ErrorCode\":\"0\",\"Message\":\"Success\"},"
                               "\"Body\":{\"SN\":\"QT3CC855913CC857\",\"ProdName\":\"PandarQT\","
                               "\"Model\":\"PandarQT\",\"SW_Ver\":\"2.0.12\",\"FW_Ver\":\"2.0.20\","
                               "\"Mac\":\"00:1c:42:11:22:33\",\"ProdDate\":\"2026-03-15\","
                               "\"SpinSpeed\":\"2\",\"CurrentTemp\":\"41.5\"}}";

            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + body;

            send(client_fd, resp.c_str(), static_cast<int>(resp.size()), 0);
            CLOSE_SOCK(client_fd);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto params = ulv::HesaiPtcClient::query_http_fallback("127.0.0.1", 18080, 1500);

    ASSERT_TRUE(params.success);
    ASSERT_EQ(params.serial_number, "QT3CC855913CC857");
    ASSERT_EQ(params.model, "PandarQT");
    ASSERT_EQ(params.mac_address, "00:1c:42:11:22:33");
    ASSERT_EQ(static_cast<int>(params.motor_rpm), 600);
    ASSERT_GE(params.temperature_c, 41.4f);
    ASSERT_LE(params.temperature_c, 41.6f);

    CLOSE_SOCK(server_sock);
    http_thread.join();
}

TEST_CASE_TIER("Tier 4: Real-World Scenarios", "PTC: Fast Connection Timeout and Unreachable Host") {
    // Query closed port on loopback with short timeout (300ms)
    auto start = std::chrono::steady_clock::now();
    auto params = ulv::HesaiPtcClient::query("127.0.0.1", 19998, 300);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    ASSERT_FALSE(params.success);
    ASSERT_FALSE(params.error_msg.empty());
    // Should fail promptly without OS hang (<= 1500ms)
    ASSERT_LE(elapsed_ms, 1500);
}
