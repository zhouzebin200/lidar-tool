#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace ulv_plugin_utils {

inline std::string json_string(const char* json, const char* key) {
  if (json == nullptr || key == nullptr) return {};
  const std::string source(json);
  const std::string marker = std::string("\"") + key + "\"";
  size_t position = source.find(marker);
  while (position != std::string::npos) {
    size_t colon = position + marker.size();
    while (colon < source.size() && std::isspace(static_cast<unsigned char>(source[colon]))) colon++;
    if (colon < source.size() && source[colon] == ':') {
      size_t val = colon + 1;
      while (val < source.size() && std::isspace(static_cast<unsigned char>(source[val]))) val++;
      if (val >= source.size() || source[val] != '"') return {};
      std::string out;
      for (size_t i = val + 1; i < source.size(); ++i) {
        if (source[i] == '\\' && i + 1 < source.size()) {
          out.push_back(source[++i]);
        } else if (source[i] == '"') {
          return out;
        } else {
          out.push_back(source[i]);
        }
      }
      return {};
    }
    position = source.find(marker, position + marker.size());
  }
  return {};
}

inline uint32_t json_u32(const char* json, const char* key, uint32_t fallback) {
  if (json == nullptr || key == nullptr) return fallback;
  const std::string source(json);
  const std::string marker = std::string("\"") + key + "\"";
  size_t position = source.find(marker);
  while (position != std::string::npos) {
    size_t colon = position + marker.size();
    while (colon < source.size() && std::isspace(static_cast<unsigned char>(source[colon]))) colon++;
    if (colon < source.size() && source[colon] == ':') {
      size_t val = colon + 1;
      while (val < source.size() && std::isspace(static_cast<unsigned char>(source[val]))) val++;
      try {
        return static_cast<uint32_t>(std::stoul(source.substr(val)));
      } catch (...) {
        return fallback;
      }
    }
    position = source.find(marker, position + marker.size());
  }
  return fallback;
}

inline std::ifstream open_ifstream_utf8(const std::string& utf8_path) {
  if (utf8_path.empty()) return {};
  return std::ifstream(std::filesystem::u8path(utf8_path), std::ios::binary);
}

inline std::ofstream open_ofstream_utf8(const std::string& utf8_path) {
  if (utf8_path.empty()) return {};
  return std::ofstream(std::filesystem::u8path(utf8_path), std::ios::binary | std::ios::trunc);
}

}  // namespace ulv_plugin_utils
