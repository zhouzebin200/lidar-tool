/*********************************************************************************************************************
Copyright (c) 2020 RoboSense
All rights reserved

By downloading, copying, installing or using the software you agree to this license. If you do not agree to this
license, do not download, install, copy or use the software.

License Agreement
For RoboSense LiDAR SDK Library
(3-clause BSD License)
*********************************************************************************************************************/

#pragma once

#include <cstdint>
#include <cstddef>

namespace robosense
{
namespace lidar
{

class CRC32Custom
{
public:
  CRC32Custom()
  {
    generateTable();
    reset();
  }

  void reset()
  {
    crc_ = INIT;
  }

  void update(const uint8_t* data, size_t length)
  {
    for (size_t i = 0; i < length; ++i)
    {
      uint8_t index = static_cast<uint8_t>((crc_ ^ data[i]) & 0xFF);
      crc_ = (crc_ >> 8) ^ table_[index];
    }
  }

  uint32_t final() const
  {
    return crc_ ^ XOR_OUT;
  }

  static uint32_t calculate(const uint8_t* data, size_t length)
  {
    CRC32Custom crc32;
    crc32.update(data, length);
    return crc32.final();
  }

private:
  static constexpr uint32_t POLY = 0xF4ACFB13;
  static constexpr uint32_t INIT = 0xFFFFFFFF;
  static constexpr uint32_t XOR_OUT = 0xFFFFFFFF;

  uint32_t crc_;
  uint32_t table_[256];

  static uint32_t reflect32(uint32_t value)
  {
    uint32_t result = 0;
    for (int i = 0; i < 32; ++i)
    {
      if (value & (1u << i))
      {
        result |= (1u << (31 - i));
      }
    }
    return result;
  }

  void generateTable()
  {
    const uint32_t reflected_poly = reflect32(POLY);
    for (uint32_t i = 0; i < 256; ++i)
    {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit)
      {
        crc = (crc & 1) ? ((crc >> 1) ^ reflected_poly) : (crc >> 1);
      }
      table_[i] = crc;
    }
  }
};

}  // namespace lidar
}  // namespace robosense
