#include "test_framework.hpp"
#include "ptc_client.hpp"

#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>

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

void write_be32_test(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

// Helper to set up an ephemeral TCP listener on loopback port 0
int create_ephemeral_listener(uint16_t& out_port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = 0; // OS assigns port
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) != 0) {
        CLOSE_SOCK(s);
        return -1;
    }

    struct sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(s, reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len) != 0) {
        CLOSE_SOCK(s);
        return -1;
    }

    out_port = ntohs(bound_addr.sin_port);
    if (listen(s, 5) != 0) {
        CLOSE_SOCK(s);
        return -1;
    }
    return s;
}

// Build a valid PTC 2.0 response packet with 12-byte header
std::vector<uint8_t> build_ptc2_response(uint8_t cmd, uint8_t ret_code, const std::vector<uint8_t>& payload) {
    // PTC 2.0 header:
    // 0..1: "Wu" (0x57, 0x75)
    // 2: Protocol Version (0x01)
    // 3: Command
    // 4: Return code
    // 5: Reserved (0x00)
    // 6: Checksum type (0x00)
    // 7: Checksum length (0x00)
    // 8..11: Payload length (read_be32)
    std::vector<uint8_t> buf(12 + payload.size(), 0);
    buf[0] = 0x57; // 'W'
    buf[1] = 0x75; // 'u'
    buf[2] = 0x01; // ver
    buf[3] = cmd;  // cmd
    buf[4] = ret_code;
    buf[5] = 0x00; // rsv
    buf[6] = 0x00; // check_type
    buf[7] = 0x00; // check_len
    write_be32_test(&buf[8], static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(&buf[12], payload.data(), payload.size());
    }
    return buf;
}

} // namespace

// ============================================================================
// SUITE 1: CORRUPTED PACKET & FUZZING ADVERSARIAL CHALLENGES
// ============================================================================

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Fuzzing: Invalid Magic Identifiers Rejection") {
    // Array of malicious or corrupted magic bytes
    std::vector<std::pair<uint8_t, uint8_t>> bad_magics = {
        {0x00, 0x00}, // Null bytes
        {0xFF, 0xFF}, // All ones
        {0x47, 0x73}, // 'G', 's' (off-by-one from 'G', 't')
        {0x46, 0x74}, // 'F', 't' (off-by-one)
        {0x57, 0x74}, // 'W', 't' (mixed PTC2 / PTC1)
        {0x56, 0x75}, // 'V', 'u'
        {0x12, 0x34}, // Random
        {0xAA, 0x55}  // Alternating bit pattern
    };

    for (const auto& mag : bad_magics) {
        uint16_t port = 0;
        int server_sock = create_ephemeral_listener(port);
        ASSERT_GE(server_sock, 0);

        std::thread srv([&, server_sock, mag]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                uint8_t req[8];
                recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                // Send bad magic header
                uint8_t bad_hdr[8] = {mag.first, mag.second, 0x07, 0x00, 0, 0, 0, 0};
                send(client_fd, reinterpret_cast<const char*>(bad_hdr), 8, 0);
                CLOSE_SOCK(client_fd);
            }
        });

        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
        ASSERT_FALSE(res.success);

        CLOSE_SOCK(server_sock);
        srv.join();
    }
}

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Fuzzing: Oversized Payload Length Protection (>8MB & Overflows)") {
    // Tests: 8MB + 1, 16MB, 2GB (0x80000000), 4GB-1 (0xFFFFFFFF)
    std::vector<uint32_t> malicious_lens = {
        8 * 1024 * 1024 + 1,
        16 * 1024 * 1024,
        0x80000000U,
        0xFFFFFFFFU
    };

    for (uint32_t len_val : malicious_lens) {
        uint16_t port = 0;
        int server_sock = create_ephemeral_listener(port);
        ASSERT_GE(server_sock, 0);

        std::thread srv([&, server_sock, len_val]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                uint8_t req[8];
                recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                // Send header with malicious payload length
                uint8_t bad_hdr[8] = {0x47, 0x74, 0x07, 0x00, 0, 0, 0, 0};
                write_be32_test(&bad_hdr[4], len_val);
                send(client_fd, reinterpret_cast<const char*>(bad_hdr), 8, 0);
                CLOSE_SOCK(client_fd);
            }
        });

        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
        ASSERT_FALSE(res.success);

        CLOSE_SOCK(server_sock);
        srv.join();
    }
}

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Fuzzing: Truncated Headers (<8B for PTC1, <12B for PTC2)") {
    // Truncated header fragment lengths for PTC 1.0 (needs 8B total)
    std::vector<size_t> ptc1_trunc_lens = {0, 1, 2, 3, 5, 7};
    for (size_t trunc : ptc1_trunc_lens) {
        uint16_t port = 0;
        int server_sock = create_ephemeral_listener(port);
        ASSERT_GE(server_sock, 0);

        std::thread srv([&, server_sock, trunc]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                uint8_t req[8];
                recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (trunc > 0) {
                    uint8_t hdr[8] = {0x47, 0x74, 0x07, 0x00, 0, 0, 0, 0};
                    send(client_fd, reinterpret_cast<const char*>(hdr), static_cast<int>(trunc), 0);
                }
                CLOSE_SOCK(client_fd);
            }
        });

        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
        ASSERT_FALSE(res.success);

        CLOSE_SOCK(server_sock);
        srv.join();
    }

    // Truncated header fragment lengths for PTC 2.0 (needs 12B total)
    std::vector<size_t> ptc2_trunc_lens = {2, 4, 6, 8, 11};
    for (size_t trunc : ptc2_trunc_lens) {
        uint16_t port = 0;
        int server_sock = create_ephemeral_listener(port);
        ASSERT_GE(server_sock, 0);

        std::thread srv([&, server_sock, trunc]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                uint8_t req[8];
                recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                uint8_t hdr[12] = {0x57, 0x75, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0};
                send(client_fd, reinterpret_cast<const char*>(hdr), static_cast<int>(trunc), 0);
                CLOSE_SOCK(client_fd);
            }
        });

        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
        ASSERT_FALSE(res.success);

        CLOSE_SOCK(server_sock);
        srv.join();
    }
}

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Fuzzing: Truncated Payload Abrupt Termination") {
    // Declares 100 bytes payload, but sends only 15 bytes and disconnects
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            uint8_t req[8];
            recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
            uint8_t hdr[8] = {0x47, 0x74, 0x07, 0x00, 0, 0, 0, 100}; // declares 100 bytes
            send(client_fd, reinterpret_cast<const char*>(hdr), 8, 0);
            uint8_t partial[15] = {0};
            send(client_fd, reinterpret_cast<const char*>(partial), 15, 0);
            CLOSE_SOCK(client_fd);
        }
    });

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
    ASSERT_FALSE(res.success);

    CLOSE_SOCK(server_sock);
    srv.join();
}

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Fuzzing: Command ID Mismatch and Non-Zero Return Codes") {
    // Case 1: Server sends wrong command ID (sends 0x09 instead of 0x07)
    {
        uint16_t port = 0;
        int server_sock = create_ephemeral_listener(port);
        ASSERT_GE(server_sock, 0);

        std::thread srv([&, server_sock]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                uint8_t req[8];
                recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                auto resp = ulv::HesaiPtcClient::build_ptc1_response(0x09, 0x00, {}); // Mismatch
                send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                CLOSE_SOCK(client_fd);
            }
        });

        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
        ASSERT_FALSE(res.success);

        CLOSE_SOCK(server_sock);
        srv.join();
    }

    // Case 2: Server returns non-zero return code (0x01: Error) on Command 0x07
    {
        uint16_t port = 0;
        int server_sock = create_ephemeral_listener(port);
        ASSERT_GE(server_sock, 0);

        std::thread srv([&, server_sock]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                uint8_t req[8];
                recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                auto resp = ulv::HesaiPtcClient::build_ptc1_response(0x07, 0x01, {}); // Return code 1
                send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                CLOSE_SOCK(client_fd);
            }
        });

        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 500);
        ASSERT_FALSE(res.success);
        ASSERT_CONTAINS(res.error_msg, "PTC 0x07");

        CLOSE_SOCK(server_sock);
        srv.join();
    }
}

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Fuzzing: Corrupted Binary Payloads Robustness") {
    ulv::LidarDiagnosticParams out;

    // 1. All zeros payload
    std::vector<uint8_t> zeros(120, 0);
    bool ok_zeros = ulv::HesaiPtcClient::parse_inventory_payload(zeros.data(), zeros.size(), out);
    ASSERT_TRUE(ok_zeros); // Should extract empty strings gracefully
    ASSERT_TRUE(out.serial_number.empty());

    // 2. All 0xFF non-null-terminated payload
    std::vector<uint8_t> all_ff(120, 0xFF);
    bool ok_ff = ulv::HesaiPtcClient::parse_inventory_payload(all_ff.data(), all_ff.size(), out);
    ASSERT_TRUE(ok_ff);
    ASSERT_EQ(out.serial_number.size(), 18); // Bounded strictly to 18 bytes

    // 3. Status payload sub-minimum length
    std::vector<uint8_t> short_status(9, 0);
    ASSERT_FALSE(ulv::HesaiPtcClient::parse_status_payload(short_status.data(), short_status.size(), out));

    // 4. Monitor payload sub-minimum length
    std::vector<uint8_t> short_mon(11, 0);
    ASSERT_FALSE(ulv::HesaiPtcClient::parse_monitor_payload(short_mon.data(), short_mon.size(), out));

    // 5. Config payload sub-minimum length
    std::vector<uint8_t> short_cfg(17, 0);
    ASSERT_FALSE(ulv::HesaiPtcClient::parse_config_payload(short_cfg.data(), short_cfg.size(), out));
}

// ============================================================================
// SUITE 2: DUAL PTC 1.0 & PTC 2.0 PROTOCOL SIMULATION (MOCK LIDAR SERVERS)
// ============================================================================

TEST_CASE_TIER("Milestone 3 Challenge", "Mock Lidar: Hesai AT128 (PTC 1.0 Standard)") {
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            while (running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    if (cmd == 0x07) {
                        std::vector<uint8_t> payload(120, 0);
                        std::string sn = "AT128-PROD-2026";
                        std::string pn = "PN-AT128-HW2";
                        std::string fw = "v3.2.0";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        std::memcpy(&payload[18], pn.c_str(), pn.size());
                        std::memcpy(&payload[50], "2026-06-10", 10);
                        payload[66] = 0x00; payload[67] = 0x1C; payload[68] = 0x42;
                        payload[69] = 0xDE; payload[70] = 0xAD; payload[71] = 0x01;
                        std::memcpy(&payload[72], fw.c_str(), fw.size());
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x09) {
                        std::vector<uint8_t> payload(44, 0);
                        write_be32_test(&payload[0], 5400); // 1.5h
                        payload[4] = 0x04; payload[5] = 0xB0; // 1200 RPM (0x04B0)
                        write_be32_test(&payload[6], 4450);  // 44.5 C
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x27) {
                        std::vector<uint8_t> payload(16, 0);
                        write_be32_test(&payload[0], 120000); // 1200 mA
                        write_be32_test(&payload[4], 1220);   // 12.2 V
                        write_be32_test(&payload[8], 1464);   // 14.64 W
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x08) {
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

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1500);
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.model, "Hesai AT128");
    ASSERT_EQ(res.serial_number, "AT128-PROD-2026");
    ASSERT_EQ(res.product_number, "PN-AT128-HW2");
    ASSERT_EQ(res.firmware_version, "v3.2.0");
    ASSERT_EQ(res.mac_address, "00:1C:42:DE:AD:01");
    ASSERT_EQ(static_cast<int>(res.motor_rpm), 1200);
    ASSERT_GE(res.temperature_c, 44.4f);
    ASSERT_GE(res.operating_voltage, 12.1f);
    ASSERT_GE(res.operating_current_ma, 1199.0f);
    ASSERT_EQ(res.ip_address, "192.168.1.201");
    ASSERT_EQ(res.destination_port, 2368);

    running = false;
    CLOSE_SOCK(server_sock);
    srv.join();
}

TEST_CASE_TIER("Milestone 3 Challenge", "Mock Lidar: Hesai Pandar40P (PTC 1.0 Standard)") {
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            while (running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    if (cmd == 0x07) {
                        std::vector<uint8_t> payload(120, 0);
                        std::string sn = "PA40P-CALIB-888";
                        std::string pn = "Pandar40P";
                        std::string fw = "v1.4.1";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        std::memcpy(&payload[18], pn.c_str(), pn.size());
                        std::memcpy(&payload[50], "2025-10-01", 10);
                        payload[66] = 0x00; payload[67] = 0x1C; payload[68] = 0x42;
                        payload[69] = 0x40; payload[70] = 0x40; payload[71] = 0x40;
                        std::memcpy(&payload[72], fw.c_str(), fw.size());
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x09) {
                        std::vector<uint8_t> payload(44, 0);
                        write_be32_test(&payload[0], 10800); // 3h
                        payload[4] = 0x02; payload[5] = 0x58; // 600 RPM (0x0258)
                        write_be32_test(&payload[6], 3850);  // 38.5 C
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x27) {
                        std::vector<uint8_t> payload(16, 0);
                        write_be32_test(&payload[0], 50000);  // 500 mA
                        write_be32_test(&payload[4], 2400);   // 24.0 V
                        write_be32_test(&payload[8], 1200);   // 12.0 W
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x08) {
                        std::vector<uint8_t> payload(18, 0);
                        payload[0] = 192; payload[1] = 168; payload[2] = 1; payload[3] = 201;
                        payload[4] = 255; payload[5] = 255; payload[6] = 255; payload[7] = 0;
                        payload[8] = 192; payload[9] = 168; payload[10] = 1; payload[11] = 1;
                        payload[12] = 192; payload[13] = 168; payload[14] = 1; payload[15] = 200;
                        payload[16] = 0x09; payload[17] = 0x40; // 2368
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    }
                }
            }
            CLOSE_SOCK(client_fd);
        }
    });

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1500);
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.model, "Hesai Pandar40");
    ASSERT_EQ(res.serial_number, "PA40P-CALIB-888");
    ASSERT_EQ(res.product_number, "Pandar40P");
    ASSERT_EQ(static_cast<int>(res.motor_rpm), 600);
    ASSERT_GE(res.temperature_c, 38.4f);
    ASSERT_GE(res.operating_voltage, 23.9f);
    ASSERT_GE(res.operating_current_ma, 499.0f);

    running = false;
    CLOSE_SOCK(server_sock);
    srv.join();
}

TEST_CASE_TIER("Milestone 3 Challenge", "Mock Lidar: Hesai OT128 (PTC 1.0 Custom Layout & 8 Temp Channels)") {
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            while (running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    if (cmd == 0x07) {
                        // OT128 Inventory layout:
                        // 0..17: SN ("OT128-BENCH-007")
                        // 18..33: Date ("2026-04-12")
                        // 34..39: MAC (AA:BB:CC:11:22:33)
                        // 40..55: SW Version ("SW_OT128_V2")
                        // 56..71: HW Version ("HW_REV_3")
                        // 72..87: Controller FW ("FW_CTRL_1")
                        // 109..140: PN ("PN-OT128-ULTRA")
                        std::vector<uint8_t> payload(145, 0);
                        std::string sn = "OT128-BENCH-007";
                        std::string date = "2026-04-12";
                        std::string sw = "SW_OT128_V2";
                        std::string hw = "HW_REV_3";
                        std::string ctrl = "FW_CTRL_1";
                        std::string pn = "PN-OT128-ULTRA";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        std::memcpy(&payload[18], date.c_str(), date.size());
                        payload[34] = 0xAA; payload[35] = 0xBB; payload[36] = 0xCC;
                        payload[37] = 0x11; payload[38] = 0x22; payload[39] = 0x33;
                        std::memcpy(&payload[40], sw.c_str(), sw.size());
                        std::memcpy(&payload[56], hw.c_str(), hw.size());
                        std::memcpy(&payload[72], ctrl.c_str(), ctrl.size());
                        std::memcpy(&payload[109], pn.c_str(), pn.size());
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x09) {
                        // OT128 Status layout: 8 temperature channels (6 + 8*4 = 38 bytes),
                        // startup_times at 40..43, total_op_time at 44..47, ptp_status at 48, humidity at 49..52
                        std::vector<uint8_t> payload(54, 0);
                        write_be32_test(&payload[0], 12000); // 12000s uptime
                        payload[4] = 0x02; payload[5] = 0x58; // 600 RPM
                        for (int i = 0; i < 8; ++i) {
                            write_be32_test(&payload[6 + i * 4], 4100 + i * 50); // 41.0C .. 44.5C
                        }
                        write_be32_test(&payload[40], 42); // 42 boots
                        write_be32_test(&payload[44], 864000); // total 10 days
                        payload[48] = 2; // Locked
                        write_be32_test(&payload[49], 355); // 35.5% humidity
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x27) {
                        std::vector<uint8_t> payload(16, 0);
                        write_be32_test(&payload[0], 150000); // 1500 mA
                        write_be32_test(&payload[4], 1200);   // 12.0 V
                        write_be32_test(&payload[8], 1800);   // 18.0 W
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x08) {
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

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1500);
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.model, "Hesai OT128");
    ASSERT_EQ(res.serial_number, "OT128-BENCH-007");
    ASSERT_EQ(res.product_number, "PN-OT128-ULTRA");
    ASSERT_EQ(res.date_of_manufacture, "2026-04-12");
    ASSERT_EQ(res.mac_address, "AA:BB:CC:11:22:33");
    ASSERT_CONTAINS(res.firmware_version, "SW_OT128_V2");
    ASSERT_CONTAINS(res.firmware_version, "FW_CTRL_1");
    ASSERT_EQ(res.hardware_version, "HW_REV_3");
    ASSERT_EQ(res.startup_times, 42);
    ASSERT_EQ(res.total_operation_time_s, 864000);
    ASSERT_EQ(res.ptp_status, 2);
    ASSERT_GE(res.humidity_percent, 35.4f);
    ASSERT_EQ(res.temperatures_c.size(), 8);
    ASSERT_GE(res.temperature_c, 40.9f);

    running = false;
    CLOSE_SOCK(server_sock);
    srv.join();
}

TEST_CASE_TIER("Milestone 3 Challenge", "Mock Lidar: Hesai FT120 Solid-State (0 RPM & 0x27 Failure Handling)") {
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            while (running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    if (cmd == 0x07) {
                        std::vector<uint8_t> payload(120, 0);
                        std::string sn = "FT120-FLASH-001";
                        std::string pn = "PN-FT120-SOLID";
                        std::string fw = "v2.1.0";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        std::memcpy(&payload[18], pn.c_str(), pn.size());
                        std::memcpy(&payload[50], "2026-01-15", 10);
                        payload[66] = 0x00; payload[67] = 0x1C; payload[68] = 0x42;
                        payload[69] = 0xFF; payload[70] = 0xEE; payload[71] = 0xDD;
                        std::memcpy(&payload[72], fw.c_str(), fw.size());
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x09) {
                        std::vector<uint8_t> payload(44, 0);
                        write_be32_test(&payload[0], 1800);
                        payload[4] = 0x00; payload[5] = 0x00; // Solid-state reports 0 RPM
                        write_be32_test(&payload[6], 4600);   // 46.0 C
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x27) {
                        // ADVERSARIAL CHALLENGE: Pure solid-state FT120 returns Command Not Supported (0x01)
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x01, {}); // Error code 1
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x08) {
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

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1500);
    // Crucial check: Query MUST NOT fail despite Command 0x27 error return code!
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.model, "Hesai FT120");
    ASSERT_EQ(res.serial_number, "FT120-FLASH-001");
    // Solid state motor RPM must strictly be 0.0f
    ASSERT_EQ(res.motor_rpm, 0.0f);
    ASSERT_GE(res.temperature_c, 45.9f);
    ASSERT_EQ(res.destination_port, 2368);

    running = false;
    CLOSE_SOCK(server_sock);
    srv.join();
}

TEST_CASE_TIER("Milestone 3 Challenge", "Mock Lidar: Hesai PTC 2.0 Server ('Wu' 12-Byte Header Framing)") {
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            while (running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    if (cmd == 0x07) {
                        std::vector<uint8_t> payload(120, 0);
                        std::string sn = "ATXP-PTC2-777";
                        std::string pn = "PN-ATXP";
                        std::string fw = "v4.0.0_ptc2";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        std::memcpy(&payload[18], pn.c_str(), pn.size());
                        std::memcpy(&payload[50], "2026-09-20", 10);
                        payload[66] = 0x00; payload[67] = 0x1C; payload[68] = 0x42;
                        payload[69] = 0x22; payload[70] = 0x33; payload[71] = 0x44;
                        std::memcpy(&payload[72], fw.c_str(), fw.size());
                        // Send PTC 2.0 response ("Wu" magic)
                        auto resp = build_ptc2_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x09) {
                        std::vector<uint8_t> payload(44, 0);
                        write_be32_test(&payload[0], 7200);
                        payload[4] = 0x04; payload[5] = 0xB0; // 1200 RPM
                        write_be32_test(&payload[6], 4350);  // 43.5 C
                        auto resp = build_ptc2_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x27) {
                        std::vector<uint8_t> payload(16, 0);
                        write_be32_test(&payload[0], 110000); // 1100 mA
                        write_be32_test(&payload[4], 1200);   // 12.0 V
                        write_be32_test(&payload[8], 1320);   // 13.2 W
                        auto resp = build_ptc2_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    } else if (cmd == 0x08) {
                        std::vector<uint8_t> payload(18, 0);
                        payload[0] = 192; payload[1] = 168; payload[2] = 1; payload[3] = 201;
                        payload[4] = 255; payload[5] = 255; payload[6] = 255; payload[7] = 0;
                        payload[8] = 192; payload[9] = 168; payload[10] = 1; payload[11] = 1;
                        payload[12] = 192; payload[13] = 168; payload[14] = 1; payload[15] = 100;
                        payload[16] = 0x09; payload[17] = 0x40; // 2368
                        auto resp = build_ptc2_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    }
                }
            }
            CLOSE_SOCK(client_fd);
        }
    });

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1500);
    // PTC 2.0 framing must be correctly detected and decoded
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.model, "Hesai ATXP");
    ASSERT_EQ(res.serial_number, "ATXP-PTC2-777");
    ASSERT_EQ(res.firmware_version, "v4.0.0_ptc2");
    ASSERT_EQ(static_cast<int>(res.motor_rpm), 1200);
    ASSERT_GE(res.temperature_c, 43.4f);
    ASSERT_GE(res.operating_voltage, 11.9f);

    running = false;
    CLOSE_SOCK(server_sock);
    srv.join();
}

// ============================================================================
// SUITE 3: NETWORK STRESS, SLOW-LORIS & CONCURRENCY
// ============================================================================

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Network Stress: Slow-Loris Byte-by-Byte Transmission") {
    // Simulates an adversary sending responses 1 byte at a time with minor delays
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
        if (client_fd >= 0) {
            while (running) {
                uint8_t req[8];
                ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                if (r <= 0) break;
                if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                    uint8_t cmd = req[2];
                    std::vector<uint8_t> payload(120, 0);
                    std::string sn = "SLOW-LORIS-OK";
                    std::memcpy(&payload[0], sn.c_str(), sn.size());
                    auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                    // Send 1 byte at a time with 1ms delay
                    for (size_t i = 0; i < resp.size(); ++i) {
                        send(client_fd, reinterpret_cast<const char*>(&resp[i]), 1, 0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
            CLOSE_SOCK(client_fd);
        }
    });

    auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1500);
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.serial_number, "SLOW-LORIS-OK");

    running = false;
    CLOSE_SOCK(server_sock);
    srv.join();
}

TEST_CASE_TIER("Milestone 3 Challenge", "PTC Network Stress: Rapid Consecutive Queries (Socket Churn)") {
    // Set up mock server and run 25 rapid queries to ensure zero descriptor leakage
    uint16_t port = 0;
    int server_sock = create_ephemeral_listener(port);
    ASSERT_GE(server_sock, 0);

    std::atomic<bool> running{true};
    std::thread srv([&, server_sock]() {
        while (running) {
            struct pollfd pfd{};
            pfd.fd = server_sock;
            pfd.events = POLLIN;
            int pret = poll(&pfd, 1, 50);
            if (pret <= 0 || !(pfd.revents & POLLIN)) continue;

            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (client_fd >= 0) {
                while (true) {
                    uint8_t req[8];
                    ssize_t r = recv(client_fd, reinterpret_cast<char*>(req), 8, 0);
                    if (r <= 0) break;
                    if (r == 8 && req[0] == 0x47 && req[1] == 0x74) {
                        uint8_t cmd = req[2];
                        std::vector<uint8_t> payload(120, 0);
                        std::string sn = "CHURN-PASS";
                        std::memcpy(&payload[0], sn.c_str(), sn.size());
                        auto resp = ulv::HesaiPtcClient::build_ptc1_response(cmd, 0x00, payload);
                        send(client_fd, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
                    }
                }
                CLOSE_SOCK(client_fd);
            }
        }
    });

    for (int i = 0; i < 25; ++i) {
        auto res = ulv::HesaiPtcClient::query_ptc("127.0.0.1", port, 1000);
        ASSERT_TRUE(res.success);
        ASSERT_EQ(res.serial_number, "CHURN-PASS");
    }

    running = false;
    srv.join();
    CLOSE_SOCK(server_sock);
}
