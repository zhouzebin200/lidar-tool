/*********************************************************************************************************************
Copyright (c) 2020 RoboSense
All rights reserved

By downloading, copying, installing or using the software you agree to this license. If you do not agree to this
license, do not download, install, copy or use the software.

License Agreement
For RoboSense LiDAR SDK Library
(3-clause BSD License)

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the names of the RoboSense, nor Suteng Innovation Technology, nor the names of other contributors may be used
to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*********************************************************************************************************************/

#pragma once

#include <rs_driver/driver/decoder/decoder.hpp>
#include <rs_driver/driver/decoder/split_strategy.hpp>
#include <rs_driver/driver/decoder/em4_compression.hpp>
#include <array>
#include <vector>

#define EM4_PIXELS_PER_COLUMN 520
#define EM4_VECSELS_PER_COLUMN 26
#define EM4_PIXELS_PER_VCSEL 20
#define EM4_COMPRESS_SPLIT_LEN 1368 // must less than EM4_COMPRESS_MAX_LEN
#define EM4_COMPRESS_MAX_LEN 2800
namespace robosense
{
namespace lidar
{
#pragma pack(push, 1)

typedef struct
{
  uint8_t ip_local[4];
  uint8_t net_mask[4];
  uint8_t mac[6];
  uint8_t msop_ip_remote[4];
  uint8_t msop_local_port[2];
  uint8_t msop_remote_port[2];
  uint8_t difop_remote_ip[4];
  uint8_t difop_local_port[2];
  uint8_t difop_remote_port[2];
  uint8_t doip_ip_remote[4];
  uint8_t doip_local_port[2];
  uint8_t doip_remote_port[2];
  uint8_t logic_ecu_addr[2];
  uint8_t logic_func_addr[2];
  uint8_t logic_tester_addr[2];
  uint8_t net_addr[2];
  uint8_t reserved1[7];
} RSEM4DifopEther;

typedef struct
{
  uint8_t pwr_spl_fault[3];
  uint8_t temp_fault[3];
  uint8_t sw_fault[3];
  uint8_t perf_fault[6];
  uint8_t fault_status;
  uint8_t fault_level;  
} RSEM4Faults;

typedef struct
{
  uint8_t id[8];
  uint8_t sw_ver[4];
  uint8_t AIPn[4];
  uint8_t PIPn[4];
  uint8_t mcu_ver[4];
  uint8_t hw_ver;
  RSSN int_sn;
  uint8_t _sn[4];
  uint8_t reserved0[9];
  RSEM4DifopEther ether;
  uint8_t work_mode;
  uint8_t frame_rate;
  uint8_t wave_mode;
  RSTimeInfo time_info;
  uint8_t master_slave_mode;
  uint8_t surface_id;
  int8_t yaw_offset[26];
  int16_t pitch_angle[520];
  int16_t roll_offset;
  uint8_t voltage_temp[66];
  uint8_t reserved2[5];
  RSEM4Faults faults;
  uint8_t reserved3[28];
  uint16_t data_length;
  uint16_t counter;
  uint32_t data_id;
  uint32_t crc32;
} RSEM4DifopPkt;

typedef struct
{
  uint8_t id[4];
  uint8_t reserved0[63];
  uint8_t surface_id;
  uint8_t pixelCnt;  // 1 vcsel: 20 pixel
  uint8_t vcselCnt; // 1 column: 26 vcsel
  int8_t yaw_offset[26];
  int16_t pitch_angle[520];
  int16_t surface_pitch_offset[4];
  int16_t roll_offset;
  uint8_t reserved1[4];
  uint16_t data_length;
  uint16_t counter;
  uint32_t data_id;
  uint32_t crc32;
} RSEM4Difop2Pkt;

typedef struct
{
  uint16_t distance;
  uint8_t intensity;
  uint8_t point_attribute;
} RSEM4Channel; // 4-bytes

typedef struct
{
  RSEM4Channel channel[1];
} RSEM4Block;

typedef struct
{
  uint8_t id[4];
  uint16_t pkt_seq;
  uint16_t protocol_version;
  uint8_t return_mode;
  uint8_t time_mode;
  RSTimestampUTC timestamp;
  uint8_t fram_sync;
  uint8_t lidar_health;
  uint16_t column_num;
  int16_t yaw_angle;
  uint8_t pack_mode;
  uint8_t surface_id;
  uint8_t frame_rate;
  uint8_t reserved;
  uint8_t lidar_type;
  uint8_t temperature;
} RSEM4MsopHeader;  // 32-bytes

typedef struct
{
  RSEM4MsopHeader header;
  RSEM4Block blocks[260];
  uint16_t data_length;
  uint16_t counter;
  uint32_t data_id;
  uint32_t crc32;
} RSEM4MsopPkt;  // 1084-bytes

#pragma pack(pop)


// EM4 only: supports v1.1 fixed/legacy packets and current compressed columns.
// Other LiDAR decoders retain their original implementation.
template <typename T_PointCloud>
class DecoderRSEM4 : public Decoder<T_PointCloud>
{
public:
  constexpr static double FRAME_DURATION = 0.1;
  constexpr static uint32_t SINGLE_PKT_NUM = 1050;
  explicit DecoderRSEM4(const RSDecoderParam& param)
    : Decoder<T_PointCloud>(getConstParam(), param) {
    this->bCheckMsopLen_=false;
    this->bCheckDifopLen_=false;
    this->packet_duration_=FRAME_DURATION/SINGLE_PKT_NUM;
    for (size_t i=0;i<520;++i) pitch_[i]=-1300+int(i)*5;
  }

  void decodeDifopPkt(const uint8_t* data, size_t size) override {
    if (size==1162 && std::memcmp(data,"\xa5\xff\x00\xae",4)==0) {
      if (data[67]!=4 || data[68]!=20 || data[69]!=26) return;
      std::array<int,520> pitch;
      std::array<int,4> surface;
      for (size_t i=0;i<520;++i) pitch[i]=static_cast<int16_t>(em4::big16(data+96+2*i));
      for (size_t i=0;i<4;++i) surface[i]=static_cast<int16_t>(em4::big16(data+1136+2*i));
      for (int angle:pitch) for (int correction:surface)
        if (angle+correction < -9000 || angle+correction > 9000) return;
      pitch_=pitch;surface_pitch_=surface;
      // User guide v1.1 Table 10: 26 signed bytes, one per 20 pixels.
      for (size_t i=0;i<26;++i) yaw_[i]=static_cast<int8_t>(data[70+i]);
      this->angles_ready_=true;
    } else if (size==sizeof(RSEM4DifopPkt) &&
        std::memcmp(data,"\xa5\xff\x00\x5a\x11\x11\x55\x55",8)==0) {
      const auto& packet=*reinterpret_cast<const RSEM4DifopPkt*>(data);
      for (size_t i=0;i<26;++i) yaw_[i]=packet.yaw_offset[i];
      for (size_t i=0;i<520;++i) pitch_[i]=static_cast<int16_t>(em4::big16(data+offsetof(RSEM4DifopPkt,pitch_angle)+2*i));
      surface_pitch_.fill(0);
      this->angles_ready_=true;
    }
  }

  bool decodeMsopPkt(const uint8_t* data, size_t size) override {
    if (size<32 || size>EM4_COMPRESS_MAX_LEN || data[30]!=0x70 || data[27]<1 || data[27]>4) return false;
    unsigned mode=data[26]&15, fragment=data[26]>>4;
    if (mode!=1 && mode!=3) return false;
    if (mode==1 && (size!=sizeof(RSEM4MsopPkt) || fragment)) return false;
    if (mode==3 && fragment) {
      if (fragment==1) {
        pending_.assign(data,data+size);
        return false;
      }
      if (fragment!=2 || pending_.empty()) { pending_.clear();return false; }
      bool matching=pending_[4]==data[4] && pending_[5]==data[5] &&
        pending_[22]==data[22] && pending_[23]==data[23] &&
        pending_[27]==data[27] && pending_[8]==data[8];
      if (!matching || pending_.size()+size-32>EM4_COMPRESS_MAX_LEN) { pending_.clear();return false; }
      std::vector<uint8_t> combined;
      combined.swap(pending_);
      combined.insert(combined.end(),data+32,data+size);
      combined[26]=3;
      return decodeMsopPkt(combined.data(),combined.size());
    }
    pending_.clear();
    uint16_t radius[520]{}, attributes[520]{};
    size_t count=mode==1?260:520;
    if (mode==3 && !em4::decodeColumn(data,size,radius,attributes)) return false;
    if (mode==1) for (size_t i=0;i<260;++i) {
      radius[i]=em4::big16(data+32+4*i);
      attributes[i]=data[34+4*i] | (uint16_t(data[35+4*i])<<8);
    }
    double timestamp=this->param_.use_lidar_clock ? parseTimeUTCWithUs(reinterpret_cast<const RSTimestampUTC*>(data+10))*1e-6 : getTimeHost()*1e-6;
    uint16_t sequence=em4::big16(data+4);
    bool split=split_strategy_.newPacket(sequence);
    if (split && !this->point_cloud_->points.empty())
      this->cb_split_frame_(this->const_param_.LASER_NUM,this->cloudTs());
    if (this->point_cloud_->points.empty()) this->first_point_ts_=timestamp;
    int yaw=static_cast<int16_t>(em4::big16(data+24));
    int surface=data[27]-1;
    for (size_t i=0;i<count;++i) {
      size_t ring=mode==1 ? i+((sequence%2)?0:260) : i;
      append(radius[i],attributes[i],ring,yaw,surface,timestamp);
    }
    this->prev_point_ts_=timestamp;this->prev_pkt_ts_=timestamp;
    this->temperature_=int(data[31])-80;
    return split;
  }

private:
  static RSDecoderConstParam& getConstParam() {
    static RSDecoderConstParam param={1084,1310,4,2,
      {0x55,0xaa,0x5a,0xa5},{0xa5,0xff,0,0x5a,0x11,0x11,0x55,0x55},{0,0},
      1,260,1,0.2f,300.0f,0.005f,80.0f};
    return param;
  }
  void append(uint16_t radius,uint16_t attribute,size_t ring,int base_yaw,int surface,double timestamp) {
    float distance=radius*this->const_param_.DISTANCE_RES;
    typename T_PointCloud::PointT point;
    if (radius != 0 && this->distance_section_.in(distance)) {
      int yaw=base_yaw+yaw_[ring/20];
      int pitch=pitch_[ring]+surface_pitch_[surface];
      float x=distance*COS(pitch)*COS(yaw), y=distance*COS(pitch)*SIN(yaw), z=distance*SIN(pitch);
      this->transformPoint(x,y,z);
      setX(point,x);setY(point,y);setZ(point,z);
      setIntensity(point,attribute&255);
    } else {
      if (this->param_.dense_points) return;
      setX(point,NAN);setY(point,NAN);setZ(point,NAN);setIntensity(point,0);
    }
    setRing(point,ring);setTimestamp(point,timestamp);setFeature(point,(attribute>>15)&1);
    this->point_cloud_->points.push_back(point);
  }
  std::array<int,26> yaw_{};
  std::array<int,520> pitch_{};
  std::array<int,4> surface_pitch_{};
  std::vector<uint8_t> pending_;
  SplitStrategyBySeq split_strategy_;
};

} // namespace lidar
} // namespace robosense
