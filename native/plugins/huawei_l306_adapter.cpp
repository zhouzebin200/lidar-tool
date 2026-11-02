// Huawei L306 / D3 decoder for the 1021-byte UDP protocol used by demo_ws_192.

#include "ulv_decoder_plugin.h"
#include "huawei_l306_calibration.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr size_t kPacketSize = 1021;
constexpr size_t kLutCount = 4;
constexpr size_t kHorizontalCorrectionCount = 1500;
constexpr size_t kChannels = 192;
constexpr size_t kLutIndexOffset = 23;
constexpr size_t kHorizontalCorrectionIndexOffset = 25;
constexpr size_t kBlockHeaderOffset = 53;
constexpr size_t kAzimuthOffset = 55;
constexpr size_t kPointOffset = 57;
constexpr size_t kPointStride = 5;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDistanceScale = 0.004f;
constexpr float kMinDistance = 0.2f;
constexpr float kMaxDistance = 200.0f;

uint16_t big_u16(const uint8_t* source) {
  return (static_cast<uint16_t>(source[0]) << 8u) |
         static_cast<uint16_t>(source[1]);
}

int16_t signed_big_i16(const uint8_t* source) {
  return static_cast<int16_t>(big_u16(source));
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

bool is_l306_packet(const uint8_t* data, size_t size) {
  return data != nullptr && size == kPacketSize &&
         data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0 &&
         data[kLutIndexOffset] < kLutCount &&
         big_u16(data + kHorizontalCorrectionIndexOffset) < kHorizontalCorrectionCount &&
         big_u16(data + kHorizontalCorrectionIndexOffset + 2) < kHorizontalCorrectionCount &&
         data[kBlockHeaderOffset] == 0xff && data[kBlockHeaderOffset + 1] == 0xdd &&
         data[1017] == 0 && data[1018] == 0 && data[1019] == 0 && data[1020] == 0;
}

const unsigned char* horizontal_table(size_t lut_index) {
  static const unsigned char* tables[kLutCount] = {
      FovVerticalAngleOffset1, FovVerticalAngleOffset2,
      FovVerticalAngleOffset3, FovVerticalAngleOffset4};
  return tables[lut_index];
}

const unsigned char* vertical_table(size_t lut_index) {
  static const unsigned char* tables[kLutCount] = {
      FovHorizontalAngleOffset1, FovHorizontalAngleOffset2,
      FovHorizontalAngleOffset3, FovHorizontalAngleOffset4};
  return tables[lut_index];
}

float corrected_azimuth_degrees(const uint8_t* data, size_t lut_index,
                                size_t correction_index) {
  const float raw = static_cast<float>(big_u16(data + kAzimuthOffset)) / 256.0f;
  const float correction = static_cast<float>(signed_big_i16(
      horizontal_table(lut_index) + correction_index * 2)) / 1000.0f;
  float corrected = raw + correction;
  if (corrected > 120.0f) corrected -= 256.0f;
  if (corrected < 0.0f) corrected += 360.0f;
  if (corrected >= 360.0f) corrected -= 360.0f;
  return corrected;
}

class HuaweiL306Adapter {
 public:
  HuaweiL306Adapter(uint32_t device_id, ulv_frame_callback_v1 callback, void* user_data)
      : device_id_(device_id), callback_(callback), user_data_(user_data) {
    points_.reserve(120000);
    for (size_t lut = 0; lut < kLutCount; ++lut) {
      for (size_t ring = 0; ring < kChannels; ++ring) {
        const float base_degrees = static_cast<float>(
            signed_big_i16(FovHorizontalAngleData + ring * 2)) / 256.0f;
        const float correction_degrees = static_cast<float>(
            signed_big_i16(vertical_table(lut) + ring * 2)) / 1000.0f;
        const float elevation = (base_degrees + correction_degrees) * kPi / 180.0f;
        vertical_sin_[lut][ring] = std::sin(elevation);
        vertical_cos_[lut][ring] = std::cos(elevation);
      }
    }
  }

  bool valid() const { return callback_ != nullptr; }

  int feed(const ulv_udp_packet_v1* packet) {
    if (packet == nullptr || !is_l306_packet(packet->payload, packet->payload_size)) return -1;
    const uint8_t* data = packet->payload;
    const size_t lut_index = data[kLutIndexOffset];
    const size_t correction_index = big_u16(data + kHorizontalCorrectionIndexOffset);
    if (lut_index >= kLutCount || correction_index >= kHorizontalCorrectionCount) return -1;

    const float angle_degrees = corrected_azimuth_degrees(data, lut_index, correction_index);
    const bool boundary = have_angle_ && previous_angle_ - angle_degrees > 10.0f &&
                          points_.size() > 3000;
    if (boundary) emit_frame(packet->receive_timestamp_ns);

    const float horizontal = angle_degrees * kPi / 180.0f;
    const float sin_h = std::sin(horizontal);
    const float cos_h = std::cos(horizontal);
    for (size_t ring = 0; ring < kChannels; ++ring) {
      const size_t offset = kPointOffset + ring * kPointStride;
      const uint16_t raw_range = big_u16(data + offset);
      const float distance = static_cast<float>(raw_range) * kDistanceScale;
      if (distance < kMinDistance || distance > kMaxDistance) continue;

      const float planar = distance * vertical_cos_[lut_index][ring];
      ulv_point_v1 point{};
      point.x = planar * cos_h;
      point.y = planar * sin_h;
      point.z = distance * vertical_sin_[lut_index][ring];
      point.intensity = static_cast<float>(data[offset + 4]);
      point.ring = static_cast<uint16_t>(ring);
      points_.push_back(point);
    }
    previous_angle_ = angle_degrees;
    have_angle_ = true;
    return 0;
  }

  const char* status_json() const {
    return "{\"state\":\"calibrated\",\"label\":\"Huawei L306 demo_ws_192 embedded calibration LUT\"}";
  }

 private:
  void emit_frame(uint64_t timestamp_ns) {
    if (points_.empty()) return;
    ulv_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.device_id = device_id_;
    frame.sequence = sequence_.fetch_add(1);
    frame.timestamp_ns = timestamp_ns;
    frame.vendor = "Huawei";
    frame.model = "L306";
    frame.points = points_.data();
    frame.point_count = points_.size();
    callback_(&frame, user_data_);
    points_.clear();
  }

  uint32_t device_id_ = 0;
  ulv_frame_callback_v1 callback_ = nullptr;
  void* user_data_ = nullptr;
  std::vector<ulv_point_v1> points_;
  float vertical_sin_[kLutCount][kChannels]{};
  float vertical_cos_[kLutCount][kChannels]{};
  float previous_angle_ = 0.0f;
  bool have_angle_ = false;
  std::atomic<uint64_t> sequence_{0};
};

}  // namespace

extern "C" {

uint32_t ulv_plugin_abi_version(void) { return ULV_PLUGIN_ABI_VERSION; }

const char* ulv_plugin_manifest_json(void) {
  return "{\"name\":\"Huawei L306/D3 calibrated adapter\",\"vendor\":\"Huawei\",\"abi\":2}";
}

void* ulv_plugin_create(const char* config_json, ulv_frame_callback_v1 callback, void* user_data) {
  try {
    auto* adapter = new HuaweiL306Adapter(json_u32(config_json, "device_id", 0), callback, user_data);
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
    return static_cast<HuaweiL306Adapter*>(instance)->feed(packet);
  } catch (...) {
    return -2;
  }
}

int ulv_plugin_feed_udp_batch(void* instance, const ulv_udp_packet_v1* packets, size_t packet_count) {
  if (instance == nullptr || packets == nullptr) return -1;
  int result = 0;
  auto* adapter = static_cast<HuaweiL306Adapter*>(instance);
  for (size_t index = 0; index < packet_count; ++index) {
    const int current = adapter->feed(&packets[index]);
    if (current != 0) result = current;
  }
  return result;
}

void ulv_plugin_destroy(void* instance) { delete static_cast<HuaweiL306Adapter*>(instance); }

const char* ulv_plugin_instance_status_json(void* instance) {
  if (instance == nullptr) return "{\"state\":\"error\",\"label\":\"decoder unavailable\"}";
  return static_cast<HuaweiL306Adapter*>(instance)->status_json();
}

}  // extern "C"
