#pragma once
#include "pointcloud_renderer.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace ulv {
// The CPU legend and GLSL shader use the same palette stops.
inline constexpr float turbo_stops[5][3] = {
    {0.08f,0.08f,0.45f},{0.0f,0.75f,1.0f},{0.1f,0.95f,0.35f},
    {1.0f,0.85f,0.05f},{0.85f,0.05f,0.03f}
};
inline std::string turbo_glsl() {
    std::string code="vec3 turbo(float x) { const vec3 stops[5] = vec3[5](";
    for(int i=0;i<5;++i) {
        if(i) code+=",";
        code+="vec3("+std::to_string(turbo_stops[i][0])+","+std::to_string(turbo_stops[i][1])+","+std::to_string(turbo_stops[i][2])+")";
    }
    return code+"); float p=clamp(x,0.0,1.0)*4.0; int i=min(int(p),3); return mix(stops[i],stops[i+1],p-float(i)); }\n";
}
struct LegendRange {const char* title; float minimum, maximum; bool visible=true; bool categorical=false;};
inline LegendRange legend_range(const RenderSettings& s) {
    switch(s.color_mode) {
        case ColorMode::HeightTurbo: return {"高度 (m)",s.current_z_min,s.current_z_max};
        case ColorMode::IntensityGray: return {"反射强度",0,s.current_intensity_max};
        case ColorMode::DistanceTurbo: return {"距离 (m)",0,s.max_distance};
        case ColorMode::RingChannel: return {"扫描线",0,0,true,true};
        default: return {"",0,0,false};
    }
}
inline Vec3 legend_color(const RenderSettings& s,float t) {
    t=std::clamp(t,0.0f,1.0f);
    if(s.color_mode==ColorMode::IntensityGray) {float v=std::pow(t,0.55f);return {v,v,v};}
    float p=t*4;int i=std::min(int(p),3);float f=p-i;
    return {turbo_stops[i][0]*(1-f)+turbo_stops[i+1][0]*f,
            turbo_stops[i][1]*(1-f)+turbo_stops[i+1][1]*f,
            turbo_stops[i][2]*(1-f)+turbo_stops[i+1][2]*f};
}
}
