#pragma once

namespace ulv {

// Tracks a single-frame preview decode triggered while PCAP playback is paused.
class ReplayPreview {
public:
    void reset() {
        pending_ = false;
        exhausted_ = false;
    }

    void request() {
        pending_ = true;
        exhausted_ = false;
    }

    void observe(bool got_frame, bool hit_eof) {
        if (got_frame) {
            pending_ = false;
            exhausted_ = false;
        } else if (hit_eof) {
            pending_ = false;
            exhausted_ = true;
        }
    }

    bool pending() const { return pending_; }
    bool exhausted() const { return exhausted_; }

private:
    bool pending_ = false;
    bool exhausted_ = false;
};

} // namespace ulv
