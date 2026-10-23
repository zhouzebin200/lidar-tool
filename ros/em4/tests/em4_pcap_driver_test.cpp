#include <rs_driver/api/lidar_driver.hpp>
#include <rs_driver/msg/point_cloud_msg.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
using namespace robosense::lidar;
using Cloud=PointCloudT<PointXYZIRT>;
int main(int argc,char** argv) {
  if (argc<2) { std::cerr<<"Usage: em4_pcap_driver_test capture.pcapng [report.json]\n";return 2; }
  RSDriverParam p;p.lidar_type=LidarType::RSEM4;p.input_type=InputType::PCAP_FILE;
  p.input_param.pcap_path=argv[1];p.input_param.use_vlan=true;
  p.input_param.msop_port=34870;p.input_param.difop_port=34872;
  p.input_param.pcap_repeat=false;p.input_param.pcap_rate=20;
  p.decoder_param.wait_for_difop=true;p.decoder_param.dense_points=true;
  p.decoder_param.use_lidar_clock=true;p.decoder_param.min_distance=.2f;p.decoder_param.max_distance=300;
  std::mutex lock;std::condition_variable changed;bool ended=false;
  uint64_t frames=0,points=0,msop=0,difop=0,overflow=0,bad=0;std::set<int> rings;
  LidarDriver<Cloud> driver;
  driver.regPointCloudCallback([](){return std::make_shared<Cloud>();},[&](std::shared_ptr<Cloud> cloud){
    ++frames;points+=cloud->points.size();
    for (const auto& point:cloud->points) {
      rings.insert(point.ring);if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)) ++bad;
    }
  });
  driver.regPacketCallback([&](const Packet& packet){if(packet.is_difop) ++difop;else ++msop;});
  driver.regExceptionCallback([&](const Error& error){
    if (error.error_code==ERRCODE_PCAPEXIT) {std::lock_guard<std::mutex> guard(lock);ended=true;changed.notify_all();}
    if (error.error_code==ERRCODE_PKTBUFOVERFLOW || error.error_code==ERRCODE_CLOUDOVERFLOW) ++overflow;
  });
  if(!driver.init(p)||!driver.start()) return 3;
  { std::unique_lock<std::mutex> guard(lock);if(!changed.wait_for(guard,std::chrono::seconds(45),[&](){return ended;})) return 4; }
  // EOF is emitted by the input thread before the decoder drains its queue.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  driver.stop();
  std::string json="{\"frames\":"+std::to_string(frames)+",\"points\":"+std::to_string(points)+
    ",\"msop_packets\":"+std::to_string(msop)+",\"difop_packets\":"+std::to_string(difop)+
    ",\"rings\":"+std::to_string(rings.size())+",\"overflow\":"+std::to_string(overflow)+
    ",\"nonfinite_points\":"+std::to_string(bad)+"}";
  std::cout<<json<<'\n';if(argc>2) std::ofstream(argv[2])<<json<<'\n';
  return frames>=90 && points>20000000 && msop==157556 && difop==5 && rings.size()==520 && !overflow && !bad ? 0:1;
}
