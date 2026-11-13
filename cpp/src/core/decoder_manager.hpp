#pragma once

#include "ulv_point_types.hpp"
#include "frame_buffer.hpp"
#include "detector.hpp"
#include "ulv_decoder_plugin.h"

#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <mutex>

namespace ulv {

class DecoderManager {
public:
    DecoderManager(FrameBufferQueue& frame_queue);
    ~DecoderManager();

    bool load_plugins(const std::string& plugin_dir, const std::string& corrections_dir);
    void feed_packet(const uint8_t* payload, size_t size, const std::string& src_ip, uint16_t src_port, uint16_t dst_port, uint64_t timestamp_ns);
    void reset();

    bool has_active_decoder() const;
    std::string active_vendor() const;
    std::string active_model() const;

private:
    struct PluginHandle {
        void* library_handle = nullptr;
        void* (*create_fn)(const char*, ulv_frame_callback_v1, void*) = nullptr;
        int (*feed_fn)(void*, const void*) = nullptr;
        void (*destroy_fn)(void*) = nullptr;
    };

    mutable std::mutex mutex_;
    FrameBufferQueue& frame_queue_;
    std::string corrections_dir_;
    std::unordered_map<std::string, PluginHandle> loaded_plugins_;

    std::string active_vendor_;
    std::string active_model_;
    void* active_instance_ = nullptr;
    PluginHandle* active_plugin_ = nullptr;

    void reset_unlocked();
    bool init_decoder_for_device(const DetectedLidar& detected, const std::string& src_ip, uint16_t dst_port);
    static void on_frame_callback(const struct ulv_frame_v1* raw_frame, void* user_data);
};

} // namespace ulv
