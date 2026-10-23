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
#include <rs_driver/driver/decoder/rle.hpp>

#define EMX_PIXELS_PER_COLUMN 192
#define EMX_VECSELS_PER_COLUMN 24
#define EMX_PIXELS_PER_VCSEL 8
// #define EMX_COMPRESS_SPLIT_LEN 1368 // must less than EMX_COMPRESS_MAX_LEN
// #define EMX_COMPRESS_MAX_LEN 2800
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
} RSEMXDifopEther;

typedef struct
{
  uint8_t pwr_spl_fault[3];
  uint8_t temp_fault[3];
  uint8_t sw_fault[3];
  uint8_t perf_fault[6];
  uint8_t fault_status;
  uint8_t fault_level;  
} RSEMXFaults;

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
  RSEMXDifopEther ether;
  uint8_t work_mode;
  uint8_t frame_rate;
  uint8_t wave_mode;
  RSTimeInfo time_info;
  uint8_t master_slave_mode;
  uint8_t surface_id;
  int8_t yaw_offset[26];
  int16_t pitch_angle[192];
  int16_t roll_offset;
  uint8_t voltage_temp[66];
  uint8_t reserved2[5];
  RSEMXFaults faults;
  uint8_t reserved3[28];
  uint16_t data_length;
  uint16_t counter;
  uint32_t data_id;
  uint32_t crc32;
} RSEMXDifopPkt;
typedef struct
{
  uint8_t id[4];
  uint8_t reserved0[63];
  uint8_t surface_id;
  uint8_t pixelCnt;  // 1 vcsel: 8 pixel
  uint8_t vcselCnt; // 1 column: 24 vcsel
  int8_t yaw_offset[24];
  int16_t pitch_angle[192];
  int16_t surface_pitch_offset[2];
  int16_t roll_offset;
  uint8_t reserved1[4];
  uint16_t data_length;
  uint16_t counter;
  uint32_t data_id;
  uint32_t crc32;
} RSEMXDifop2Pkt;
typedef struct
{
  uint16_t distance;
  uint8_t intensity;
  uint8_t point_attribute;
} RSEMXChannel; // 4-bytes

typedef struct
{
  RSEMXChannel channel[1];
} RSEMXBlock;

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
} RSEMXMsopHeader;  // 32-bytes

typedef struct
{
  RSEMXMsopHeader header;
  RSEMXBlock blocks[192];
  uint16_t data_length;
  uint16_t counter;
  uint32_t data_id;
  uint32_t crc32;
} RSEMXMsopPkt;  // 1084-bytes

#pragma pack(pop)

template <typename T_PointCloud>
class DecoderRSEMX : public Decoder<T_PointCloud>
{
public:
  constexpr static double FRAME_DURATION = 0.1;
  constexpr static uint32_t SINGLE_PKT_NUM = 1050;

  virtual bool decodeMsopPkt(const uint8_t* pkt, size_t size) override;
  virtual ~DecoderRSEMX(){};

  virtual void decodeDifopPkt(const uint8_t* pkt, size_t size) override;

  explicit DecoderRSEMX(const RSDecoderParam& param);
private:
  RLE* rle_; // RLE methods
  int yaw_offset_[EMX_VECSELS_PER_COLUMN];
  int pitch_angle_[EMX_PIXELS_PER_COLUMN];
  int last_pkt_cnt_;
  int last_pkt_pack_num_;
  // uint8_t split_pkt_tmp_buffer[EMX_COMPRESS_SPLIT_LEN];

  static RSDecoderConstParam& getConstParam();
  bool decodeGeneralPkt(const uint8_t* pkt, size_t size);
  bool decodeCompPkt(const uint8_t* pkt, size_t size);
 
  SplitStrategyBySeq split_strategy_;

  
  void parseCompDataOneByOne(uint8_t* udp_payload_input, const uint16_t payload_len, 
                                                uint16_t* radius_decode_array, uint16_t *ref_attr_decode_array,
                                                const int16_t yaw_angle,  const double pkt_ts);

  void parseCompDataSaveCur(uint8_t* udp_payload_input, const uint16_t payload_len, 
                                                uint16_t* radius_decode_array, uint16_t *ref_attr_decode_array,
                                                const int16_t yaw_angle,  const double pkt_ts);

  void parseCompDataDropCur(uint8_t* udp_payload_input, const uint16_t payload_len, 
                                                uint16_t* radius_decode_array, uint16_t *ref_attr_decode_array,
                                                const int16_t yaw_angle, const double pkt_ts);

  void parseCompDataCombine(uint8_t* udp_payload_input, const uint16_t payload_len, 
                                                uint16_t* radius_decode_array, uint16_t *ref_attr_decode_array,
                                                const int16_t yaw_angle, const double pkt_ts);
  
  void rleDecodeMethod(uint8_t* udp_payload, const uint16_t payload_len,  uint16_t* radius_decode_array, uint16_t *ref_attr_decode_array, bool is_wreck,
                                           const int16_t yaw_angle, const double pkt_ts);

};
template <typename T_PointCloud>
inline RSDecoderConstParam& DecoderRSEMX<T_PointCloud>::getConstParam()
{
  static RSDecoderConstParam param = 
  {
    812 // msop len
      , 654 // difop len
      , 4 // msop id len
      , 8 // difop id len
      , {0x55, 0xAA, 0x5A, 0xA5} // msop id
    , {0xA5, 0xFF, 0x00, 0x5A, 0x11, 0x11, 0x55, 0x55} // difop id
    , {0x00, 0x00}
    , 1  // laser number
    , EMX_PIXELS_PER_COLUMN // blocks per packet
      , 1 // channels per block
      , 0.2f // distance min
      , 300.0f // distance max
      , 0.005f // distance resolution
      , 80.0f // initial value of temperature 
  };

  return param;
}

template <typename T_PointCloud>
inline DecoderRSEMX<T_PointCloud>::DecoderRSEMX(const RSDecoderParam& param)
: Decoder<T_PointCloud>(getConstParam(), param), last_pkt_cnt_(-1), last_pkt_pack_num_(-1)
  
{  

  this->packet_duration_ = FRAME_DURATION / SINGLE_PKT_NUM;
  this->angles_ready_ = true;

    this->bCheckMsopLen_ = false;
    this->bCheckDifopLen_ = false;
  for (int i = 0; i < EMX_VECSELS_PER_COLUMN; i++) { this->yaw_offset_[i] = 0; }
  const int DEFAULT_ANGLE[EMX_PIXELS_PER_COLUMN] = {
        -2416, -2395, -2374, -2353, -2332, -2311, -2290, -2269, -2248, -2227, 
    -2206, -2185, -2164, -2143, -2122, -2101, -2080, -2059, -2038, -2017, 
    -1996, -1975, -1954, -1933, -1912, -1891, -1870, -1849, -1828, -1807, 
    -1786, -1765, -1744, -1723, -1702, -1681, -1660, -1639, -1618, -1597, 
    -1576, -1555, -1534, -1513, -1492, -1471, -1450, -1429, -1408, -1387, 
    -1366, -1345, -1324, -1303, -1282, -1261, -1240, -1219, -1198, -1177, 
    -1156, -1135, -1114, -1093, -1072, -1051, -1030, -1009, -988, -967, 
    -946, -925, -904, -883, -862, -841, -820, -799, -778, -757, 
    -736, -715, -694, -673, -652, -631, -610, -589, -568, -547, 
    -526, -505, -484, -463, -442, -421, -400, -379, -358, -337, 
    -316, -295, -274, -253, -232, -211, -190, -169, -148, -127, 
    -106, -85, -64, -43, -22, -1, 20, 41, 62, 83, 
    104, 125, 146, 167, 188, 209, 230, 251, 272, 293, 
    314, 335, 356, 377, 398, 419, 440, 461, 482, 503, 
    524, 545, 566, 587, 608, 629, 650, 671, 692, 713, 
    734, 755, 776, 797, 818, 839, 860, 881, 902, 923, 
    944, 965, 986, 1007, 1028, 1049, 1070, 1091, 1112, 1133, 
    1154, 1175, 1196, 1217, 1238, 1259, 1280, 1301, 1322, 1343, 
    1364, 1385, 1406, 1427, 1448, 1469, 1490, 1511, 1532, 1553, 
    1574, 1595,
  };
  for (int i = 0; i < EMX_PIXELS_PER_COLUMN; i++) { this->pitch_angle_[i] = DEFAULT_ANGLE[i]; }
}

template <typename T_PointCloud>
inline void DecoderRSEMX<T_PointCloud>::decodeDifopPkt(const uint8_t* packet, size_t size)
{
  if(size < sizeof(RSEMXDifopPkt))
  {
    return;
  }
  const RSEMXDifopPkt& pkt = *(const RSEMXDifopPkt*)(packet);
  for(int i = 0; i < EMX_VECSELS_PER_COLUMN; i++) { this->yaw_offset_[i] = RS_INT8(pkt.yaw_offset[i]); }
  for (int i = 0; i < EMX_PIXELS_PER_COLUMN; i++)
  {
    this->pitch_angle_[i] = RS_SWAP_INT16(pkt.pitch_angle[i]);
  }
}


template <typename T_PointCloud>
inline bool DecoderRSEMX<T_PointCloud>::decodeMsopPkt(const uint8_t* packet, size_t size)
{
  const RSEMXMsopHeader& header = *(RSEMXMsopHeader*)packet;
  
  uint8_t pack_mode = header.pack_mode & 0x03;
  if(pack_mode == 0x01) // without compression
  {

    return this->decodeGeneralPkt(packet, size);
  }else if(pack_mode == 0x03) // with compression
  {

    return this->decodeCompPkt(packet, size);
  }
  return false;
}


template <typename T_PointCloud>

inline bool DecoderRSEMX<T_PointCloud>::decodeCompPkt(const uint8_t* packet, size_t size)
{
  if(size < sizeof(RSEMXMsopHeader))
  {
    RS_WARNING << "MSOP packet size is too small: " << size << ",  less than sizeof(RSEMXMsopHeader):"<<sizeof(RSEMXMsopHeader)<< RS_REND;
    return false;
  }
  bool ret = false;
  const RSEMXMsopHeader& header = *(RSEMXMsopHeader*)packet;
  
  double pkt_ts = 0;
  if (this->param_.use_lidar_clock)
  {
    pkt_ts = parseTimeUTCWithUs(&header.timestamp) * 1e-6;
  }
  else
  {
    uint64_t ts = getTimeHost();

    // roll back to first block to approach lidar ts as near as possible.
    pkt_ts = getTimeHost() * 1e-6 - this->getPacketDuration();

    if (this->write_pkt_ts_)
    {
      createTimeUTCWithUs (ts, (RSTimestampUTC*)&header.timestamp);
    }
  }
  uint16_t pkt_seq = ntohs(header.pkt_seq);
  this->temperature_ = static_cast<float>((int)header.temperature - this->const_param_.TEMPERATURE_RES);
 
  
  if (split_strategy_.newPacket(pkt_seq))
  {
    // msop-compress-debug
    this->cb_split_frame_(this->const_param_.LASER_NUM, this->cloudTs());
    this->first_point_ts_ = pkt_ts;
    ret = true;
  }

  int16_t yaw_angle = int16_t((float)(RS_SWAP_INT16(header.yaw_angle) / 5.12)) ;

  //distance
  uint16_t radius_decode_array[EMX_PIXELS_PER_COLUMN] = { 0 };

  //intensity
  uint16_t ref_attr_decode_array[EMX_PIXELS_PER_COLUMN] = { 0 };
  uint8_t* udp_payload_input = (uint8_t *)packet;


  rleDecodeMethod(udp_payload_input, size, radius_decode_array, ref_attr_decode_array, false, yaw_angle, pkt_ts);

  return ret;
}


template <typename T_PointCloud>
inline void DecoderRSEMX<T_PointCloud>::rleDecodeMethod(uint8_t* udp_payload, const uint16_t payload_len, uint16_t* radius_decode_array, uint16_t *ref_attr_decode_array, bool is_wreck,
                                           const int16_t yaw_angle, const double pkt_ts)
{ 
  uint32_t parse_index = sizeof(RSEM4MsopHeader);
  if((parse_index + 3 ) >= payload_len ){
    return ;
  }                                          
  //distance
  uint32_t radius_decode_len = 0;
  uint16_t radius_raw_len = 0;
  
  if ((udp_payload[parse_index] == 0x55) && (udp_payload[parse_index + 1] == 0xaa))
  {
    radius_raw_len = (uint16_t)((udp_payload[parse_index + 2] << 8) | udp_payload[parse_index + 3]);  
  }else{
    RS_WARNING << "MSOP radius header id error" << RS_REND;
    return;
  }
  bool radius_rle_ret   = this->rle_->PcC_u32OffsetRLEDecode(udp_payload + 36, radius_raw_len, radius_decode_array, EMX_PIXELS_PER_COLUMN, radius_decode_len);
  /* Prevent the compiler warning about the unused parameter. */
  (void)radius_rle_ret;

  // intensity
  uint32_t intensity_decode_len = 0;
  uint16_t intensity_raw_len = 0;
    
  // update parse index 
  parse_index = parse_index + 4 + radius_raw_len;

  if((parse_index + 4 ) >= payload_len ){
    return ;
  }

  if ((udp_payload[parse_index] == 0xaa) && (udp_payload[parse_index + 1] == 0x55))
  {
    intensity_raw_len = (uint16_t)((udp_payload[parse_index + 2] << 8) | udp_payload[parse_index + 3]);
  }else{
    return;
  }

  bool intensity_rle_ret  = this->rle_->PcC_u32OffsetRLEDecode(udp_payload + parse_index + 4, intensity_raw_len, ref_attr_decode_array, EMX_PIXELS_PER_COLUMN, intensity_decode_len);   

  int actual_decode_len = EMX_PIXELS_PER_COLUMN;
  if ((intensity_rle_ret || is_wreck) && intensity_decode_len < EMX_PIXELS_PER_COLUMN && intensity_decode_len > 0) {
    actual_decode_len = intensity_decode_len;
  }

  for (int i = 0; i < actual_decode_len; i++)
  {
    int real_chan = i;
    float distance = radius_decode_array[i] * this->const_param_.DISTANCE_RES;

    if (this->distance_section_.in(distance))
    {     
      int vecsel = real_chan / EMX_PIXELS_PER_VCSEL;
      int yaw = yaw_angle + this->yaw_offset_[vecsel];
      int pitch = this->pitch_angle_[real_chan] / 2;
      float x = distance * COS (pitch) * COS (yaw);
      float y = distance * COS (pitch) * SIN (yaw);
      float z = distance * SIN (pitch);
      this->transformPoint(x, y, z);      
      uint8_t intensity = (uint8_t)(ref_attr_decode_array[i] & 0xFFU);

      typename T_PointCloud::PointT point;

      setX(point, x);
      setY(point, y);
      setZ(point, z);      
      setTimestamp(point, pkt_ts);
      setRing(point, real_chan);
      setIntensity(point,intensity);
      this->point_cloud_->points.emplace_back(point);
    }
    else if (!this->param_.dense_points)
    {
      typename T_PointCloud::PointT point;
      setX(point, NAN);
      setY(point, NAN);
      setZ(point, NAN);
      setIntensity(point, 0);
      setTimestamp(point, pkt_ts);
      setRing(point, real_chan);
      this->point_cloud_->points.emplace_back(point);
    }
    this->prev_point_ts_ = pkt_ts;
  }
  this->prev_pkt_ts_ = pkt_ts;
}


template <typename T_PointCloud>
inline bool DecoderRSEMX<T_PointCloud>::decodeGeneralPkt(const uint8_t* packet, size_t size)
{
  if(size < sizeof(RSEMXMsopPkt))
  {
    RS_WARNING << "decodeGeneralPkt: packet size < sizeof(RSEMXMsopPkt)" << RS_REND;
    return false;
  }
  const RSEMXMsopPkt& pkt = *(RSEMXMsopPkt*)packet;
  bool ret = false;
  this->temperature_ = static_cast<float>((int)pkt.header.temperature - this->const_param_.TEMPERATURE_RES);

  double pkt_ts = 0;
  if (this->param_.use_lidar_clock)
  {
    pkt_ts = parseTimeUTCWithUs(&pkt.header.timestamp) * 1e-6;
  }
  else
  {
    uint64_t ts = getTimeHost();

    // roll back to first block to approach lidar ts as near as possible.
    pkt_ts = getTimeHost() * 1e-6 - this->getPacketDuration();

    if (this->write_pkt_ts_)
    {
      createTimeUTCWithUs (ts, (RSTimestampUTC*)&pkt.header.timestamp);
    }
  }

  uint16_t pkt_seq = ntohs(pkt.header.pkt_seq);

  if (split_strategy_.newPacket(pkt_seq))
  {
    this->cb_split_frame_(this->const_param_.LASER_NUM, this->cloudTs());
    this->first_point_ts_ = pkt_ts;
    ret = true;
  }

  for (uint16_t blk = 0; blk < this->const_param_.BLOCKS_PER_PKT; blk++)
  {
    const RSEMXBlock& block = pkt.blocks[blk];

    double point_time = pkt_ts;

    for (uint16_t chan = 0; chan < this->const_param_.CHANNELS_PER_BLOCK; chan++)
    {
      const RSEMXChannel& channel = block.channel[chan];
      
      float distance = ntohs(channel.distance) * this->const_param_.DISTANCE_RES;

      int vecsel = blk / EMX_PIXELS_PER_VCSEL;
      
      int yaw =  int((float)(RS_SWAP_INT16(pkt.header.yaw_angle) / 5.12)) + this->yaw_offset_[vecsel];

      int pitch = this->pitch_angle_[blk] / 2;
      if (this->distance_section_.in(distance) )
      {
        float x = distance * COS (pitch) * COS (yaw);
        float y = distance * COS (pitch) * SIN (yaw);
        float z = distance * SIN (pitch);
        // compute average area  
        this->transformPoint(x, y, z);

        typename T_PointCloud::PointT point;

        setX(point, x);
        setY(point, y);
        setZ(point, z);
        setIntensity(point, channel.intensity);
        setTimestamp(point, point_time);
        setRing(point, blk);
        this->point_cloud_->points.emplace_back(point);
      }
      else if (!this->param_.dense_points)
      {
        typename T_PointCloud::PointT point;
        setX(point, NAN);
        setY(point, NAN);
        setZ(point, NAN);
        setIntensity(point, 0);
        setTimestamp(point, point_time);
        setRing(point, blk);
        this->point_cloud_->points.emplace_back(point);
      }
    }

    this->prev_point_ts_ = point_time;
  }

  this->prev_pkt_ts_ = pkt_ts;


  return ret;
}

}  // namespace lidar
}  // namespace robosense
