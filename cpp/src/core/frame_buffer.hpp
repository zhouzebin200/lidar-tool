#pragma once

#include "ulv_point_types.hpp"
#include <mutex>
#include <memory>
#include <atomic>

namespace ulv {

class FrameBufferQueue {
public:
    FrameBufferQueue() : has_new_frame_(false) {}

    void push_frame(PointCloudFrame&& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        back_frame_ = std::move(frame);
        has_new_frame_.store(true, std::memory_order_release);
    }

    bool pop_latest_frame(PointCloudFrame& out_frame) {
        if (!has_new_frame_.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_new_frame_.load(std::memory_order_relaxed)) {
            return false;
        }
        out_frame = std::move(back_frame_);
        has_new_frame_.store(false, std::memory_order_release);
        return true;
    }

    bool has_new_frame() const {
        return has_new_frame_.load(std::memory_order_acquire);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        back_frame_ = PointCloudFrame{};
        has_new_frame_.store(false, std::memory_order_release);
    }

private:
    std::mutex mutex_;
    PointCloudFrame back_frame_;
    std::atomic<bool> has_new_frame_;
};

} // namespace ulv
