#include "ulv_decoder_plugin.h"
#include "robosense_e2.hpp"

#include <rs_driver/api/lidar_driver.hpp>
#include <rs_driver/msg/point_cloud_msg.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using RsPoint = PointXYZIRT;
using RsCloud = PointCloudT<RsPoint>;
using robosense::lidar::Error;
using robosense::lidar::InputType;
using robosense::lidar::LidarDriver;
using robosense::lidar::LidarType;
using robosense::lidar::Packet;
using robosense::lidar::RSDriverParam;

namespace {

std::string json_string(const char* json, const char* key) {
  if (json == nullptr) return {};
  const std::string source(json);
  const std::string marker = std::string("\"") + key + "\"";
  size_t position = source.find(marker);
  if (position == std::string::npos) return {};
  position = source.find(':', position + marker.size());
  if (position == std::string::npos) return {};
  position = source.find('"', position + 1);
  if (position == std::string::npos) return {};
  const size_t end = source.find('"', position + 1);
  return end == std::string::npos ? std::string() : source.substr(position + 1, end - position - 1);
}

uint32_t json_u32(const char* json, const char* key, uint32_t fallback) {
  if (json == nullptr) return fallback;
  const std::string source(json);
  const std::string marker = std::string("\"") + key + "\"";
  size_t position = source.find(marker);
  if (position == std::string::npos) return fallback;
  position = source.find(':', position + marker.size());
  if (position == std::string::npos) return fallback;
  position++;
  while (position < source.size() && std::isspace(static_cast<unsigned char>(source[position]))) position++;
  try {
    return static_cast<uint32_t>(std::stoul(source.substr(position)));
  } catch (...) {
    return fallback;
  }
}

bool lidar_type_for(const std::string& model, LidarType* result) {
  static const std::unordered_map<std::string, LidarType> types = {
      {"RS16", LidarType::RS16}, {"RS-LiDAR-16", LidarType::RS16},
      {"RS32", LidarType::RS32}, {"RS-LiDAR-32", LidarType::RS32},
      {"RSBP", LidarType::RSBP}, {"RS-Bpearl", LidarType::RSBP},
      {"RSAIRY", LidarType::RSAIRY}, {"RS-AIRY", LidarType::RSAIRY},
      {"RSAIRYLITE_ETH", LidarType::RSAIRYLITE_ETH}, {"RS-AIRYLITE-ETH", LidarType::RSAIRYLITE_ETH},
      {"RSFAIRY", LidarType::RSFAIRY}, {"RS-FAIRY", LidarType::RSFAIRY},
      {"RSHELIOS", LidarType::RSHELIOS}, {"RS-Helios", LidarType::RSHELIOS},
      {"RSHELIOS_16P", LidarType::RSHELIOS_16P}, {"RS-Helios-16P", LidarType::RSHELIOS_16P},
      {"RS128", LidarType::RS128}, {"RS-Ruby-128", LidarType::RS128},
      {"RS80", LidarType::RS80}, {"RS-Ruby-80", LidarType::RS80},
      {"RS48", LidarType::RS48}, {"RS-Ruby-48", LidarType::RS48},
      {"RSP128", LidarType::RSP128}, {"RS-Ruby-Plus-128", LidarType::RSP128},
      {"RSP80", LidarType::RSP80}, {"RS-Ruby-Plus-80", LidarType::RSP80},
      {"RSP48", LidarType::RSP48}, {"RS-Ruby-Plus-48", LidarType::RSP48},
      {"RSM1", LidarType::RSM1}, {"RS-LiDAR-M1", LidarType::RSM1},
      {"RSM1_JUMBO", LidarType::RSM1_JUMBO},
      {"RSM2", LidarType::RSM2}, {"RS-LiDAR-M2", LidarType::RSM2},
      {"RSM3", LidarType::RSM3}, {"RS-LiDAR-M3", LidarType::RSM3},
      {"RSE1", LidarType::RSE1}, {"RS-LiDAR-E1", LidarType::RSE1},
      {"RSMX", LidarType::RSMX}, {"RS-LiDAR-MX", LidarType::RSMX},
      {"RSEMX", LidarType::RSEMX}, {"RS-LiDAR-EMX", LidarType::RSEMX},
  };
  const auto iterator = types.find(model);
  if (iterator == types.end()) return false;
  *result = iterator->second;
  return true;
}

bool has_builtin_mems_geometry(LidarType type) {
  // These solid-state decoders ship usable nominal geometry in rs_driver.
  // DIFOP still replaces it when available, but missing/different-port DIFOP
  // must not suppress every MSOP frame indefinitely (notably automotive EMX).
  return type == LidarType::RSM1 || type == LidarType::RSM1_JUMBO ||
         type == LidarType::RSM2 || type == LidarType::RSM3 ||
         type == LidarType::RSE1 || type == LidarType::RSMX ||
         type == LidarType::RSEMX;
}

std::vector<std::string> candidate_models_for(const std::string& detected_model) {
  if (detected_model == "RS16 / RS32 / RSBP") {
    return {"RS16", "RS32", "RSBP"};
  }
  if (detected_model == "RS48 / RSP48") {
    return {"RS48", "RSP48"};
  }
  if (detected_model.find("MEMS") != std::string::npos) {
    return {"RSM1", "RSM2", "RSM3", "RSE1", "RSMX", "RSEMX", "RSM1_JUMBO"};
  }
  if (detected_model.find("Ruby") != std::string::npos ||
      detected_model.find("Helios") != std::string::npos ||
      detected_model.find("AIRY") != std::string::npos ||
      detected_model.find("FAIRY") != std::string::npos) {
    return {"RS128", "RS80", "RS48", "RSP128", "RSP80", "RSP48",
            "RSHELIOS", "RSHELIOS_16P", "RSAIRY", "RSFAIRY"};
  }
  // A DIFOP packet can arrive before MSOP and standard-port traffic may not
  // expose a unique signature. Probe every decoder and lock onto the first
  // one that produces a plausible complete frame.
  return {"RS16", "RS32", "RSBP", "RSAIRY", "RSAIRYLITE_ETH", "RSFAIRY",
          "RSHELIOS", "RSHELIOS_16P", "RS128", "RS80", "RS48", "RSP128",
          "RSP80", "RSP48", "RSM1", "RSM2", "RSM3", "RSE1", "RSMX",
          "RSEMX", "RSM1_JUMBO"};
}

class RoboSenseAdapter {
 public:
  RoboSenseAdapter(std::string model, uint32_t device_id, ulv_frame_callback_v1 callback, void* user_data)
      : model_(std::move(model)), device_id_(device_id), callback_(callback), user_data_(user_data) {}

  bool start() {
    if (callback_ == nullptr) return false;
    if (model_ == "RSE2" || model_ == "RS-LiDAR-E2" || model_ == "E2") {
      e2_.reset(new E2Decoder(device_id_, callback_, user_data_));
      return true;
    }
    LidarType exact_type;
    if (lidar_type_for(model_, &exact_type)) {
      return start_candidate(model_, exact_type);
    }
    for (const std::string& candidate_model : candidate_models_for(model_)) {
      LidarType type;
      if (lidar_type_for(candidate_model, &type)) start_candidate(candidate_model, type);
    }
    return !candidates_.empty();
  }

  int feed(const ulv_udp_packet_v1* packet) {
    if (packet == nullptr || packet->payload == nullptr || packet->payload_size == 0) return -1;
    return feed_payload(packet->payload, packet->payload_size, packet->receive_timestamp_ns);
  }

  int feed_payload(const uint8_t* payload, size_t payload_size, uint64_t receive_timestamp_ns) {
    if (payload == nullptr || payload_size == 0) return -1;
    if (!e2_ && (E2Decoder::is_msop(payload, payload_size) || E2Decoder::is_angles(payload, payload_size)))
      e2_.reset(new E2Decoder(device_id_, callback_, user_data_));
    if (e2_) {
      e2_->feed(payload, payload_size, receive_timestamp_ns);
      return 0;
    }
    Packet rs_packet(payload_size);
    std::memcpy(rs_packet.buf_.data(), payload, payload_size);
    rs_packet.timestamp = static_cast<double>(receive_timestamp_ns) / 1e9;
    Candidate* selected = selected_.load();
    if (selected != nullptr) {
      selected->driver->decodePacket(rs_packet);
      return 0;
    }
    for (const auto& candidate : candidates_) candidate->driver->decodePacket(rs_packet);
    return 0;
  }

  ~RoboSenseAdapter() {
    for (const auto& candidate : candidates_) candidate->driver->stop();
  }

 private:
  struct Candidate {
    std::string model;
    std::unique_ptr<LidarDriver<RsCloud>> driver;
    std::mutex cloud_pool_mutex;
    std::shared_ptr<RsCloud> reusable_cloud;
  };

  bool start_candidate(const std::string& model, LidarType type) {
    std::unique_ptr<Candidate> candidate(new Candidate());
    candidate->model = model;
    candidate->driver.reset(new LidarDriver<RsCloud>());
    RSDriverParam parameter;
    parameter.input_type = InputType::RAW_PACKET;
    parameter.lidar_type = type;
    parameter.decoder_param.dense_points = true;
    parameter.decoder_param.wait_for_difop = !has_builtin_mems_geometry(type);
    Candidate* candidate_pointer = candidate.get();
    candidate->driver->regPointCloudCallback(
        [candidate_pointer]() {
          std::lock_guard<std::mutex> guard(candidate_pointer->cloud_pool_mutex);
          if (candidate_pointer->reusable_cloud) {
            std::shared_ptr<RsCloud> cloud = std::move(candidate_pointer->reusable_cloud);
            cloud->points.clear();
            return cloud;
          }
          return std::make_shared<RsCloud>();
        },
        [this, candidate_pointer](std::shared_ptr<RsCloud> cloud) {
          on_cloud(candidate_pointer, cloud);
          cloud->points.clear();
          std::lock_guard<std::mutex> guard(candidate_pointer->cloud_pool_mutex);
          candidate_pointer->reusable_cloud = std::move(cloud);
        });
    candidate->driver->regExceptionCallback([](const Error&) {});
    if (!candidate->driver->init(parameter) || !candidate->driver->start()) return false;
    candidates_.push_back(std::move(candidate));
    return true;
  }

  void on_cloud(Candidate* candidate, const std::shared_ptr<RsCloud>& cloud) {
    if (cloud == nullptr || cloud->points.size() < 16) return;
    size_t plausible = 0;
    const size_t sample_count = std::min<size_t>(cloud->points.size(), 512);
    for (size_t index = 0; index < sample_count; index++) {
      const auto& point = cloud->points[index];
      const float range_squared = point.x * point.x + point.y * point.y + point.z * point.z;
      if (std::isfinite(range_squared) && range_squared > 0.000001f && range_squared < 4000000.0f) plausible++;
    }
    if (plausible * 4 < sample_count * 3) return;

    Candidate* expected = nullptr;
    selected_.compare_exchange_strong(expected, candidate);
    if (selected_.load() != candidate) return;

    std::lock_guard<std::mutex> guard(frame_mutex_);
    converted_.clear();
    converted_.reserve(cloud->points.size());
    for (const auto& source : cloud->points) {
      if (!std::isfinite(source.x) || !std::isfinite(source.y) || !std::isfinite(source.z)) continue;
      const float range_squared = source.x * source.x + source.y * source.y + source.z * source.z;
      if (range_squared <= 0.000001f) continue;
      ulv_point_v1 point{};
      point.x = source.x;
      point.y = source.y;
      point.z = source.z;
      point.intensity = static_cast<float>(source.intensity) / 255.0f;
      point.ring = source.ring;
      converted_.push_back(point);
    }
    ulv_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.device_id = device_id_;
    frame.sequence = sequence_.fetch_add(1);
    frame.timestamp_ns = static_cast<uint64_t>(cloud->timestamp * 1e9);
    frame.vendor = "RoboSense";
    frame.model = candidate->model.c_str();
    frame.points = converted_.data();
    frame.point_count = converted_.size();
    callback_(&frame, user_data_);
  }

  std::unique_ptr<E2Decoder> e2_;
  std::string model_;
  uint32_t device_id_;
  ulv_frame_callback_v1 callback_;
  void* user_data_;
  std::vector<std::unique_ptr<Candidate>> candidates_;
  std::atomic<Candidate*> selected_{nullptr};
  std::mutex frame_mutex_;
  std::vector<ulv_point_v1> converted_;
  std::atomic<uint64_t> sequence_{0};
};

}  // namespace

extern "C" {

uint32_t ulv_plugin_abi_version(void) { return ULV_PLUGIN_ABI_VERSION; }

const char* ulv_plugin_manifest_json(void) {
  return "{\"name\":\"RoboSense rs_driver adapter\",\"vendor\":\"RoboSense\",\"abi\":2}";
}

void* ulv_plugin_create(const char* config_json, ulv_frame_callback_v1 callback, void* user_data) {
  const std::string model = json_string(config_json, "model");
  std::unique_ptr<RoboSenseAdapter> adapter(
      new RoboSenseAdapter(model, json_u32(config_json, "device_id", 0), callback, user_data));
  if (!adapter->start()) return nullptr;
  return adapter.release();
}

int ulv_plugin_feed_udp(void* instance, const ulv_udp_packet_v1* packet) {
  if (instance == nullptr || packet == nullptr || packet->struct_size < sizeof(ulv_udp_packet_v1)) return -1;
  return static_cast<RoboSenseAdapter*>(instance)->feed(packet);
}

int ulv_plugin_feed_udp_batch(void* instance, const ulv_udp_packet_v1* packets, size_t packet_count) {
  if (instance == nullptr || (packets == nullptr && packet_count != 0)) return -1;
  RoboSenseAdapter* adapter = static_cast<RoboSenseAdapter*>(instance);
  int result = 0;
  for (size_t index = 0; index < packet_count; index++) {
    if (packets[index].struct_size < sizeof(ulv_udp_packet_v1)) return -1;
    const int packet_result = adapter->feed(&packets[index]);
    if (packet_result != 0) result = packet_result;
  }
  return result;
}

int ulv_plugin_feed_udp_payload_batch(void* instance, const char* /*source_ip*/,
                                      uint16_t /*source_port*/, uint16_t /*destination_port*/,
                                      uint64_t receive_timestamp_ns, const uint8_t* payloads,
                                      size_t payload_size, size_t packet_count) {
  if (instance == nullptr || (payloads == nullptr && packet_count != 0) || payload_size == 0) return -1;
  RoboSenseAdapter* adapter = static_cast<RoboSenseAdapter*>(instance);
  int result = 0;
  for (size_t index = 0; index < packet_count; index++) {
    const int packet_result = adapter->feed_payload(
        payloads + index * payload_size, payload_size, receive_timestamp_ns);
    if (packet_result != 0) result = packet_result;
  }
  return result;
}

void ulv_plugin_destroy(void* instance) { delete static_cast<RoboSenseAdapter*>(instance); }

}  // extern "C"
