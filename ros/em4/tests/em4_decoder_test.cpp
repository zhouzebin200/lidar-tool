#include <rs_driver/msg/point_cloud_msg.hpp>
#include <rs_driver/driver/decoder/split_strategy.hpp>
#include <rs_driver/driver/decoder/decoder_RSEM4.hpp>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace robosense::lidar;
using Cloud = PointCloudT<PointXYZIRT>;
using Bytes = std::vector<uint8_t>;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void be(Bytes& p, size_t offset, int value) { p[offset]=value>>8; p[offset+1]=value; }
static Bytes calibration() {
  Bytes p(1162); p[0]=0xa5;p[1]=0xff;p[3]=0xae;p[67]=4;p[68]=20;p[69]=26;
  p[70]=156; p[71]=100; // signed -100 / +100 hundredths of a degree
  be(p,1138,30); // surface B +0.30 degree
  return p;
}
struct Fixture {
  DecoderRSEM4<Cloud> decoder;
  size_t emitted=0;
  static RSDecoderParam parameters() {
    RSDecoderParam p;p.dense_points=true;p.use_lidar_clock=true;p.wait_for_difop=true;
    return p;
  }
  Fixture():decoder(parameters()) {
    decoder.point_cloud_=std::make_shared<Cloud>();
    decoder.regCallback([](const Error&){},[this](uint16_t,double){++emitted;decoder.point_cloud_->points.clear();});
  }
  void calibrate() {auto p=calibration();decoder.processDifopPkt(p.data(),p.size());}
  void feed(const Bytes& p) {decoder.processMsopPkt(p.data(),p.size());}
};
static Bytes header(size_t size, uint16_t sequence=1, uint8_t mode=3) {
  Bytes p(size);p[0]=0x55;p[1]=0xaa;p[2]=0x5a;p[3]=0xa5;be(p,4,sequence);
  p[8]=4;p[15]=1;p[26]=mode;p[27]=2;p[30]=0x70;
  return p;
}
static Bytes compressed(const Bytes& radius={8,2,200,0}, uint16_t sequence=1) {
  auto p=header(32,sequence);p.insert(p.end(),radius.begin(),radius.end());
  p.insert(p.end(),{8,2,5,0});p.resize(p.size()+12);return p;
}
static Bytes fixed(uint16_t sequence=1) {
  auto p=header(1084,sequence,1);
  for (int i=0;i<260;++i) {be(p,32+4*i,200);p[34+4*i]=5;}
  return p;
}
static void point(const PointXYZIRT& p,double distance,double yaw,double pitch) {
  double r=3.14159265358979323846/180;
  check(std::abs(p.x-distance*std::cos(pitch*r)*std::cos(yaw*r))<2e-5,"X coordinate / radius / yaw mismatch");
  check(std::abs(p.y-distance*std::cos(pitch*r)*std::sin(yaw*r))<2e-5,"Y coordinate / signed per-20-channel yaw mismatch");
  check(std::abs(p.z-distance*std::sin(pitch*r))<2e-5,"Z coordinate / surface compensation mismatch");
}
int main() {
  int failures=0;
  auto test=[&](const char* name,const std::function<void()>& fn) {
    try{fn();std::cout<<"PASS "<<name<<'\n';}
    catch(const std::exception& e){++failures;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}
  };
  test("DIFOP2 unlocks fixed-format points and applies signed yaw/surface pitch",[]{
    Fixture f;f.calibrate();f.feed(fixed());auto& p=f.decoder.point_cloud_->points;
    check(p.size()==260,"expected 260 fixed-format points after DIFOP2");
    point(p[0],1,-1,.3);point(p[20],1,1,.3);check(p[20].ring==20,"ring mismatch");
  });
  test("current compressed column has 520 points",[]{
    Fixture f;f.calibrate();f.feed(compressed());auto& p=f.decoder.point_cloud_->points;
    check(p.size()==520,"expected 520 decompressed points");point(p[0],1,-1,.3);
    check(p.back().ring==519,"last ring mismatch");check(p[0].intensity==5,"intensity mismatch");
  });
  test("signed 4-bit differences and 16-bit block alignment",[]{
    Fixture f;f.calibrate();f.feed(compressed({3,0x40,200,0,0x2f,0,5,2,200,0}));
    auto& p=f.decoder.point_cloud_->points;check(p.size()==520,"nibble decompression failed");
    point(p[0],1,-1,.3);point(p[1],.995,-1,.3);point(p[2],1.005,-1,.3);
  });
  test("signed 8-bit differences",[]{
    Fixture f;f.calibrate();f.feed(compressed({3,0x80,200,0,156,100,5,2,200,0}));
    auto& p=f.decoder.point_cloud_->points;check(p.size()==520,"byte decompression failed");
    point(p[1],.5,-1,.3);point(p[2],1,-1,.3);
  });
  test("raw 16-bit compression blocks",[]{
    Fixture f;f.calibrate();f.feed(compressed({3,0xc0,200,0,144,1,88,2,5,2,200,0}));
    auto& p=f.decoder.point_cloud_->points;check(p.size()==520,"raw decompression failed");
    point(p[1],2,-1,.3);point(p[2],3,-1,.3);
  });
  test("malformed compressed data emits no partial points",[]{
    Fixture f;f.calibrate();auto p=compressed({0,0,200,0});f.feed(p);
    check(f.decoder.point_cloud_->points.empty(),"zero-length block was accepted");
    p=compressed();p.resize(35);f.feed(p);check(f.decoder.point_cloud_->points.empty(),"truncated header accepted");
    p=compressed({9,2,200,0});f.feed(p);check(f.decoder.point_cloud_->points.empty(),"521-point block accepted");
  });
  test("missing or truncated calibration does not unlock points",[]{
    Fixture f;auto c=calibration();c.resize(1161);f.decoder.processDifopPkt(c.data(),c.size());f.feed(fixed());
    check(f.decoder.point_cloud_->points.empty(),"invalid calibration unlocked decoding");
  });
  test("zero radius stays invalid when user minimum is zero",[]{
    auto param=Fixture::parameters();param.min_distance=0;param.max_distance=300;
    DecoderRSEM4<Cloud> decoder(param);decoder.point_cloud_=std::make_shared<Cloud>();
    decoder.regCallback([](const Error&){},[](uint16_t,double){});
    auto c=calibration();decoder.processDifopPkt(c.data(),c.size());
    auto p=compressed({8,2,0,0});decoder.processMsopPkt(p.data(),p.size());
    check(decoder.point_cloud_->points.empty(),"invalid zero distance emitted at the origin");
  });
  test("optional feature point type preserves the SDK binary flag",[]{
    using FeatureCloud=PointCloudT<PointXYZIRTF>;
    DecoderRSEM4<FeatureCloud> decoder(Fixture::parameters());
    decoder.point_cloud_=std::make_shared<FeatureCloud>();
    decoder.regCallback([](const Error&){},[](uint16_t,double){});
    auto c=calibration();decoder.processDifopPkt(c.data(),c.size());
    auto p=header(52);const uint8_t blocks[]={8,2,200,0,8,2,5,128};
    std::memcpy(p.data()+32,blocks,8);decoder.processMsopPkt(p.data(),p.size());
    check(decoder.point_cloud_->points.size()==520,"feature fixture failed");
    check(decoder.point_cloud_->points[0].feature==1,"feature must retain upstream high-bit flag semantics");
  });
  test("legacy 1310-byte calibration remains supported",[]{
    Fixture f;RSEM4DifopPkt c{};const uint8_t magic[]={0xa5,0xff,0,0x5a,0x11,0x11,0x55,0x55};
    std::memcpy(c.id,magic,8);c.yaw_offset[0]=-100;
    f.decoder.processDifopPkt(reinterpret_cast<uint8_t*>(&c),sizeof(c));f.feed(fixed());
    auto& p=f.decoder.point_cloud_->points;check(p.size()==260,"legacy calibration rejected");point(p[0],1,-1,0);
  });
  test("legacy marked RLE compressed stream remains supported",[]{
    Fixture f;f.calibrate();Bytes values;
    for(int k=0;k<5;++k){int n=k==4?12:127;values.push_back(0x80|n);values.push_back(0);values.push_back(200);values.resize(values.size()+n-1,0);}
    auto p=header(32);p.insert(p.end(),{0x55,0xaa,uint8_t(values.size()>>8),uint8_t(values.size())});
    p.insert(p.end(),values.begin(),values.end());
    p.insert(p.end(),{0xaa,0x55,uint8_t(values.size()>>8),uint8_t(values.size())});
    p.insert(p.end(),values.begin(),values.end());p.resize(p.size()+12);f.feed(p);
    check(f.decoder.point_cloud_->points.size()==520,"legacy RLE points missing");
    point(f.decoder.point_cloud_->points[0],1,-1,.3);
  });
  test("new-format fragments require matching sequence and surface",[]{
    Fixture f;f.calibrate();auto p=compressed();auto first=Bytes(p.begin(),p.begin()+36);first[26]=0x13;
    auto second=header(32,1,0x23);second.insert(second.end(),p.begin()+36,p.end());
    f.feed(first);check(f.decoder.point_cloud_->points.empty(),"first fragment emitted incomplete column");
    f.feed(second);check(f.decoder.point_cloud_->points.size()==520,"matching fragments failed");
    f.decoder.point_cloud_->points.clear();f.feed(first);be(second,4,2);f.feed(second);
    check(f.decoder.point_cloud_->points.empty(),"mismatched sequence was combined");
  });
  std::cout<<"Failures: "<<failures<<'\n';return failures?1:0;
}
