// Lightweight Velodyne packet decoder for Universal LiDAR Viewer.
//
// The packet layout, calibration schema, timing constants and coordinate
// conversion follow ros-drivers/velodyne (BSD-2-Clause).  The ROS/PCL layers
// are intentionally not linked so the viewer remains a portable executable.

#include "ulv_decoder_plugin.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t kPacketSize = 1206;
constexpr int kBlocksPerPacket = 12;
constexpr int kBlockSize = 100;
constexpr int kScansPerBlock = 32;
constexpr uint16_t kUpperBank = 0xeeff;
constexpr uint16_t kLowerBank = 0xddff;
constexpr uint16_t kBank3 = 0xccff;
constexpr uint16_t kBank4 = 0xbbff;
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kMaximumFramePoints = 180000;

uint16_t little_u16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
         (static_cast<uint16_t>(source[1]) << 8u);
}

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

std::string join_path(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  const char last = directory.back();
  return directory + ((last == '/' || last == '\\') ? "" : "/") + name;
}

std::string model_from_packet(const uint8_t* data, size_t size) {
  if (data == nullptr || size != kPacketSize) return {};
  const uint16_t first_header = little_u16(data);
  if (first_header != kUpperBank && first_header != kLowerBank &&
      first_header != kBank3 && first_header != kBank4) return {};
  switch (data[1205]) {
    case 0x21: return "HDL-32E";
    case 0x22: return "VLP-16";
    case 0x23: return "VLP-16 Puck LITE";
    case 0x24: return "VLP-16 Hi-Res";
    case 0x28: return "VLP-32C";
    case 0x40: return "HDL-64E S3";
    case 0xa0:
    case 0xa1: return "VLS-128";
    default: break;
  }
  bool lower = false;
  bool four_banks = false;
  for (int block = 0; block < kBlocksPerPacket; block++) {
    const uint16_t header = little_u16(data + block * kBlockSize);
    lower = lower || header == kLowerBank;
    four_banks = four_banks || header == kBank3 || header == kBank4;
  }
  if (four_banks) return "VLS-128";
  if (lower) return "HDL-64E";
  return {};
}

std::string calibration_name_for(const std::string& model) {
  if (model == "VLP-16" || model == "VLP-16 Puck LITE") return "VLP16db.yaml";
  if (model == "VLP-16 Hi-Res") return "VLP16_hires_db.yaml";
  if (model == "VLP-32C") return "VeloView-VLP-32C.yaml";
  if (model == "HDL-32E") return "32db.yaml";
  if (model == "HDL-64E S3") return "64e_s3-xiesc.yaml";
  if (model == "HDL-64E") return "64e_utexas.yaml";
  if (model == "VLS-128") return "VLS128.yaml";
  return {};
}

bool exact_model(const std::string& model) {
  return !calibration_name_for(model).empty();
}

struct LaserCorrection {
  float rot = 0.0f;
  float vert = 0.0f;
  float distance = 0.0f;
  float distance_x = 0.0f;
  float distance_y = 0.0f;
  float vertical_offset = 0.0f;
  float horizontal_offset = 0.0f;
  float focal_distance = 0.0f;
  float focal_slope = 0.0f;
  float min_intensity = 0.0f;
  float max_intensity = 255.0f;
  bool two_point = false;
  uint16_t ring = 0;
  float cos_rot = 1.0f;
  float sin_rot = 0.0f;
  float cos_vert = 1.0f;
  float sin_vert = 0.0f;
};

bool number_value(const std::string& source, const std::string& key, float* result) {
  const std::string marker = key + ":";
  size_t position = source.find(marker);
  if (position == std::string::npos) return false;
  position += marker.size();
  while (position < source.size() && std::isspace(static_cast<unsigned char>(source[position]))) position++;
  size_t end = position;
  while (end < source.size()) {
    const char value = source[end];
    if (value == ',' || value == '}' || value == '\r' || value == '\n') break;
    end++;
  }
  try {
    *result = std::stof(source.substr(position, end - position));
    return true;
  } catch (...) {
    return false;
  }
}

bool integer_value(const std::string& source, const std::string& key, int* result) {
  float parsed = 0.0f;
  if (!number_value(source, key, &parsed)) return false;
  *result = static_cast<int>(parsed);
  return true;
}

bool bool_value(const std::string& source, const std::string& key, bool fallback) {
  const std::string marker = key + ":";
  size_t position = source.find(marker);
  if (position == std::string::npos) return fallback;
  position += marker.size();
  while (position < source.size() && std::isspace(static_cast<unsigned char>(source[position]))) position++;
  return source.compare(position, 4, "true") == 0;
}

bool read_file(const std::string& path, std::string* contents) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  std::ostringstream stream;
  stream << input.rdbuf();
  *contents = stream.str();
  return !contents->empty();
}

bool load_calibration_yaml(const std::string& path, std::vector<LaserCorrection>* corrections,
                           float* distance_resolution, std::string* error) {
  std::string source;
  if (!read_file(path, &source)) {
    *error = "cannot open calibration file: " + path;
    return false;
  }
  int expected_lasers = 0;
  if (!integer_value(source, "num_lasers", &expected_lasers) || expected_lasers <= 0 || expected_lasers > 256) {
    *error = "invalid num_lasers in calibration";
    return false;
  }
  if (!number_value(source, "distance_resolution", distance_resolution)) *distance_resolution = 0.002f;
  corrections->assign(static_cast<size_t>(expected_lasers), LaserCorrection{});
  std::vector<bool> present(static_cast<size_t>(expected_lasers), false);

  size_t position = source.find("lasers:");
  if (position == std::string::npos) {
    *error = "calibration has no lasers list";
    return false;
  }
  position = source.find('\n', position);
  while (position != std::string::npos && position < source.size()) {
    size_t item_start = position + 1;
    if (item_start >= source.size() || source[item_start] != '-') {
      position = source.find("\n-", item_start);
      continue;
    }
    size_t item_end = source.find("\n-", item_start + 1);
    const size_t root_end = source.find("\nnum_lasers:", item_start + 1);
    if (item_end == std::string::npos || (root_end != std::string::npos && root_end < item_end)) item_end = root_end;
    if (item_end == std::string::npos) item_end = source.size();
    const std::string item = source.substr(item_start, item_end - item_start);
    int laser_id = -1;
    if (integer_value(item, "laser_id", &laser_id) && laser_id >= 0 && laser_id < expected_lasers) {
      LaserCorrection correction;
      number_value(item, "rot_correction", &correction.rot);
      number_value(item, "vert_correction", &correction.vert);
      number_value(item, "dist_correction", &correction.distance);
      number_value(item, "dist_correction_x", &correction.distance_x);
      number_value(item, "dist_correction_y", &correction.distance_y);
      number_value(item, "vert_offset_correction", &correction.vertical_offset);
      number_value(item, "horiz_offset_correction", &correction.horizontal_offset);
      number_value(item, "focal_distance", &correction.focal_distance);
      number_value(item, "focal_slope", &correction.focal_slope);
      number_value(item, "min_intensity", &correction.min_intensity);
      number_value(item, "max_intensity", &correction.max_intensity);
      correction.two_point = bool_value(item, "two_pt_correction_available", false);
      correction.cos_rot = std::cos(correction.rot);
      correction.sin_rot = std::sin(correction.rot);
      correction.cos_vert = std::cos(correction.vert);
      correction.sin_vert = std::sin(correction.vert);
      (*corrections)[static_cast<size_t>(laser_id)] = correction;
      present[static_cast<size_t>(laser_id)] = true;
    }
    if (item_end >= source.size() || item_end == root_end) break;
    position = item_end;
  }
  if (std::find(present.begin(), present.end(), false) != present.end()) {
    *error = "calibration laser list is incomplete";
    return false;
  }

  std::vector<std::pair<float, size_t>> vertical_order;
  vertical_order.reserve(corrections->size());
  for (size_t laser = 0; laser < corrections->size(); laser++) {
    vertical_order.emplace_back((*corrections)[laser].vert, laser);
  }
  std::stable_sort(vertical_order.begin(), vertical_order.end());
  for (size_t ring = 0; ring < vertical_order.size(); ring++) {
    (*corrections)[vertical_order[ring].second].ring = static_cast<uint16_t>(ring);
  }
  return true;
}

class VelodyneAdapter {
 public:
  VelodyneAdapter(std::string model, std::string correction_file, std::string correction_directory,
                  uint32_t device_id, ulv_frame_callback_v1 callback, void* user_data)
      : model_(std::move(model)), manual_correction_file_(std::move(correction_file)),
        correction_directory_(std::move(correction_directory)), device_id_(device_id),
        callback_(callback), user_data_(user_data) {
    if (exact_model(model_)) load_calibration();
  }

  bool valid() const { return callback_ != nullptr; }

  int feed(const ulv_udp_packet_v1* packet) {
    if (packet == nullptr || packet->payload == nullptr) return -1;
    if (packet->payload_size != kPacketSize) return 0;  // Position/NMEA packets are discovery-only.
    const std::string detected_model = model_from_packet(packet->payload, packet->payload_size);
    if (!detected_model.empty() && detected_model != model_) {
      model_ = detected_model;
      calibration_loaded_ = false;
      corrections_.clear();
      load_calibration();
    }
    if (!calibration_loaded_ && exact_model(model_)) load_calibration();
    if (!calibration_loaded_) return 1;

    const uint16_t packet_azimuth = little_u16(packet->payload + 2);
    if (have_azimuth_ && last_azimuth_ > packet_azimuth + 18000u && !points_.empty()) {
      emit_frame(packet->receive_timestamp_ns);
    }
    have_azimuth_ = true;
    last_azimuth_ = packet_azimuth;
    if (model_ == "VLS-128") {
      decode_vls128(packet->payload);
    } else if (corrections_.size() == 16) {
      decode_vlp16(packet->payload);
    } else {
      decode_generic(packet->payload);
    }
    return 0;
  }

  const char* status_json() {
    std::lock_guard<std::mutex> guard(status_mutex_);
    std::ostringstream output;
    output << "{\"state\":\"" << (calibration_loaded_ ? "ready" : (calibration_error_.empty() ? "pending" : "error"))
           << "\",\"source\":\"" << (manual_correction_file_.empty() ? "ros_driver_default" : "manual")
           << "\",\"model\":\"" << model_ << "\",\"file\":\"" << calibration_file_
           << "\",\"label\":\"";
    if (calibration_loaded_) {
      output << "ros-drivers/velodyne 校准已加载";
      if (model_.find("HDL-64E") != std::string::npos && manual_correction_file_.empty()) {
        output << "（HDL-64E 建议换用设备专属标定）";
      }
    } else if (!calibration_error_.empty()) {
      output << calibration_error_;
    } else {
      output << "等待 Velodyne 数据包确认型号";
    }
    output << "\"}";
    status_json_ = output.str();
    return status_json_.c_str();
  }

 private:
  void load_calibration() {
    const std::string default_name = calibration_name_for(model_);
    if (default_name.empty()) return;
    calibration_file_ = manual_correction_file_.empty()
                            ? join_path(correction_directory_, default_name)
                            : manual_correction_file_;
    std::string error;
    std::vector<LaserCorrection> parsed;
    float resolution = 0.002f;
    const bool loaded = load_calibration_yaml(calibration_file_, &parsed, &resolution, &error);
    std::lock_guard<std::mutex> guard(status_mutex_);
    calibration_loaded_ = loaded;
    calibration_error_ = loaded ? std::string() : error;
    if (loaded) {
      corrections_ = std::move(parsed);
      distance_resolution_ = resolution;
    }
  }

  void append_point(uint16_t laser, uint16_t azimuth, uint16_t raw_distance, uint8_t raw_intensity,
                    uint8_t return_type) {
    if (raw_distance == 0 || laser >= corrections_.size()) return;
    const LaserCorrection& correction = corrections_[laser];
    float distance = raw_distance * distance_resolution_ + correction.distance;
    if (!std::isfinite(distance) || distance <= 0.05f || distance > 500.0f) return;
    const float angle = static_cast<float>(azimuth) * (kPi / 18000.0f);
    const float cos_angle = std::cos(angle);
    const float sin_angle = std::sin(angle);
    const float cos_rot = cos_angle * correction.cos_rot + sin_angle * correction.sin_rot;
    const float sin_rot = sin_angle * correction.cos_rot - cos_angle * correction.sin_rot;
    float xy = distance * correction.cos_vert - correction.vertical_offset * correction.sin_vert;
    const float xx = std::fabs(xy * sin_rot - correction.horizontal_offset * cos_rot);
    const float yy = std::fabs(xy * cos_rot + correction.horizontal_offset * sin_rot);
    float correction_x = 0.0f;
    float correction_y = 0.0f;
    if (correction.two_point) {
      correction_x = (correction.distance - correction.distance_x) * (xx - 2.4f) / (25.04f - 2.4f) +
                     correction.distance_x - correction.distance;
      correction_y = (correction.distance - correction.distance_y) * (yy - 1.93f) / (25.04f - 1.93f) +
                     correction.distance_y - correction.distance;
    }
    const float distance_x = distance + correction_x;
    xy = distance_x * correction.cos_vert - correction.vertical_offset * correction.sin_vert;
    const float temporary_x = xy * sin_rot - correction.horizontal_offset * cos_rot;
    const float distance_y = distance + correction_y;
    xy = distance_y * correction.cos_vert - correction.vertical_offset * correction.sin_vert;
    const float temporary_y = xy * cos_rot + correction.horizontal_offset * sin_rot;
    const float z = distance_y * correction.sin_vert + correction.vertical_offset * correction.cos_vert;

    float intensity = static_cast<float>(raw_intensity);
    const float focal_offset = 256.0f * std::pow(1.0f - correction.focal_distance / 13100.0f, 2.0f);
    intensity += correction.focal_slope *
                 std::fabs(focal_offset - 256.0f * std::pow((1.0f - distance) / 65535.0f, 2.0f));
    intensity = std::max(correction.min_intensity, std::min(correction.max_intensity, intensity));
    ulv_point_v1 point{};
    point.x = temporary_y;
    point.y = -temporary_x;
    point.z = z;
    point.intensity = std::max(0.0f, std::min(1.0f, intensity / 255.0f));
    point.ring = correction.ring;
    point.return_type = return_type;
    points_.push_back(point);
  }

  void decode_generic(const uint8_t* data) {
    const uint8_t return_type = data[1204];
    for (int block = 0; block < kBlocksPerPacket; block++) {
      const uint8_t* raw_block = data + block * kBlockSize;
      const uint16_t header = little_u16(raw_block);
      if (header != kUpperBank && header != kLowerBank) continue;
      const uint16_t azimuth = little_u16(raw_block + 2) % 36000u;
      const uint16_t bank = header == kLowerBank ? 32u : 0u;
      for (uint16_t scan = 0; scan < kScansPerBlock; scan++) {
        const uint8_t* raw = raw_block + 4 + scan * 3;
        append_point(static_cast<uint16_t>(bank + scan), azimuth, little_u16(raw), raw[2], return_type);
      }
    }
  }

  void decode_vlp16(const uint8_t* data) {
    const uint8_t return_type = data[1204];
    float last_difference = 0.0f;
    for (int block = 0; block < kBlocksPerPacket; block++) {
      const uint8_t* raw_block = data + block * kBlockSize;
      if (little_u16(raw_block) != kUpperBank) return;
      const uint16_t azimuth = little_u16(raw_block + 2) % 36000u;
      float difference = last_difference;
      if (block + 1 < kBlocksPerPacket) {
        const uint16_t next = little_u16(data + (block + 1) * kBlockSize + 2) % 36000u;
        difference = static_cast<float>((36000u + next - azimuth) % 36000u);
        if (difference > 1000.0f && last_difference > 0.0f) difference = last_difference;
        if (difference <= 1000.0f) last_difference = difference;
      }
      for (int firing = 0; firing < 2; firing++) {
        for (uint16_t laser = 0; laser < 16; laser++) {
          const int scan = firing * 16 + laser;
          const uint8_t* raw = raw_block + 4 + scan * 3;
          const float fraction = (laser * 2.304f + firing * 55.296f) / 110.592f;
          const uint16_t corrected = static_cast<uint16_t>(std::lround(azimuth + difference * fraction)) % 36000u;
          append_point(laser, corrected, little_u16(raw), raw[2], return_type);
        }
      }
    }
  }

  void decode_vls128(const uint8_t* data) {
    const uint8_t return_type = data[1204];
    const bool dual_return = return_type == 0x39;
    const int block_limit = kBlocksPerPacket - (dual_return ? 4 : 0);
    for (int block = 0; block < block_limit; block++) {
      const uint8_t* raw_block = data + block * kBlockSize;
      const uint16_t header = little_u16(raw_block);
      uint16_t bank = 0;
      if (header == kUpperBank) bank = 0;
      else if (header == kLowerBank) bank = 32;
      else if (header == kBank3) bank = 64;
      else if (header == kBank4) bank = 96;
      else return;
      const int group = (block / 4) * 4;
      const uint16_t azimuth = little_u16(data + group * kBlockSize + 2) % 36000u;
      uint16_t next_azimuth = azimuth;
      if (!dual_return && group + 4 < kBlocksPerPacket) {
        next_azimuth = little_u16(data + (group + 4) * kBlockSize + 2) % 36000u;
      }
      const float difference = dual_return ? 0.0f : static_cast<float>((36000u + next_azimuth - azimuth) % 36000u);
      for (uint16_t scan = 0; scan < 32; scan++) {
        const uint16_t laser = static_cast<uint16_t>(bank + scan);
        const uint8_t firing_order = static_cast<uint8_t>(laser / 8);
        const float fraction = (2.665f / 53.3f) * static_cast<float>(firing_order + firing_order / 8);
        const uint16_t corrected = static_cast<uint16_t>(std::lround(azimuth + difference * fraction)) % 36000u;
        const uint8_t* raw = raw_block + 4 + scan * 3;
        append_point(laser, corrected, little_u16(raw), raw[2], return_type);
      }
    }
  }

  void emit_frame(uint64_t timestamp_ns) {
    if (points_.empty()) return;
    const std::vector<ulv_point_v1>* output = &points_;
    sampled_.clear();
    if (points_.size() > kMaximumFramePoints) {
      const size_t stride = (points_.size() + kMaximumFramePoints - 1) / kMaximumFramePoints;
      sampled_.reserve((points_.size() + stride - 1) / stride);
      for (size_t index = 0; index < points_.size(); index += stride) sampled_.push_back(points_[index]);
      output = &sampled_;
    }
    ulv_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.device_id = device_id_;
    frame.sequence = sequence_.fetch_add(1);
    frame.timestamp_ns = timestamp_ns;
    frame.vendor = "Velodyne";
    frame.model = model_.c_str();
    frame.points = output->data();
    frame.point_count = output->size();
    callback_(&frame, user_data_);
    points_.clear();
  }

  std::string model_;
  std::string manual_correction_file_;
  std::string correction_directory_;
  std::string calibration_file_;
  std::string calibration_error_;
  std::string status_json_;
  uint32_t device_id_ = 0;
  ulv_frame_callback_v1 callback_ = nullptr;
  void* user_data_ = nullptr;
  std::vector<LaserCorrection> corrections_;
  std::vector<ulv_point_v1> points_;
  std::vector<ulv_point_v1> sampled_;
  float distance_resolution_ = 0.002f;
  bool calibration_loaded_ = false;
  bool have_azimuth_ = false;
  uint16_t last_azimuth_ = 0;
  std::atomic<uint64_t> sequence_{0};
  std::mutex status_mutex_;
};

}  // namespace

extern "C" {

uint32_t ulv_plugin_abi_version(void) { return ULV_PLUGIN_ABI_VERSION; }

const char* ulv_plugin_manifest_json(void) {
  return "{\"name\":\"Velodyne ros-drivers raw adapter\",\"vendor\":\"Velodyne\",\"abi\":2}";
}

void* ulv_plugin_create(const char* config_json, ulv_frame_callback_v1 callback, void* user_data) {
  try {
    VelodyneAdapter* adapter = new VelodyneAdapter(
        json_string(config_json, "model"), json_string(config_json, "correction_file"),
        json_string(config_json, "default_correction_directory"),
        json_u32(config_json, "device_id", 0), callback, user_data);
    if (!adapter->valid()) {
      delete adapter;
      return nullptr;
    }
    return adapter;
  } catch (...) {
    return nullptr;
  }
}

int ulv_plugin_feed_udp(void* instance, const ulv_udp_packet_v1* packet) {
  if (instance == nullptr || packet == nullptr || packet->struct_size < sizeof(ulv_udp_packet_v1)) return -1;
  try {
    return static_cast<VelodyneAdapter*>(instance)->feed(packet);
  } catch (...) {
    return -2;
  }
}

void ulv_plugin_destroy(void* instance) { delete static_cast<VelodyneAdapter*>(instance); }

const char* ulv_plugin_instance_status_json(void* instance) {
  if (instance == nullptr) return "{\"state\":\"error\",\"label\":\"decoder unavailable\"}";
  return static_cast<VelodyneAdapter*>(instance)->status_json();
}

}  // extern "C"
