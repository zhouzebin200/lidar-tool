#include "test_framework.hpp"
#include "net/udp_receiver.hpp"
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>

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
#define CLOSE_SOCK(s) close(s)
#endif

namespace {

bool send_udp_datagram(const std::string& target_ip, uint16_t target_port, const std::vector<uint8_t>& data) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_ip.c_str(), &dest_addr.sin_addr);

    ssize_t sent = sendto(sock, reinterpret_cast<const char*>(data.data()), data.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
    CLOSE_SOCK(sock);
    return sent == static_cast<ssize_t>(data.size());
}

} // namespace

TEST_CASE_TIER("Tier 1: Feature Coverage", "UDP: Invalid Arguments Handling") {
    ulv::UdpReceiver receiver;
    // Empty port list should fail gracefully
    bool ok1 = receiver.start("127.0.0.1", {}, [](const uint8_t*, size_t, const std::string&, uint16_t, uint16_t, uint64_t){});
    ASSERT_FALSE(ok1);
    ASSERT_FALSE(receiver.is_running());

    // Null callback should fail gracefully
    bool ok2 = receiver.start("127.0.0.1", {19500}, nullptr);
    ASSERT_FALSE(ok2);
    ASSERT_FALSE(receiver.is_running());
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "UDP: Single Port Ingestion & Counter Verification") {
    ulv::UdpReceiver receiver;
    uint16_t test_port = 19510;

    std::atomic<int> callback_count{0};
    std::atomic<size_t> last_size{0};
    std::string received_str;

    bool started = receiver.start("127.0.0.1", {test_port}, [&](const uint8_t* data, size_t size,
                                                               const std::string& src_ip, uint16_t src_port,
                                                               uint16_t dst_port, uint64_t timestamp_ns) {
        callback_count++;
        last_size = size;
        received_str = std::string(reinterpret_cast<const char*>(data), size);
    });

    ASSERT_TRUE(started);
    ASSERT_TRUE(receiver.is_running());

    // Give worker thread time to bind socket and enter poll loop
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::string test_payload = "LIDAR_PACKET_TEST_PAYLOAD_001";
    std::vector<uint8_t> payload_bytes(test_payload.begin(), test_payload.end());
    bool sent = send_udp_datagram("127.0.0.1", test_port, payload_bytes);
    ASSERT_TRUE(sent);

    // Wait for packet to be received
    int wait_cycles = 0;
    while (callback_count == 0 && wait_cycles < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        wait_cycles++;
    }

    ASSERT_EQ(callback_count.load(), 1);
    ASSERT_EQ(last_size.load(), test_payload.size());
    ASSERT_EQ(received_str, test_payload);
    ASSERT_EQ(receiver.packets_received(), 1);
    ASSERT_EQ(receiver.bytes_received(), test_payload.size());

    receiver.stop();
    ASSERT_FALSE(receiver.is_running());
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "UDP: Empty IP Fallback to INADDR_ANY") {
    ulv::UdpReceiver receiver;
    uint16_t test_port = 19520;
    std::atomic<int> count{0};

    // Passing empty string or 0.0.0.0 binds to INADDR_ANY
    bool started = receiver.start("", {test_port}, [&](const uint8_t*, size_t, const std::string&, uint16_t, uint16_t, uint64_t) {
        count++;
    });
    ASSERT_TRUE(started);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::vector<uint8_t> ping = {'P', 'I', 'N', 'G'};
    send_udp_datagram("127.0.0.1", test_port, ping);

    for (int i = 0; i < 20 && count == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GE(count.load(), 1);
    receiver.stop();
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "UDP: Multi-Port Routing (MSOP & DIFOP)") {
    ulv::UdpReceiver receiver;
    uint16_t msop_port = 19530;
    uint16_t difop_port = 19531;

    std::atomic<int> msop_packets{0};
    std::atomic<int> difop_packets{0};

    bool started = receiver.start("127.0.0.1", {msop_port, difop_port}, [&](const uint8_t*, size_t, const std::string&, uint16_t, uint16_t dst_port, uint64_t) {
        if (dst_port == msop_port) msop_packets++;
        else if (dst_port == difop_port) difop_packets++;
    });
    ASSERT_TRUE(started);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<uint8_t> pkt_msop = {'M', 'S', 'O', 'P'};
    std::vector<uint8_t> pkt_difop = {'D', 'I', 'F', 'O', 'P'};

    send_udp_datagram("127.0.0.1", msop_port, pkt_msop);
    send_udp_datagram("127.0.0.1", difop_port, pkt_difop);

    for (int i = 0; i < 25 && (msop_packets == 0 || difop_packets == 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ASSERT_EQ(msop_packets.load(), 1);
    ASSERT_EQ(difop_packets.load(), 1);
    ASSERT_EQ(receiver.packets_received(), 2);

    receiver.stop();
}

TEST_CASE_TIER("Tier 4: Real-World Scenarios", "UDP: High-Frequency Packet Burst Ingestion") {
    ulv::UdpReceiver receiver;
    uint16_t burst_port = 19540;
    std::atomic<int> total_received{0};

    bool started = receiver.start("127.0.0.1", {burst_port}, [&](const uint8_t*, size_t, const std::string&, uint16_t, uint16_t, uint64_t) {
        total_received++;
    });
    ASSERT_TRUE(started);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const int burst_count = 100;
    std::vector<uint8_t> dummy_lidar_packet(1024, 0xAA);
    for (int i = 0; i < burst_count; ++i) {
        send_udp_datagram("127.0.0.1", burst_port, dummy_lidar_packet);
    }

    for (int i = 0; i < 50 && total_received < burst_count; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ASSERT_EQ(total_received.load(), burst_count);
    ASSERT_EQ(receiver.packets_received(), burst_count);
    ASSERT_EQ(receiver.bytes_received(), burst_count * 1024);

    receiver.stop();
}
