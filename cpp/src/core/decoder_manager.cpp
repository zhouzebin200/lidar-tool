#include "decoder_manager.hpp"

#include <iostream>
#include <filesystem>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#define DLOPEN_PATH(p) LoadLibraryW((p).c_str())
#define DLSYM(handle, symbol) GetProcAddress((HMODULE)handle, symbol)
#define DLCLOSE(handle) FreeLibrary((HMODULE)handle)
#define DLERROR() "Windows LoadLibrary failed"
#define LIB_EXT ".dll"
#define LIB_PREFIX "lib"
#elif defined(__APPLE__)
#include <dlfcn.h>
#define DLOPEN_PATH(p) dlopen((p).c_str(), RTLD_NOW | RTLD_LOCAL)
#define DLSYM(handle, symbol) dlsym(handle, symbol)
#define DLCLOSE(handle) dlclose(handle)
#define DLERROR() dlerror()
#define LIB_EXT ".dylib"
#define LIB_PREFIX "lib"
#else
#include <dlfcn.h>
#define DLOPEN_PATH(p) dlopen((p).c_str(), RTLD_NOW | RTLD_LOCAL)
#define DLSYM(handle, symbol) dlsym(handle, symbol)
#define DLCLOSE(handle) dlclose(handle)
#define DLERROR() dlerror()
#define LIB_EXT ".so"
#define LIB_PREFIX "lib"
#endif

namespace ulv {

static std::string json_escape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool DecoderManager::has_active_decoder() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_instance_ != nullptr;
}

std::string DecoderManager::active_vendor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_vendor_;
}

std::string DecoderManager::active_model() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_model_;
}

DecoderManager::DecoderManager(FrameBufferQueue& frame_queue)
    : frame_queue_(frame_queue) {}

DecoderManager::~DecoderManager() {
    reset();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, plugin] : loaded_plugins_) {
        if (plugin.library_handle) {
            DLCLOSE(plugin.library_handle);
        }
    }
    loaded_plugins_.clear();
}

bool DecoderManager::load_plugins(const std::string& plugin_dir, const std::string& corrections_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    corrections_dir_ = corrections_dir;

    std::vector<std::string> vendors = {"hesai", "robosense", "velodyne", "huawei_l306", "seyond"};

    for (const auto& v : vendors) {
        std::string lib_name = std::string(LIB_PREFIX) + "ulv_" + v + LIB_EXT;
        std::filesystem::path lib_path = std::filesystem::u8path(plugin_dir) / lib_name;

        std::error_code ec;
        if (!std::filesystem::exists(lib_path, ec)) {
            continue;
        }

        void* handle = DLOPEN_PATH(lib_path);
        if (!handle) {
            std::cerr << "Failed to dlopen " << lib_path.u8string() << ": " << DLERROR() << std::endl;
            continue;
        }

        auto create_fn = reinterpret_cast<void* (*)(const char*, ulv_frame_callback_v1, void*)>(
            DLSYM(handle, "ulv_plugin_create"));
        auto feed_fn = reinterpret_cast<int (*)(void*, const void*)>(
            DLSYM(handle, "ulv_plugin_feed_udp"));
        auto destroy_fn = reinterpret_cast<void (*)(void*)>(
            DLSYM(handle, "ulv_plugin_destroy"));

        if (!create_fn || !feed_fn || !destroy_fn) {
            std::cerr << "Plugin " << lib_name << " missing ABI functions." << std::endl;
            DLCLOSE(handle);
            continue;
        }

        PluginHandle p;
        p.library_handle = handle;
        p.create_fn = create_fn;
        p.feed_fn = feed_fn;
        p.destroy_fn = destroy_fn;

        loaded_plugins_[v] = p;
        std::cout << "[DecoderManager] Loaded plugin: " << v << " from " << lib_path.filename().string() << std::endl;
    }

    return !loaded_plugins_.empty();
}

void DecoderManager::feed_packet(
    const uint8_t* payload,
    size_t size,
    const std::string& src_ip,
    uint16_t src_port,
    uint16_t dst_port,
    uint64_t timestamp_ns
) {
    if (!payload || size == 0) return;

    // Peel off SOME/IP or custom encapsulation if present (8, 12, or 16-byte prepended headers)
    const uint8_t* actual_payload = payload;
    size_t actual_size = size;

    if (size >= 12) {
        if ((actual_payload[0] != 0x55 || actual_payload[1] != 0xAA) &&
            (actual_payload[0] != 0xEE || actual_payload[1] != 0xFF) &&
            (actual_payload[0] != 0xFF || actual_payload[1] != 0xEE)) {
            if ((actual_payload[8] == 0x55 && actual_payload[9] == 0xAA) ||
                (actual_payload[8] == 0xEE && actual_payload[9] == 0xFF) ||
                (actual_payload[8] == 0xFF && actual_payload[9] == 0xEE)) {
                actual_payload += 8;
                actual_size -= 8;
            } else if (size >= 16 && ((actual_payload[12] == 0x55 && actual_payload[13] == 0xAA) ||
                                      (actual_payload[12] == 0xEE && actual_payload[13] == 0xFF) ||
                                      (actual_payload[12] == 0xFF && actual_payload[13] == 0xEE))) {
                actual_payload += 12;
                actual_size -= 12;
            } else if (size >= 20 && ((actual_payload[16] == 0x55 && actual_payload[17] == 0xAA) ||
                                      (actual_payload[16] == 0xEE && actual_payload[17] == 0xFF) ||
                                      (actual_payload[16] == 0xFF && actual_payload[17] == 0xEE))) {
                actual_payload += 16;
                actual_size -= 16;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!active_instance_) {
        DetectedLidar detected = LidarDetector::identify_packet(actual_payload, actual_size, dst_port);
        if (detected.vendor != LidarVendor::Unknown && detected.is_point_cloud) {
            if (!init_decoder_for_device(detected, src_ip, dst_port)) {
                return;
            }
        } else {
            return;
        }
    }

    if (active_instance_ && active_plugin_ && active_plugin_->feed_fn) {
        ulv_udp_packet_v1 pkt{};
        pkt.struct_size = sizeof(ulv_udp_packet_v1);
        pkt.source_ip = src_ip.c_str();
        pkt.source_port = src_port;
        pkt.destination_port = dst_port;
        pkt.receive_timestamp_ns = timestamp_ns;
        pkt.payload = actual_payload;
        pkt.payload_size = actual_size;

        active_plugin_->feed_fn(active_instance_, &pkt);
    }
}

bool DecoderManager::init_decoder_for_device(
    const DetectedLidar& detected,
    const std::string& src_ip,
    uint16_t dst_port
) {
    // Caller feed_packet holds mutex_; invoke reset_unlocked() to avoid self-deadlock
    reset_unlocked();

    std::string plugin_key;
    if (detected.vendor == LidarVendor::Hesai) plugin_key = "hesai";
    else if (detected.vendor == LidarVendor::RoboSense) plugin_key = "robosense";
    else if (detected.vendor == LidarVendor::Velodyne) plugin_key = "velodyne";
    else if (detected.vendor == LidarVendor::Huawei) plugin_key = "huawei_l306";
    else if (detected.vendor == LidarVendor::Seyond) plugin_key = "seyond";
    else return false;

    auto it = loaded_plugins_.find(plugin_key);
    if (it == loaded_plugins_.end()) {
        std::cerr << "No loaded plugin for vendor: " << plugin_key << std::endl;
        return false;
    }

    // Build configuration JSON using escaped UTF-8 forward-slash paths.
    auto corr_path = [&](const char* sub) {
        return (std::filesystem::u8path(corrections_dir_) / sub).generic_u8string();
    };
    const std::string model_esc = json_escape(detected.model);
    const std::string ip_esc = json_escape(src_ip);
    std::string config_json = "{\"model\":\"" + model_esc + "\",\"device_ip\":\"" + ip_esc + "\"}";
    if (detected.vendor == LidarVendor::Hesai) {
        if (detected.model == "AT128" || detected.model == "AT128P") {
            std::string fallback = corr_path(detected.model == "AT128" ? "hesai/AT128E2X.dat" : "hesai/AT128E3X.dat");
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(fallback), ec)) {
                fallback = corr_path("hesai/AT128E3X.dat");
            }
            config_json = "{\"model\":\"" + model_esc + "\",\"device_ip\":\"" + ip_esc + "\",\"fallback_correction_file\":\"" + json_escape(fallback) + "\"}";
        }
    } else if (detected.vendor == LidarVendor::Velodyne) {
        std::string velo_dir = corr_path("velodyne");
        config_json = "{\"model\":\"" + model_esc + "\",\"device_ip\":\"" + ip_esc + "\",\"default_correction_directory\":\"" + json_escape(velo_dir) + "\"}";
    } else if (detected.vendor == LidarVendor::Seyond) {
        std::string fallback = corr_path("seyond/Robin-W.bin");
        config_json = "{\"model\":\"" + model_esc + "\",\"device_ip\":\"" + ip_esc + "\",\"fallback_anglehv_file\":\"" + json_escape(fallback) + "\"}";
    }

    active_plugin_ = &it->second;
    active_instance_ = active_plugin_->create_fn(
        config_json.c_str(),
        on_frame_callback,
        this
    );

    if (!active_instance_) {
        std::cerr << "Failed to create decoder instance for " << detected.model << std::endl;
        active_plugin_ = nullptr;
        return false;
    }

    active_vendor_ = detected.vendor_name;
    active_model_ = detected.model;
    std::cout << "[DecoderManager] Initialized decoder for: " << active_vendor_ << " " << active_model_ << std::endl;
    return true;
}

void DecoderManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_unlocked();
}

void DecoderManager::reset_unlocked() {
    if (active_instance_ && active_plugin_ && active_plugin_->destroy_fn) {
        active_plugin_->destroy_fn(active_instance_);
    }
    active_instance_ = nullptr;
    active_plugin_ = nullptr;
    active_vendor_.clear();
    active_model_.clear();
}

void DecoderManager::on_frame_callback(const struct ulv_frame_v1* raw_frame, void* user_data) {
    auto* self = static_cast<DecoderManager*>(user_data);
    if (!self || !raw_frame) return;

    const auto* f = raw_frame;
    if (!f->points || f->point_count == 0) return;

    PointCloudFrame frame;
    frame.device_id = f->device_id;
    frame.sequence = f->sequence;
    frame.timestamp_ns = f->timestamp_ns;
    frame.vendor = f->vendor ? f->vendor : self->active_vendor_;
    frame.model = f->model ? f->model : self->active_model_;

    // Zero-copy reinterpret / memory copy into points vector
    const auto* src_pts = reinterpret_cast<const UlvPoint*>(f->points);
    frame.points.assign(src_pts, src_pts + f->point_count);

    // Compute bounds for UI
    frame.compute_bounds();

    // Push into lock-free/low-contention frame buffer queue
    self->frame_queue_.push_frame(std::move(frame));
}

} // namespace ulv
