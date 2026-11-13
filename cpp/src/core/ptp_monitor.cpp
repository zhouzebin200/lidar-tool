#include "ptp_monitor.hpp"
#include <cstdio>
#include <cmath>

namespace ulv {

PtpMonitor::PtpMonitor() {
    reset();
}

PtpMonitor::~PtpMonitor() = default;

void PtpMonitor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {};
}

PtpStatus PtpMonitor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

PtpStatus PtpMonitor::status(double current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.last_sync_timestamp > 0.0 && (current_time - status_.last_sync_timestamp) > 3.0) {
        status_.state = PtpState::FreeRun;
        status_.state_name = "FREE_RUN (失步超时)";
    }
    return status_;
}

bool PtpMonitor::is_synced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.state == PtpState::Locked;
}

std::string PtpMonitor::ptp_status_string() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.state_name;
}

void PtpMonitor::parse_ptp_packet(const uint8_t* data, size_t size, uint16_t port, double current_time) {
    if (!data || size < 34) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Check PTP version (byte 1 lower 4 bits == 2)
    uint8_t version = data[1] & 0x0F;
    if (version != 2) return;

    uint8_t msg_type = data[0] & 0x0F;
    status_.domain_number = data[4];

    if (msg_type == 0x0) { // Sync
        status_.sync_packets_received++;
        status_.last_sync_timestamp = current_time;

        if (status_.announce_packets_received > 0) {
            status_.state = PtpState::Locked;
            status_.state_name = "LOCKED (已锁定)";
        } else {
            status_.state = PtpState::Tracking;
            status_.state_name = "TRACKING (跟踪中)";
        }
    } else if (msg_type == 0xB && size >= 52) { // Announce
        status_.announce_packets_received++;
        char gmid[32];
        const uint8_t* gm = data + 44;
        std::snprintf(gmid, sizeof(gmid), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                      gm[0], gm[1], gm[2], gm[3], gm[4], gm[5], gm[6], gm[7]);
        status_.grandmaster_id = gmid;

        if (status_.sync_packets_received > 0 && current_time - status_.last_sync_timestamp < 2.0) {
            status_.state = PtpState::Locked;
            status_.state_name = "LOCKED (已锁定)";
        }
    }

    // Check timeout: if no sync received for > 3.0 seconds, revert to free-run
    if (current_time - status_.last_sync_timestamp > 3.0 && status_.last_sync_timestamp > 0.0) {
        status_.state = PtpState::FreeRun;
        status_.state_name = "FREE_RUN (失步超时)";
    }
}

} // namespace ulv
