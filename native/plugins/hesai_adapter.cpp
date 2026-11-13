#include "ulv_decoder_plugin.h"
#include "ulv_plugin_utils.h"

#include <UdpParser/udp_parser.h>
#include <ptc_client.h>
#include <lidar_types.h>
#include <UdpProtocol/udp_protocol_v4_3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace hesai::lidar;

namespace {

using ulv_plugin_utils::json_string;
using ulv_plugin_utils::json_u32;

class HesaiAdapter {
 public:
  HesaiAdapter(const char* config_json, ulv_frame_callback_v1 callback, void* user_data)
      : device_id_(json_u32(config_json, "device_id", 0)),
        requested_model_(json_string(config_json, "model")),
        device_ip_(json_string(config_json, "device_ip")),
        ptc_port_(static_cast<uint16_t>(json_u32(config_json, "ptc_port", 9347))),
        correction_file_(json_string(config_json, "correction_file")),
        fallback_correction_file_(json_string(config_json, "fallback_correction_file")),
        correction_cache_file_(json_string(config_json, "correction_cache_file")),
        firetimes_file_(json_string(config_json, "firetimes_file")),
        callback_(callback),
        user_data_(user_data) {}

  ~HesaiAdapter() {
    if (alive_) *alive_ = false;
  }

  bool valid() const { return callback_ != nullptr; }

  const char* calibration_status_json() const {
    switch (calibration_status_.load()) {
      case 1: return "{\"state\":\"factory\",\"label\":\"lidar factory calibration\"}";
      case 2: return "{\"state\":\"file\",\"label\":\"manual calibration file\"}";
      case 3: return "{\"state\":\"cache\",\"label\":\"per-device calibration cache\"}";
      case 4:
        return (detected_model_ == "PandarQT" || detected_model_ == "PandarQT64" || detected_model_ == "QT64")
            ? "{\"state\":\"nominal\",\"label\":\"official PandarQT nominal angles (PTC unreachable)\"}"
            : "{\"state\":\"nominal\",\"label\":\"official Pandar40 nominal angles (PTC unreachable)\"}";
      case 5: return "{\"state\":\"missing\",\"label\":\"calibration missing\"}";
      case 6:
        return (detected_model_ == "PandarQT" || detected_model_ == "PandarQT64" || detected_model_ == "QT64")
            ? "{\"state\":\"sdk_default\",\"label\":\"Hesai SDK PandarQT nominal correction\"}"
            : "{\"state\":\"sdk_default\",\"label\":\"Hesai SDK AT128 nominal correction\"}";
      default: return "{\"state\":\"pending\",\"label\":\"waiting for calibration\"}";
    }
  }

  int feed(const ulv_udp_packet_v1* source) {
    if (source == nullptr || source->payload == nullptr || source->payload_size < 6 ||
        source->payload_size > kBufSize) {
      return -1;
    }
    UdpPacket packet(source->payload,
                     static_cast<uint16_t>(source->payload_size),
                     source->receive_timestamp_ns / 1000,
                     0,
                     source->source_port);
    int incoming_field = packet_field(source->payload, source->payload_size);

    if (pending_factory_ready_.load()) {
      apply_pending_factory_correction();
    }

    if (parser_.GetGeneralParser() == nullptr) {
      parser_.DecodePacket(frame_, packet);
      if (parser_.GetGeneralParser() == nullptr) return -2;
      parser_.setFrameRightMemorySpace(frame_);
      detected_model_ = parser_.GetLidarType();
      if (!correction_file_.empty()) load_correction_from_file(correction_file_, 2);
      // Decoding must not wait for TCP/PTC while Windows is still applying
      // the detected IP/VLAN profile. A per-device factory cache is exact;
      // the bundled SDK correction is an immediate first-run fallback. PTC
      // remains available when neither local source can initialize the parser.
      if (!parser_.isSetCorrectionSucc() && !load_correction_from_cache() &&
          !load_sdk_default_correction() && !load_correction_from_lidar() &&
          !load_pandar40_default_correction() && !load_pandarqt_default_correction()) {
        calibration_status_.store(5);
        std::fprintf(stderr,
                     "ULV Hesai calibration unavailable for %s at %s:%u; "
                     "point-cloud elevation may be invalid\n",
                     detected_model_.c_str(), device_ip_.c_str(), ptc_port_);
      }
      if (calibration_status_.load() == 6 && !device_ip_.empty()) {
        start_async_ptc_fetch();
      }
      incoming_field = packet_field(source->payload, source->payload_size);
      if (!firetimes_file_.empty()) parser_.LoadFiretimesFile(firetimes_file_);
      // The first DecodePacket call only selects the protocol parser. Decode
      // the same datagram again now that frame memory has been allocated.
      const int first_result = parser_.DecodePacket(frame_, packet);
      if (first_result == 0 && !frame_.scan_complete && frame_.packet_num != 0) {
        current_field_ = incoming_field;
        packet_index_++;
      }
      return first_result;
    }

    const int result = parser_.DecodePacket(frame_, packet);
    if (result != 0) return result;
    if (frame_.scan_complete) {
      compute_points_parallel();
      parser_.GetGeneralParser()->FrameProcess(frame_);
      compact_points();
      emit_frame(current_field_);
      frame_.Update();
      packet_index_ = 0;
      current_field_ = incoming_field;

      // A split packet is the first valid packet of the next frame.
      if (parser_.DecodePacket(frame_, packet) == 0 && frame_.packet_num != 0) {
        packet_index_++;
      }
      return 0;
    }

    if (frame_.packet_num != packet_index_) {
      if (packet_index_ == 0) current_field_ = incoming_field;
      packet_index_++;
    }
    return 0;
  }

 private:
  bool configure_at128_fields(const uint8_t* correction, size_t size) {
    if (detected_model_ != "AT128") return true;
    correction_frame_count_ = correction != nullptr && size > 6
        ? std::min<uint8_t>(16, std::max<uint8_t>(1, correction[6]))
        : 1;
    field_starts_.clear();
    field_ends_.clear();
    field_points_.clear();
    field_ready_.clear();
    if (correction_frame_count_ <= 1) return true;
    const auto* correction_struct = static_cast<const AT::ATCorrections*>(
        parser_.getStruct(CORRECTION_STRUCT));
    if (correction_struct == nullptr) {
      correction_frame_count_ = 1;
      return false;
    }
    field_starts_.resize(correction_frame_count_);
    field_ends_.resize(correction_frame_count_);
    field_points_.resize(correction_frame_count_);
    field_ready_.assign(correction_frame_count_, false);
    for (uint8_t field = 0; field < correction_frame_count_; field++) {
      field_starts_[field] = correction_struct->floatCorr.start_frame[field] / kFineResolutionInt;
      field_ends_[field] = correction_struct->floatCorr.end_frame[field] / kFineResolutionInt;
    }
    std::fprintf(stderr, "ULV AT128 field ranges (centidegrees):");
    for (uint8_t field = 0; field < correction_frame_count_; field++) {
      std::fprintf(stderr, " %u=%u..%u", field, field_starts_[field], field_ends_[field]);
    }
    std::fprintf(stderr, "\n");
    return true;
  }

  bool load_correction_from_file(const std::string& path, int status) {
    if (path.empty()) return false;
    std::ifstream input = ulv_plugin_utils::open_ifstream_utf8(path);
    if (!input.is_open()) return false;
    const std::string correction((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
    if (correction.empty()) return false;
    const int result = parser_.LoadCorrectionString(correction.data(),
                                                     static_cast<int>(correction.size()));
    if (result != 0) return false;
    configure_at128_fields(reinterpret_cast<const uint8_t*>(correction.data()), correction.size());
    calibration_status_.store(status);
    std::fprintf(stderr, "ULV Hesai calibration loaded from file %s (%zu bytes)\n",
                 path.c_str(), correction.size());
    return true;
  }

  void compute_points_parallel() {
    const uint32_t packet_count = packet_index_;
    if (packet_count == 0) return;
    const uint32_t worker_count = std::min<uint32_t>(
        packet_count,
        std::max<uint32_t>(1, std::min<uint32_t>(8, std::thread::hardware_concurrency())));
    std::atomic<uint32_t> next_packet{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (uint32_t worker = 0; worker < worker_count; worker++) {
      workers.emplace_back([this, &next_packet, packet_count]() {
        while (true) {
          const uint32_t index = next_packet.fetch_add(1);
          if (index >= packet_count) return;
          parser_.ComputeXYZI(frame_, index);
        }
      });
    }
    for (auto& worker : workers) worker.join();
    parser_.SetComputePacketNumToZero();
  }

  int packet_field(const uint8_t* data, size_t size) const {
    if (correction_frame_count_ <= 1 || data == nullptr || size < 14 ||
        data[0] != 0xee || data[1] != 0xff || data[2] != 4 || data[3] != 3) {
      return -1;
    }
    const auto* header = reinterpret_cast<const HS_LIDAR_HEADER_ST_V3*>(
        data + sizeof(HS_LIDAR_PRE_HEADER));
    if (header->GetBlockNum() == 0 || header->GetLaserNum() == 0) return -1;
    const size_t block_size = sizeof(HS_LIDAR_BODY_AZIMUTH_ST_V3) +
        sizeof(HS_LIDAR_BODY_FINE_AZIMUTH_ST_V3) +
        sizeof(HS_LIDAR_BODY_CHN_NNIT_ST_V3) * header->GetLaserNum();
    const size_t offset = sizeof(HS_LIDAR_PRE_HEADER) + sizeof(HS_LIDAR_HEADER_ST_V3) +
        block_size * (header->GetBlockNum() - 1);
    if (offset + sizeof(HS_LIDAR_BODY_AZIMUTH_ST_V3) > size) return -1;
    const auto azimuth = reinterpret_cast<const HS_LIDAR_BODY_AZIMUTH_ST_V3*>(data + offset)->GetAzimuth();
    for (uint8_t field = 0; field < correction_frame_count_; field++) {
      const uint32_t start = field_starts_[field];
      const uint32_t end = field_ends_[field];
      const uint32_t span = (end + 36000 - start) % 36000;
      const uint32_t from_start = (azimuth + 36000 - start) % 36000;
      if (from_start <= span) return field;
    }
    return -1;
  }

  bool load_correction_from_lidar() {
    if (device_ip_.empty()) {
      std::fprintf(stderr, "ULV Hesai PTC skipped: lidar IP is unknown\n");
      return false;
    }
    PtcClient client(device_ip_, ptc_port_, false, PtcMode::tcp, 1, "", "", "", 1000, 1000, 2.5f, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2800);
    while (!client.IsOpen() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!client.IsOpen()) {
      std::fprintf(stderr,
                   "ULV Hesai PTC unreachable at %s:%u (check host/lidar subnet)\n",
                   device_ip_.c_str(), ptc_port_);
      return false;
    }
    u8Array_t correction;
    const int correction_query = detected_model_ == "JT16"
        ? client.JT16GetCorrectionInfo(correction)
        : client.GetCorrectionInfo(correction);
    if (correction_query != 0 || correction.empty()) {
      std::fprintf(stderr, "ULV Hesai PTC returned no calibration from %s:%u\n",
                   device_ip_.c_str(), ptc_port_);
      return false;
    }
    const int result = parser_.LoadCorrectionString(
        reinterpret_cast<const char*>(correction.data()), static_cast<int>(correction.size()));
    if (result == 0) {
      calibration_status_.store(1);
      save_correction_cache(correction);
      // Only protocol 4.3 (AT128) stores a mirror-field count at byte 6
      // and exposes AT::ATCorrections. Other Hesai models use different
      // correction layouts; interpreting those bytes as AT data can crash.
      configure_at128_fields(correction.data(), correction.size());
      if (detected_model_ == "ATX" || detected_model_ == "PandarQT128") {
        u8Array_t firetimes;
        if (client.GetFiretimesInfo(firetimes) == 0 && !firetimes.empty() &&
            parser_.LoadFiretimesString(reinterpret_cast<const char*>(firetimes.data()),
                                        static_cast<int>(firetimes.size())) == 0) {
          std::fprintf(stderr, "ULV Hesai firetimes loaded from %s:%u (%zu bytes)\n",
                       device_ip_.c_str(), ptc_port_, firetimes.size());
        } else {
          std::fprintf(stderr, "ULV Hesai firetimes unavailable for %s at %s:%u\n",
                       detected_model_.c_str(), device_ip_.c_str(), ptc_port_);
        }
      }
      if (detected_model_ == "PandarQT128") {
        u8Array_t channel_config;
        if (client.GetChannelConfigInfo(channel_config) == 0 && !channel_config.empty() &&
            parser_.LoadChannelConfigString(reinterpret_cast<char*>(channel_config.data())) == 0) {
          std::fprintf(stderr, "ULV Hesai channel config loaded from %s:%u (%zu bytes)\n",
                       device_ip_.c_str(), ptc_port_, channel_config.size());
        } else {
          std::fprintf(stderr, "ULV Hesai channel config unavailable for %s at %s:%u\n",
                       detected_model_.c_str(), device_ip_.c_str(), ptc_port_);
        }
      }
      std::fprintf(stderr,
                   "ULV Hesai calibration loaded from %s:%u (%zu bytes, %u fields)\n",
                   device_ip_.c_str(), ptc_port_, correction.size(), correction_frame_count_);
    }
    return result == 0;
  }

  bool load_correction_from_cache() {
    return load_correction_from_file(correction_cache_file_, 3);
  }

  bool load_sdk_default_correction() {
    if (fallback_correction_file_.empty()) return false;
    if (detected_model_ != "AT128" && detected_model_ != "AT128P") {
      return false;
    }
    return load_correction_from_file(fallback_correction_file_, 6);
  }

  void save_correction_cache(const u8Array_t& correction) {
    if (correction_cache_file_.empty() || correction.empty()) return;
    std::ofstream output = ulv_plugin_utils::open_ofstream_utf8(correction_cache_file_);
    if (!output.is_open()) {
      std::fprintf(stderr, "ULV Hesai could not write calibration cache %s\n",
                   correction_cache_file_.c_str());
      return;
    }
    output.write(reinterpret_cast<const char*>(correction.data()),
                 static_cast<std::streamsize>(correction.size()));
  }

  void start_async_ptc_fetch() {
    if (device_ip_.empty() || ptc_in_progress_.exchange(true)) return;
    std::string ip = device_ip_;
    uint16_t port = ptc_port_;
    std::string model = detected_model_;
    std::string cache_path = correction_cache_file_;
    auto alive = alive_;
    std::thread([this, alive, ip, port, model, cache_path]() {
      PtcClient client(ip, port, false, PtcMode::tcp, 1, "", "", "", 1000, 1000, 2.5f, 0);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
      while (!client.IsOpen() && std::chrono::steady_clock::now() < deadline) {
        if (!*alive) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!client.IsOpen() || !*alive) {
        if (*alive) ptc_in_progress_.store(false);
        return;
      }
      u8Array_t correction;
      const int query_res = model == "JT16"
          ? client.JT16GetCorrectionInfo(correction)
          : client.GetCorrectionInfo(correction);
      if (query_res == 0 && !correction.empty() && *alive) {
        if (!cache_path.empty()) {
          std::ofstream output = ulv_plugin_utils::open_ofstream_utf8(cache_path);
          if (output.is_open()) {
            output.write(reinterpret_cast<const char*>(correction.data()), correction.size());
          }
        }
        {
          std::lock_guard<std::mutex> lock(pending_mutex_);
          pending_factory_correction_.assign(correction.begin(), correction.end());
          pending_factory_ready_.store(true);
        }
      }
      if (*alive) ptc_in_progress_.store(false);
    }).detach();
  }

  void apply_pending_factory_correction() {
    std::vector<uint8_t> corr;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      if (pending_factory_ready_.exchange(false)) {
        corr = std::move(pending_factory_correction_);
      }
    }
    if (!corr.empty()) {
      if (parser_.LoadCorrectionString(reinterpret_cast<const char*>(corr.data()), corr.size()) == 0) {
        calibration_status_.store(1);
        configure_at128_fields(corr.data(), corr.size());
        std::fprintf(stderr, "ULV Hesai async factory calibration applied (%zu bytes)\n", corr.size());
      }
    }
  }

  bool load_pandar40_default_correction() {
    if (detected_model_ != "Pandar40" && detected_model_ != "Pandar40P" &&
        detected_model_ != "Pandar40M") {
      return false;
    }
    // Hesai's official General SDK supplies these nominal Pandar40P/40M
    // angles when a unit-specific PTC correction is unavailable. SDK 2.0's
    // P40 parser otherwise leaves all elevations at zero, flattening Z.
    static constexpr float elevation[40] = {
        15.0f, 11.0f, 8.0f, 5.0f, 3.0f, 2.0f, 1.67f, 1.33f,
        1.0f, 0.67f, 0.33f, 0.0f, -0.33f, -0.67f, -1.0f, -1.33f,
        -1.66f, -2.0f, -2.33f, -2.67f, -3.0f, -3.33f, -3.67f, -4.0f,
        -4.33f, -4.67f, -5.0f, -5.33f, -5.67f, -6.0f, -7.0f, -8.0f,
        -9.0f, -10.0f, -11.0f, -12.0f, -13.0f, -14.0f, -19.0f, -25.0f};
    static constexpr float azimuth[40] = {
        -1.042f, -1.042f, -1.042f, -1.042f, -1.042f, -1.042f, 3.125f, -5.208f,
        -1.042f, 3.125f, -5.208f, -1.042f, 3.125f, -5.208f, -1.042f, 3.125f,
        -5.208f, -1.042f, 3.125f, -5.208f, -1.042f, 3.125f, -5.208f, -1.042f,
        3.125f, -5.208f, -1.042f, 3.125f, -5.208f, -1.042f, -1.042f, -1.042f,
        -1.042f, -1.042f, -1.042f, -1.042f, -1.042f, -1.042f, -1.042f, -1.042f};
    std::ostringstream csv;
    csv << "Laser id,Elevation,Azimuth\n";
    for (int index = 0; index < 40; ++index) {
      csv << index + 1 << ',' << elevation[index] << ',' << azimuth[index] << '\n';
    }
    const std::string correction = csv.str();
    const int result = parser_.LoadCorrectionString(correction.data(),
                                                     static_cast<int>(correction.size()));
    if (result == 0) {
      calibration_status_.store(4);
      std::fprintf(stderr,
                   "ULV Hesai using official nominal Pandar40 angles; "
                   "connect PTC for unit-specific factory calibration\n");
    }
    return result == 0;
  }

  bool load_pandarqt_default_correction() {
    if (detected_model_ != "PandarQT" && detected_model_ != "PandarQT64" &&
        detected_model_ != "QT64") {
      return false;
    }
    // Hesai's official SDK supplies these nominal PandarQT64 angles when
    // a unit-specific PTC correction is unavailable. SDK 2.0's QT parser
    // otherwise leaves all elevations at zero, flattening Z onto a plane.
    static constexpr float elevation[64] = {
        -52.121f, -49.785f, -47.577f, -45.477f, -43.465f, -41.528f, -39.653f, -37.831f,
        -36.055f, -34.320f, -32.619f, -30.950f, -29.308f, -27.690f, -26.094f, -24.517f,
        -22.964f, -21.420f, -19.889f, -18.372f, -16.865f, -15.368f, -13.880f, -12.399f,
        -10.925f,  -9.457f,  -7.994f,  -6.535f,  -5.079f,  -3.626f,  -2.175f,  -0.725f,
          0.725f,   2.175f,   3.626f,   5.079f,   6.534f,   7.993f,   9.456f,  10.923f,
         12.397f,  13.877f,  15.365f,  16.861f,  18.368f,  19.885f,  21.415f,  22.959f,
         24.524f,  26.101f,  27.697f,  29.315f,  30.957f,  32.627f,  34.328f,  36.064f,
         37.840f,  39.662f,  41.537f,  43.475f,  45.487f,  47.587f,  49.795f,  52.133f};
    static constexpr float azimuth[64] = {
          8.736f,   8.314f,   7.964f,   7.669f,   7.417f,   7.198f,   7.007f,   6.838f,
          6.688f,   6.554f,   6.434f,   6.326f,   6.228f,   6.140f,   6.059f,   5.987f,
         -5.270f,  -5.216f,  -5.167f,  -5.123f,  -5.083f,  -5.047f,  -5.016f,  -4.988f,
         -4.963f,  -4.942f,  -4.924f,  -4.910f,  -4.898f,  -4.889f,  -4.884f,  -4.881f,
          5.493f,   5.496f,   5.502f,   5.512f,   5.525f,   5.541f,   5.561f,   5.584f,
          5.611f,   5.642f,   5.676f,   5.716f,   5.759f,   5.808f,   5.862f,   5.921f,
         -5.330f,  -5.396f,  -5.469f,  -5.550f,  -5.640f,  -5.740f,  -5.850f,  -5.974f,
         -6.113f,  -6.269f,  -6.447f,  -6.651f,  -6.887f,  -7.163f,  -7.493f,  -7.892f};
    std::ostringstream csv;
    csv << "Laser id,Elevation,Azimuth\n";
    for (int index = 0; index < 64; ++index) {
      csv << index + 1 << ',' << elevation[index] << ',' << azimuth[index] << '\n';
    }
    const std::string correction = csv.str();
    const int result = parser_.LoadCorrectionString(correction.data(),
                                                     static_cast<int>(correction.size()));
    if (result == 0) {
      calibration_status_.store(4);
      std::fprintf(stderr,
                   "ULV Hesai using official nominal PandarQT angles; "
                   "connect PTC for unit-specific factory calibration\n");
    }
    return result == 0;
  }

  void compact_points() {
    uint32_t point_count = 0;
    for (uint32_t packet = 0; packet < frame_.packet_num; packet++) {
      const uint32_t valid = frame_.valid_points[packet];
      const uint32_t source_offset = packet * frame_.per_points_num;
      if (point_count != source_offset && valid != 0) {
        std::memmove(frame_.points + point_count,
                     frame_.points + source_offset,
                     sizeof(LidarPointXYZICRT) * valid);
      }
      point_count += valid;
    }
    frame_.points_num = point_count;
  }

  void reset_assembly() {
    assembled_.clear();
    assembled_fields_ = 0;
    for (auto& points : field_points_) points.clear();
    std::fill(field_ready_.begin(), field_ready_.end(), false);
  }

  void emit_frame(int field_id) {
    if (frame_.points_num == 0) return;
    converted_.clear();
    converted_.reserve(frame_.points_num);
    for (uint32_t index = 0; index < frame_.points_num; index++) {
      const auto& source = frame_.points[index];
      if (!std::isfinite(source.x) || !std::isfinite(source.y) || !std::isfinite(source.z)) continue;
      const float range_squared = source.x * source.x + source.y * source.y + source.z * source.z;
      if (range_squared <= 0.000001f) continue;
      ulv_point_v1 point{};
      point.x = source.x;
      point.y = source.y;
      point.z = source.z;
      point.intensity = static_cast<float>(source.intensity) / 255.0f;
      point.ring = source.ring;
      point.flags = source.confidence;
      converted_.push_back(point);
    }
    const std::vector<ulv_point_v1>* output = &converted_;
    if (correction_frame_count_ > 1 && field_id >= 0 &&
        static_cast<size_t>(field_id) < field_points_.size()) {
      // AT128's SDK reports each mirror field as a completed scan. Keep the
      // newest scan for every field and publish a rolling full-FOV frame on
      // every field update. Clearing all fields after one publication reduces
      // a nominal 10 Hz stream to 10 / frame_number (about 3.3 Hz for the
      // common three-field correction).
      field_points_[field_id].swap(converted_);
      field_ready_[field_id] = true;
      if (!std::all_of(field_ready_.begin(), field_ready_.end(), [](bool ready) { return ready; })) return;
      assembled_.clear();
      size_t total_points = 0;
      for (const auto& points : field_points_) total_points += points.size();
      assembled_.reserve(total_points);
      for (const auto& points : field_points_) {
        assembled_.insert(assembled_.end(), points.begin(), points.end());
      }
      if (assembled_.size() < kMinAt128FramePoints) {
        reset_assembly();
        return;
      }
      output = &assembled_;
    } else if (correction_frame_count_ > 1) {
      // Older correction formats without field ranges retain count-based
      // assembly as a compatibility fallback.
      assembled_.insert(assembled_.end(), converted_.begin(), converted_.end());
      assembled_fields_++;
      if (assembled_fields_ < correction_frame_count_) return;
      output = &assembled_;
    }
    // Count-based fallback assembly has no stable field identity, so it still
    // starts a new group after emission. Field-aware AT128 assembly retains
    // the other two fields and replaces only the field that just arrived.
    const bool reset_after_emit = output == &assembled_ && field_id < 0;
    if (output->size() > kMaxDisplayPoints) {
      const size_t stride = (output->size() + kMaxDisplayPoints - 1) / kMaxDisplayPoints;
      display_points_.clear();
      display_points_.reserve((output->size() + stride - 1) / stride);
      for (size_t index = 0; index < output->size(); index += stride) {
        display_points_.push_back((*output)[index]);
      }
      output = &display_points_;
    }
    ulv_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.device_id = device_id_;
    frame.sequence = sequence_.fetch_add(1);
    frame.timestamp_ns = static_cast<uint64_t>(frame_.frame_start_timestamp * 1e9);
    frame.vendor = "Hesai";
    frame.model = detected_model_.empty() ? requested_model_.c_str() : detected_model_.c_str();
    frame.points = output->data();
    frame.point_count = output->size();
    callback_(&frame, user_data_);
    if (reset_after_emit) {
      reset_assembly();
    }
  }

  uint32_t device_id_;
  std::string requested_model_;
  std::string detected_model_;
  std::string device_ip_;
  uint16_t ptc_port_;
  uint8_t correction_frame_count_ = 1;
  int current_field_ = -1;
  std::vector<uint32_t> field_starts_;
  std::vector<uint32_t> field_ends_;
  std::vector<std::vector<ulv_point_v1>> field_points_;
  std::vector<bool> field_ready_;
  std::string correction_file_;
  std::string fallback_correction_file_;
  std::string correction_cache_file_;
  std::string firetimes_file_;
  ulv_frame_callback_v1 callback_;
  void* user_data_;
  UdpParser<LidarPointXYZICRT> parser_;
  LidarDecodedFrame<LidarPointXYZICRT> frame_;
  uint32_t packet_index_ = 0;
  std::vector<ulv_point_v1> converted_;
  std::vector<ulv_point_v1> assembled_;
  std::vector<ulv_point_v1> display_points_;
  uint8_t assembled_fields_ = 0;
  std::atomic<uint64_t> sequence_{0};
  std::atomic<int> calibration_status_{0};
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::atomic<bool> ptc_in_progress_{false};
  std::atomic<bool> pending_factory_ready_{false};
  std::vector<uint8_t> pending_factory_correction_;
  std::mutex pending_mutex_;
  // Preserve the complete spatial/ring coverage with a uniform stride while
  // keeping the browser upload below the level that stalls integrated GPUs.
  static constexpr size_t kMaxDisplayPoints = 180000;
  static constexpr size_t kMinAt128FramePoints = 10000;
};

}  // namespace

extern "C" {

uint32_t ulv_plugin_abi_version(void) { return ULV_PLUGIN_ABI_VERSION; }

const char* ulv_plugin_manifest_json(void) {
  return "{\"name\":\"Hesai SDK 2.0 raw adapter\",\"vendor\":\"Hesai\",\"abi\":2}";
}

void* ulv_plugin_create(const char* config_json, ulv_frame_callback_v1 callback, void* user_data) {
  std::unique_ptr<HesaiAdapter> adapter(new HesaiAdapter(config_json, callback, user_data));
  return adapter->valid() ? adapter.release() : nullptr;
}

int ulv_plugin_feed_udp(void* instance, const ulv_udp_packet_v1* packet) {
  if (instance == nullptr || packet == nullptr || packet->struct_size < sizeof(ulv_udp_packet_v1)) return -1;
  return static_cast<HesaiAdapter*>(instance)->feed(packet);
}

void ulv_plugin_destroy(void* instance) { delete static_cast<HesaiAdapter*>(instance); }

const char* ulv_plugin_instance_status_json(void* instance) {
  return instance == nullptr
      ? "{\"state\":\"missing\",\"label\":\"decoder instance missing\"}"
      : static_cast<HesaiAdapter*>(instance)->calibration_status_json();
}

}  // extern "C"
