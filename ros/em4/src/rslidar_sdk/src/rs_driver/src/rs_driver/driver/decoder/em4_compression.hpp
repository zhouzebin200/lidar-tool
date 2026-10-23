#pragma once
#include <cstddef>
#include <cstdint>

namespace robosense { namespace lidar { namespace em4 {
inline uint16_t little16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint16_t big16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }

// Firmware column format: 2-bit mode, 14-bit number of output values,
// little-endian initial value, then signed differences / raw words.
// Blocks are padded to 16 bits, not 32 bits.
inline bool decodeBlocks(const uint8_t* data, size_t size, size_t& offset, uint16_t* values) {
  size_t count = 0;
  while (count < 520) {
    if (offset > size || size-offset < 4) return false;
    uint16_t h = little16(data+offset), value = little16(data+offset+2);
    offset += 4;
    size_t length = h & 0x3fff;
    unsigned mode = h >> 14;
    if (!length || length > 520-count) return false;
    size_t bits = mode == 0 ? 0 : (length-1) * (mode == 1 ? 4 : mode == 2 ? 8 : 16);
    size_t bytes = ((bits+15)/16)*2;
    if (bytes > size-offset) return false;
    for (size_t i=0; i<length; ++i) {
      if (i && mode == 3) value = little16(data+offset+2*(i-1));
      else if (i && mode) {
        int difference;
        if (mode == 1) {
          unsigned nibble = (data[offset+(i-1)/2] >> (((i-1)%2)*4)) & 15;
          difference = nibble >= 8 ? int(nibble)-16 : int(nibble);
        } else difference = static_cast<int8_t>(data[offset+i-1]);
        value = static_cast<uint16_t>(int(value)+difference);
      }
      values[count++] = value;
    }
    offset += bytes;
  }
  return true;
}

// Older SDK stream: one-byte count / type, big-endian initial values,
// optional signed byte differences. Validate all reads before using them.
inline bool decodeLegacy(const uint8_t* data, size_t size, uint16_t* values) {
  size_t offset=0, count=0;
  while (offset < size) {
    uint8_t h=data[offset++]; size_t length=h&127;
    if (!length || length > 520-count) return false;
    size_t bytes=(h&128) ? length+1 : length*2;
    if (bytes > size-offset) return false;
    uint16_t value=big16(data+offset);
    for (size_t i=0; i<length; ++i) {
      if (h&128) { if (i) value=static_cast<uint16_t>(int(value)+static_cast<int8_t>(data[offset+1+i])); }
      else value=big16(data+offset+2*i);
      values[count++]=value;
    }
    offset+=bytes;
  }
  return count == 520;
}

inline bool decodeColumn(const uint8_t* data, size_t size, uint16_t* radius, uint16_t* attributes) {
  if (size < 44) return false;
  size_t end=size-12, offset=32;
  if (end-offset >= 4 && data[offset]==0x55 && data[offset+1]==0xaa) {
    size_t length=big16(data+offset+2); offset+=4;
    if (length > end-offset || !decodeLegacy(data+offset,length,radius)) return false;
    offset+=length;
    if (end-offset < 4 || data[offset]!=0xaa || data[offset+1]!=0x55) return false;
    length=big16(data+offset+2);offset+=4;
    if (length > end-offset || !decodeLegacy(data+offset,length,attributes)) return false;
    offset+=length;
  } else if (!decodeBlocks(data,end,offset,radius) || !decodeBlocks(data,end,offset,attributes)) return false;
  if (offset > end || end-offset > 2) return false;
  while (offset<end) if (data[offset++] != 0) return false;
  return true;
}
}}}
