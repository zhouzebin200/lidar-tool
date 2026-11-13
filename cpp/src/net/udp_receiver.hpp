#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <future>
#include <functional>
#include <cstdint>

namespace ulv {

struct UdpPacketData {
    std::string src_ip;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint64_t timestamp_ns = 0;
    std::vector<uint8_t> payload;
};

using UdpPacketCallback = std::function<void(const uint8_t* data, size_t size, const std::string& src_ip, uint16_t src_port, uint16_t dst_port, uint64_t timestamp_ns)>;

class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();

    bool start(const std::string& bind_ip, const std::vector<uint16_t>& ports, UdpPacketCallback callback);
    void stop();

    bool is_running() const { return is_running_.load(); }
    uint64_t packets_received() const { return packets_received_.load(); }
    uint64_t bytes_received() const { return bytes_received_.load(); }

private:
    std::atomic<bool> is_running_{false};
    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::thread worker_thread_;

    void run_worker(const std::string& bind_ip, const std::vector<uint16_t>& ports, UdpPacketCallback callback, std::promise<bool> bind_promise);
};

} // namespace ulv
