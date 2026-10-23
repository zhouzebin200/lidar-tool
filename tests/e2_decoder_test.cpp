#include "robosense_e2.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
struct Capture { std::vector<std::vector<ulv_point_v1>> frames; uint64_t ts{}; };
void collect(const ulv_frame_v1* f, void* p) { auto& c=*static_cast<Capture*>(p); assert(std::string(f->model)=="RSE2"); c.frames.emplace_back(f->points,f->points+f->point_count);c.ts=f->timestamp_ns; }
void be16(std::vector<uint8_t>& p,size_t i,unsigned n){p[i]=n>>8;p[i+1]=n;}
std::vector<uint8_t> packet(unsigned frame=0,unsigned seq=1,bool dual=false){
 std::vector<uint8_t> p(844);p[0]=0x55;p[1]=0xaa;p[2]=0x5a;p[3]=0xa5;be16(p,4,seq);p[6]=dual?0:1;p[13]=12;be16(p,16,345);p[20]=frame;p[21]=10;p[22]=10;
 for(unsigned i=0;i<200;i++){be16(p,32+i*4,dual&&i%2?4000:2000);p[34+i*4]=128;}return p;
}
uint32_t crc(const std::vector<uint8_t>& p){uint32_t poly=0;for(int i=0;i<32;i++)poly|=((0xf4acfb13u>>i)&1)<<(31-i);uint32_t v=~0u;for(size_t i=0;i<p.size()-4;i++){v^=p[i];for(int j=0;j<8;j++)v=(v>>1)^((v&1)?poly:0);}return ~v;}
std::vector<uint8_t> angles(unsigned n){std::vector<uint8_t> p(1296);p[0]=0xa5;p[1]=0xff;p[3]=0xae;for(unsigned i=0;i<640;i++)be16(p,4+i*2,(((n-1)*640+i)%3)==0?32767:0);be16(p,1286,n);uint32_t c=crc(p);for(int i=0;i<4;i++)p[1292+i]=c>>(24-8*i);return p;}
void feed(E2Decoder& d,const std::vector<uint8_t>& p){d.feed(p.data(),p.size(),999);}
int main(){
 {Capture c;E2Decoder d(7,collect,&c);feed(d,packet());assert(c.frames.empty());feed(d,packet(1));assert(c.frames.size()==1&&c.frames[0].size()==200);assert(std::fabs(c.frames[0][0].x-26762*10.f/32768)<1e-5);assert(std::fabs(c.frames[0][0].y-17969*10.f/32768)<1e-5);assert(std::fabs(c.frames[0][0].z+5889*10.f/32768)<1e-5);assert(c.frames[0][0].ring==100);assert(c.ts==12000345000ULL);}
 {Capture c;E2Decoder d(7,collect,&c);feed(d,packet(0,1,true));feed(d,packet(1,1,true));auto& f=c.frames[0];assert(f.size()==200);assert(f[0].return_type==1&&f[1].return_type==2);assert(std::fabs(f[1].x-2*f[0].x)<1e-5);assert(f[0].ring==f[1].ring);}
 {Capture c;E2Decoder d(7,collect,&c);for(unsigned i=1;i<=360;i++)feed(d,angles(i));feed(d,packet());feed(d,packet(1));assert(std::fabs(c.frames[0][0].x-32767*10.f/32768)<1e-5);assert(c.frames[0][0].y==0);}
 {Capture c;E2Decoder d(7,collect,&c);for(unsigned i=1;i<=360;i++){auto a=angles(i);if(i==100)a[100]^=1;feed(d,a);}feed(d,packet());feed(d,packet(1));assert(std::fabs(c.frames[0][0].y-17969*10.f/32768)<1e-5);for(unsigned i=1;i<=360;i++)feed(d,angles(i));feed(d,packet(2));feed(d,packet(3));assert(c.frames.back()[0].y==0);}
 {Capture c;E2Decoder d(7,collect,&c);for(unsigned i=1;i<=360;i++){if(i!=90)feed(d,angles(i));}feed(d,packet());feed(d,packet());auto bad=packet();bad[21]=24;feed(d,bad);bad=packet();bad[22]=32;feed(d,bad);bad.resize(843);feed(d,bad);d.feed(nullptr,844,0);feed(d,packet(1));assert(c.frames[0].size()==200);assert(c.frames[0][0].y!=0);}
 {Capture c;E2Decoder d(7,collect,&c);auto p=packet();for(unsigned i=0;i<200;i++)be16(p,32+i*4,0);feed(d,p);feed(d,packet(1));assert(c.frames.empty());}
 {Capture c;E2Decoder d(7,collect,&c);auto p=packet();p[21]=0;p[22]=0;feed(d,p);feed(d,packet(1));for(auto& f:c.frames)for(auto& pt:f)assert(pt.x*pt.x+pt.y*pt.y+pt.z*pt.z>0);}
 {Capture c;E2Decoder d(7,collect,&c);auto p=packet(100);d.feed(p.data(),p.size(),1000000000ULL);p=packet(0);d.feed(p.data(),p.size(),3000000000ULL);p=packet(1);d.feed(p.data(),p.size(),3001000000ULL);assert(c.frames.size()==1&&c.frames[0].size()==200);}
 {Capture c;E2Decoder d(7,collect,&c);auto p=packet(100);d.feed(p.data(),p.size(),3000000000ULL);p=packet(0);d.feed(p.data(),p.size(),1000000000ULL);p=packet(1);d.feed(p.data(),p.size(),1001000000ULL);assert(c.frames.size()==1&&c.frames[0].size()==200);}
 {Capture c;E2Decoder d(7,collect,&c);feed(d,packet(255));feed(d,packet(0));feed(d,packet(255,2));feed(d,packet(1));assert(c.frames.size()==2&&c.frames[1].size()==200);}
 std::cout<<"10 E2 native scenarios passed\n";
}
