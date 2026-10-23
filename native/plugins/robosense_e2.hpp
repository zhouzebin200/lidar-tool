// E2 protocol derived from RoboSense decoder_RSE2.hpp; see ../vendor/rse2/LICENSE.
#pragma once
#include "ulv_decoder_plugin.h"
#include "../vendor/rse2/rs_default.hpp"
#include "../vendor/rse2/rs_crc32.hpp"
#include <bitset>
#include <cmath>
#include <cstring>
#include <vector>

class E2Decoder {
 public:
  E2Decoder(uint32_t id, ulv_frame_callback_v1 callback, void* user)
      : id_(id), callback_(callback), user_(user), angles_(robosense::Lidar::RSE2_AngleData_Default) {}
  static bool is_msop(const uint8_t* p, size_t n) {
    return p && n == 844 && std::memcmp(p, "\x55\xaa\x5a\xa5", 4) == 0;
  }
  static bool is_angles(const uint8_t* p, size_t n) {
    return p && n == 1296 && std::memcmp(p, "\xa5\xff\x00\xae", 4) == 0;
  }
  void feed(const uint8_t* p, size_t n, uint64_t receive_ns) {
    if (is_angles(p,n)) { calibration(p); return; }
    if (!is_msop(p,n) || p[21]>=24 || p[22]>=32 || p[6]>3) return;
    // A single-return packet spans two adjacent 10x10 blocks.
    const bool dual = p[6]==0;
    if (!dual && p[22]>=31) return;
    // Reboots, long capture gaps and PCAP loop/restart invalidate counter history.
    if (started_ && (receive_ns < last_receive_ns_ || receive_ns-last_receive_ns_ > 1000000000ULL)) {
      points_.clear(); seen_.reset(); started_=false;
    }
    const uint16_t seq = u16(p+4);
    const uint8_t frame = p[20];
    if (started_ && frame != frame_) {
      // Discard late packets belonging to an older frame, including wraparound.
      if (static_cast<uint8_t>(frame-frame_) > 127) return;
      emit(); seen_.reset(); points_.clear(); started_=false;
    }
    if (seen_.test(seq)) return;
    if (!started_) { frame_=frame; timestamp_=timestamp(p,receive_ns); started_=true; }
    seen_.set(seq);
    last_receive_ns_=receive_ns;
    // Bound memory even if a damaged source never changes its frame counter.
    if (points_.size()+200 > 320*240*2) return;
    for (unsigned i=0;i<200;i++) {
      const unsigned pixel = dual ? i/2 : i%100;
      const unsigned row = p[21]*10 + pixel%10;
      const unsigned col = (p[22]+(dual?0:i/100))*10 + pixel/10;
      const size_t a = (row*320+col)*3;
      const uint16_t radius=u16(p+32+i*4);
      if (!radius || radius>60000) continue;
      const float scale=radius*0.005f/32768.f;
      ulv_point_v1 point{};
      point.x=signed16(angles_[a])*scale;
      point.y=signed16(angles_[a+1])*scale;
      point.z=signed16(angles_[a+2])*scale;
      if (point.x==0 && point.y==0 && point.z==0) continue;
      point.intensity=p[34+i*4]/255.f;
      point.ring=static_cast<uint16_t>(row);
      point.return_type=dual?static_cast<uint8_t>(i%2+1):1;
      points_.push_back(point);
    }
  }
 private:
  static uint16_t u16(const uint8_t* p) { return (uint16_t(p[0])<<8)|p[1]; }
  static uint32_t u32(const uint8_t* p) { return (uint32_t(u16(p))<<16)|u16(p+2); }
  static int32_t signed16(uint16_t v) { return v<32768?v:int32_t(v)-65536; }
  static uint64_t timestamp(const uint8_t* p,uint64_t fallback) {
    uint64_t sec=0;for(size_t i=8;i<14;i++)sec=(sec<<8)|p[i];
    const uint32_t us=u32(p+14);
    if (us>=1000000 || sec>18446744072ULL || (sec==0 && us==0)) return fallback;
    return sec*1000000000ULL+uint64_t(us)*1000;
  }
  void calibration(const uint8_t* p) {
    const unsigned counter=u16(p+1286);
    if (counter==0 || counter>360 || robosense::lidar::CRC32Custom::calculate(p,1292)!=u32(p+1292)) {
      pending_.clear(); next_=1; return;
    }
    // A new first packet resynchronizes after a dropped or reordered transfer.
    if (counter==1) { pending_.clear(); next_=1; }
    if(counter!=next_) { pending_.clear(); next_=1; return; }
    for(size_t i=0;i<640;i++) pending_.push_back(u16(p+4+i*2));
    next_++;
    if(counter==360) {
      // Never replace usable geometry with an all-zero table.
      bool usable=false;for(uint16_t v:pending_)if(v){usable=true;break;}
      if(usable && pending_.size()==230400) angles_.swap(pending_);
      pending_.clear(); next_=1;
    }
  }
  void emit() {
    if(points_.empty() || !callback_)return;
    ulv_frame_v1 f{};f.struct_size=sizeof(f);f.device_id=id_;f.sequence=sequence_++;
    f.timestamp_ns=timestamp_;f.vendor="RoboSense";f.model="RSE2";f.points=points_.data();f.point_count=points_.size();
    callback_(&f,user_);
  }
  uint32_t id_;ulv_frame_callback_v1 callback_;void* user_;
  std::vector<uint16_t> angles_,pending_;
  std::vector<ulv_point_v1> points_;
  std::bitset<65536> seen_;
  unsigned next_=1;uint8_t frame_=0;bool started_=false;
  uint64_t sequence_=0,timestamp_=0,last_receive_ns_=0;
};
