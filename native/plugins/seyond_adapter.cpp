#include "ulv_decoder_plugin.h"

#include "sdk_client/inno_lidar_packet_v1_adapt.h"
#include "sdk_common/inno_lidar_api.h"
#include "sdk_common/inno_lidar_packet_utils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

static_assert(sizeof(InnoCommonHeader) == 26, "Seyond common header must match the wire ABI");
static_assert(sizeof(InnoDataPacket) == 70, "Seyond data header must match the wire ABI");

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
  try {
    return static_cast<uint32_t>(std::stoul(source.substr(position + 1)));
  } catch (...) {
    return fallback;
  }
}

const char* model_for_type(uint8_t lidar_type) {
  switch (lidar_type) {
    case INNO_LIDAR_TYPE_FALCON: return "Falcon-K1";
    case INNO_LIDAR_TYPE_ROBINW: return "Robin-W";
    case INNO_LIDAR_TYPE_ROBINE: return "Robin-E";
    case INNO_LIDAR_TYPE_FALCONK2: return "Falcon-K2";
    case INNO_LIDAR_TYPE_FALCONIII: return "Falcon-K24";
    case INNO_LIDAR_TYPE_ROBINELITE: return "Robin-E1X / ELITE";
    case INNO_LIDAR_TYPE_ROBINE2X: return "Robin-E2X";
    case INNO_LIDAR_TYPE_HB: return "HB-D1";
    case INNO_LIDAR_TYPE_ROBINE2: return "Robin-E2";
    default: return "Seyond Inno family";
  }
}

class SeyondAdapter {
 public:
  SeyondAdapter(const char* config_json, ulv_frame_callback_v1 callback, void* user_data)
      : device_id_(json_u32(config_json, "device_id", 0)),
        device_ip_(json_string(config_json, "device_ip")),
        cache_file_(json_string(config_json, "correction_cache_file")),
        fallback_file_(json_string(config_json, "fallback_anglehv_file")),
        control_port_(static_cast<uint16_t>(json_u32(config_json, "seyond_control_port", 8010))),
        callback_(callback), user_data_(user_data) {
    load_angle_table_cache();
    if (angle_table_.empty()) load_angle_table_fallback();
    points_.reserve(500000);
  }

  bool valid() const { return callback_ != nullptr; }

  const char* status_json() const {
    switch (calibration_state_.load()) {
      case 1: return "{\"state\":\"factory\",\"label\":\"Seyond factory AngleHV table\"}";
      case 2: return "{\"state\":\"cache\",\"label\":\"Seyond cached AngleHV table\"}";
      case 3: return "{\"state\":\"missing\",\"label\":\"Seyond AngleHV table required for compact packets\"}";
      case 4: return "{\"state\":\"nominal\",\"label\":\"Seyond Robin-W nominal AngleHV fallback\"}";
      default: return "{\"state\":\"sdk_native\",\"label\":\"Seyond SDK native packet geometry\"}";
    }
  }

  int feed(const ulv_udp_packet_v1* source) {
    if (source == nullptr || source->payload == nullptr ||
        source->payload_size < sizeof(InnoCommonHeader)) return -1;
    const auto* common = reinterpret_cast<const InnoCommonHeader*>(source->payload);
    if (common->version.magic_number != kInnoMagicNumberDataPacket) return 0;

    std::vector<uint8_t> adapted;
    const InnoDataPacket* packet = nullptr;
    if (common->version.major_version == InnoPacketV1Adapt::kInnoProtocolMajorV1) {
      const int front_gap = static_cast<int>(InnoPacketV1Adapt::kMemorryFrontGap);
      adapted.resize(front_gap + source->payload_size);
      std::memcpy(adapted.data() + front_gap, source->payload, source->payload_size);
      char* pointer = reinterpret_cast<char*>(adapted.data() + front_gap);
      int gap = front_gap;
      if (!InnoPacketV1Adapt::check_data_packet_v1_and_convert_packet(
              &pointer, source->payload_size, gap)) return -2;
      packet = reinterpret_cast<const InnoDataPacket*>(pointer);
    } else {
      packet = reinterpret_cast<const InnoDataPacket*>(source->payload);
      if (!InnoDataPacketUtils::check_data_packet(*packet, source->payload_size)) return -3;
    }

    model_ = model_for_type(packet->common.lidar_type);
    if (CHECK_ANGLEHV_TABLE_DATA(packet->type)) {
      save_angle_table(*packet);
      return 0;
    }
    if (!CHECK_XYZ_POINTCLOUD_DATA(packet->type) && !CHECK_SPHERE_POINTCLOUD_DATA(packet->type)) return 0;

    InnoDataPacket* converted = nullptr;
    const InnoDataPacket* xyz_packet = packet;
    if (CHECK_SPHERE_POINTCLOUD_DATA(packet->type)) {
      const char* angle_table = nullptr;
      if (CHECK_CO_SPHERE_POINTCLOUD_DATA(packet->type)) {
        // A model-level fallback makes an isolated PCAP usable, but an online
        // lidar's own table always wins and is persisted by device identity.
        if (calibration_state_.load() == 4 && !angle_fetch_attempted_) {
          angle_fetch_attempted_ = true;
          fetch_angle_table(source->destination_port);
        } else if (angle_table_.empty() && !angle_fetch_attempted_) {
          angle_fetch_attempted_ = true;
          fetch_angle_table(source->destination_port);
        }
        if (angle_table_.empty()) {
          calibration_state_.store(3);
          return -4;
        }
        const auto* table_packet = reinterpret_cast<const InnoDataPacket*>(angle_table_.data());
        angle_table = reinterpret_cast<const char*>(
            reinterpret_cast<const InnoAngleHVTable*>(table_packet->payload)->table);
      }
      converted = InnoDataPacketUtils::convert_to_xyz_pointcloud_malloced(
          *packet, nullptr, angle_table);
      if (converted == nullptr) return -5;
      xyz_packet = converted;
    }

    append_packet(*xyz_packet);
    if (converted != nullptr) std::free(converted);
    return 0;
  }

 private:
  void append_packet(const InnoDataPacket& packet) {
    if (current_frame_ != kNoFrame && packet.idx != current_frame_) emit_frame();
    if (current_frame_ == kNoFrame) {
      current_frame_ = packet.idx;
      frame_timestamp_ns_ = static_cast<uint64_t>(packet.common.ts_start_us * 1000.0);
    }
    if (packet.type == INNO_ITEM_TYPE_XYZ_POINTCLOUD) {
      const auto* source = reinterpret_cast<const InnoXyzPoint*>(packet.payload);
      for (uint32_t i = 0; i < packet.item_number; ++i) {
        if (source[i].radius <= 0 || !finite_xyz(source[i].x, source[i].y, source[i].z)) continue;
        ulv_point_v1 point{};
        to_vehicle(source[i].x, source[i].y, source[i].z, point);
        point.intensity = static_cast<float>(source[i].refl) / 255.0f;
        point.ring = source[i].ring_id ? source[i].ring_id : source[i].scan_id;
        point.return_type = source[i].is_2nd_return ? 2 : 1;
        point.flags = static_cast<uint8_t>(source[i].type);
        points_.push_back(point);
      }
    } else if (CHECK_EN_XYZ_POINTCLOUD_DATA(packet.type)) {
      const auto* source = reinterpret_cast<const InnoEnXyzPoint*>(packet.payload);
      const float divisor = packet.common.lidar_type == INNO_LIDAR_TYPE_ROBINW ||
          packet.common.lidar_type == INNO_LIDAR_TYPE_ROBINE ||
          packet.common.lidar_type == INNO_LIDAR_TYPE_ROBINELITE ||
          packet.common.lidar_type == INNO_LIDAR_TYPE_ROBINE2X ||
          packet.common.lidar_type == INNO_LIDAR_TYPE_ROBINE2 ? 4095.0f : 65535.0f;
      for (uint32_t i = 0; i < packet.item_number; ++i) {
        if (source[i].radius <= 0 || !finite_xyz(source[i].x, source[i].y, source[i].z)) continue;
        ulv_point_v1 point{};
        to_vehicle(source[i].x, source[i].y, source[i].z, point);
        const uint16_t strength = packet.use_reflectance ? source[i].reflectance : source[i].intensity;
        point.intensity = static_cast<float>(strength) / divisor;
        point.ring = source[i].scan_id;
        point.return_type = source[i].is_2nd_return ? 2 : 1;
        point.flags = static_cast<uint8_t>(packet.confidence_level);
        points_.push_back(point);
      }
    }
    if (packet.is_last_sub_frame && packet.is_last_sequence) emit_frame();
  }

  static bool finite_xyz(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }

  static void to_vehicle(float x, float y, float z, ulv_point_v1& point) {
    // Seyond SDK sensor coordinates are X-up, Y-right, Z-forward. ULV uses
    // the automotive convention X-forward, Y-left, Z-up.
    point.x = z;
    point.y = -y;
    point.z = x;
  }

  void emit_frame() {
    if (current_frame_ == kNoFrame || points_.empty()) {
      current_frame_ = kNoFrame;
      points_.clear();
      return;
    }
    const std::vector<ulv_point_v1>* output = &points_;
    if (points_.size() > kMaxDisplayPoints) {
      const size_t stride = (points_.size() + kMaxDisplayPoints - 1) / kMaxDisplayPoints;
      display_points_.clear();
      display_points_.reserve((points_.size() + stride - 1) / stride);
      for (size_t i = 0; i < points_.size(); i += stride) display_points_.push_back(points_[i]);
      output = &display_points_;
    }
    ulv_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.device_id = device_id_;
    frame.sequence = sequence_.fetch_add(1);
    frame.timestamp_ns = frame_timestamp_ns_;
    frame.vendor = "Seyond";
    frame.model = model_.c_str();
    frame.points = output->data();
    frame.point_count = output->size();
    callback_(&frame, user_data_);
    current_frame_ = kNoFrame;
    points_.clear();
  }

  bool fetch_angle_table(uint16_t udp_port) {
    if (device_ip_.empty()) return false;
    const std::string name = "ulv-" + std::to_string(device_id_);
    const int handle = inno_lidar_open_live(name.c_str(), device_ip_.c_str(), control_port_,
                                             INNO_LIDAR_PROTOCOL_PCS_UDP, udp_port);
    if (handle < 0) return false;
    std::vector<uint8_t> buffer(kInnoAngleHVTableMaxSize);
    const int result = inno_lidar_get_anglehv_table(
        handle, reinterpret_cast<InnoDataPacket*>(buffer.data()));
    inno_lidar_close(handle);
    if (result != 0) return false;
    const auto* packet = reinterpret_cast<const InnoDataPacket*>(buffer.data());
    if (packet->common.size > buffer.size() || !CHECK_ANGLEHV_TABLE_DATA(packet->type) ||
        !InnoDataPacketUtils::check_data_packet(*packet, packet->common.size)) return false;
    angle_table_.assign(buffer.begin(), buffer.begin() + packet->common.size);
    calibration_state_.store(1);
    write_angle_table_cache();
    return true;
  }

  void save_angle_table(const InnoDataPacket& packet) {
    if (packet.common.size < sizeof(InnoDataPacket) || packet.common.size > kInnoAngleHVTableMaxSize) return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&packet);
    angle_table_.assign(bytes, bytes + packet.common.size);
    calibration_state_.store(1);
    write_angle_table_cache();
  }

  void load_angle_table_cache() {
    if (load_angle_table_file(cache_file_)) calibration_state_.store(2);
  }

  void load_angle_table_fallback() {
    if (load_angle_table_file(fallback_file_)) calibration_state_.store(4);
  }

  bool load_angle_table_file(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    angle_table_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (angle_table_.size() < sizeof(InnoDataPacket)) {
      angle_table_.clear();
      return false;
    }
    const auto* packet = reinterpret_cast<const InnoDataPacket*>(angle_table_.data());
    if (packet->common.size != angle_table_.size() || !CHECK_ANGLEHV_TABLE_DATA(packet->type) ||
        !InnoDataPacketUtils::check_data_packet(*packet, angle_table_.size())) {
      angle_table_.clear();
      return false;
    }
    return true;
  }

  void write_angle_table_cache() const {
    if (cache_file_.empty() || angle_table_.empty()) return;
    std::ofstream output(cache_file_, std::ios::binary | std::ios::trunc);
    if (output.is_open()) {
      output.write(reinterpret_cast<const char*>(angle_table_.data()),
                   static_cast<std::streamsize>(angle_table_.size()));
    }
  }

  static constexpr uint64_t kNoFrame = ~uint64_t{0};
  static constexpr size_t kMaxDisplayPoints = 180000;
  uint32_t device_id_;
  std::string device_ip_;
  std::string cache_file_;
  std::string fallback_file_;
  uint16_t control_port_;
  ulv_frame_callback_v1 callback_;
  void* user_data_;
  std::string model_ = "Seyond Inno family";
  std::vector<uint8_t> angle_table_;
  bool angle_fetch_attempted_ = false;
  uint64_t current_frame_ = kNoFrame;
  uint64_t frame_timestamp_ns_ = 0;
  std::vector<ulv_point_v1> points_;
  std::vector<ulv_point_v1> display_points_;
  std::atomic<uint64_t> sequence_{0};
  std::atomic<int> calibration_state_{0};
};

}  // namespace

extern "C" {

uint32_t ulv_plugin_abi_version(void) { return ULV_PLUGIN_ABI_VERSION; }

const char* ulv_plugin_manifest_json(void) {
  return "{\"name\":\"Seyond Inno SDK raw adapter\",\"vendor\":\"Seyond\",\"abi\":2}";
}

void* ulv_plugin_create(const char* config_json, ulv_frame_callback_v1 callback, void* user_data) {
  auto* adapter = new SeyondAdapter(config_json, callback, user_data);
  if (!adapter->valid()) {
    delete adapter;
    return nullptr;
  }
  return adapter;
}

int ulv_plugin_feed_udp(void* instance, const ulv_udp_packet_v1* packet) {
  return instance == nullptr ? -1 : static_cast<SeyondAdapter*>(instance)->feed(packet);
}

int ulv_plugin_feed_udp_batch(void* instance, const ulv_udp_packet_v1* packets, size_t packet_count) {
  if (instance == nullptr || packets == nullptr) return -1;
  int result = 0;
  for (size_t i = 0; i < packet_count; ++i) {
    const int current = static_cast<SeyondAdapter*>(instance)->feed(&packets[i]);
    if (current != 0) result = current;
  }
  return result;
}

void ulv_plugin_destroy(void* instance) { delete static_cast<SeyondAdapter*>(instance); }

const char* ulv_plugin_instance_status_json(void* instance) {
  return instance == nullptr
      ? "{\"state\":\"missing\",\"label\":\"decoder instance missing\"}"
      : static_cast<SeyondAdapter*>(instance)->status_json();
}

}  // extern "C"
