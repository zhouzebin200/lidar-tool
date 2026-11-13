#pragma once

#include <string>
#include <cstdint>
#include <mutex>

namespace ulv {

enum class PtpState {
    FreeRun,
    Listening,
    Tracking,
    Locked
};

struct PtpStatus {
    PtpState state = PtpState::FreeRun;
    std::string state_name = "FREE_RUN (未锁定)";
    std::string grandmaster_id = "--";
    uint8_t domain_number = 0;
    int64_t offset_from_master_ns = 0;
    uint32_t mean_path_delay_ns = 0;
    uint64_t sync_packets_received = 0;
    uint64_t announce_packets_received = 0;
    double last_sync_timestamp = 0.0;
};

class PtpMonitor {
public:
    PtpMonitor();
    ~PtpMonitor();

    void parse_ptp_packet(const uint8_t* data, size_t size, uint16_t port, double current_time);
    PtpStatus status() const;
    PtpStatus status(double current_time);
    void reset();

    bool is_synced() const;
    std::string ptp_status_string() const;

private:
    mutable std::mutex mutex_;
    PtpStatus status_;
};

} // namespace ulv
