#include "pointcloud_renderer.hpp"
#include "color_mapping.hpp"
#include "ulv_gl.hpp"

#include <iostream>
#include <vector>

namespace ulv {

static const char* kVertexShaderSource = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aIntensity;
layout(location = 2) in uint aRing;
layout(location = 3) in uint aReturnType;
layout(location = 4) in uint aFlags;

uniform mat4 uMVP;
uniform int uColorMode;
uniform float uPointSize;
uniform float uMaxDistance;
uniform float uZMin;
uniform float uZMax;
uniform float uIntensityMax;
uniform vec3 uRoiMin;
uniform vec3 uRoiMax;
uniform int uRoiEnable;

out float vValue;
out float vVisible;

void main() {
    float range = length(aPos);
    vVisible = (range <= uMaxDistance) ? 1.0 : 0.0;

    if (uRoiEnable == 1) {
        if (aPos.x < uRoiMin.x || aPos.x > uRoiMax.x ||
            aPos.y < uRoiMin.y || aPos.y > uRoiMax.y ||
            aPos.z < uRoiMin.z || aPos.z > uRoiMax.z) {
            vVisible = 0.0;
        }
    }

    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;

    if (uColorMode == 0) {
        // 高度 · Turbo 伪彩 (与网页端一致)
        vValue = clamp((aPos.z - uZMin) / max(0.01, uZMax - uZMin), 0.0, 1.0);
    } else if (uColorMode == 1) {
        // 反射强度 · 灰度 (0.0 ~ 1.0)
        vValue = clamp(aIntensity / max(0.01, uIntensityMax), 0.0, 1.0);
    } else if (uColorMode == 2) {
        // 距离 · Turbo 伪彩
        vValue = clamp(range / max(1.0, uMaxDistance), 0.0, 1.0);
    } else if (uColorMode == 3) {
        // 扫描线 / Ring 分类
        vValue = fract(float(aRing) * 0.61803398875);
    } else {
        vValue = 0.5;
    }
}
)";

static const std::string kFragmentShaderSource = std::string(R"(#version 330 core
in float vValue;
in float vVisible;

uniform int uColorMode;
uniform int uCircular;

out vec4 FragColor;

)") + turbo_glsl() + R"(
// 网页端原生分类黄金分割色
vec3 categorical(float h) {
    vec3 rgb = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return mix(vec3(1.0), rgb, 0.78);
}

void main() {
    if (vVisible < 0.5) {
        discard;
    }

    if (uCircular == 1) {
        vec2 p = gl_PointCoord * 2.0 - vec2(1.0);
        if (dot(p, p) > 1.0) {
            discard;
        }
    }

    vec3 color;
    if (uColorMode == 0) {
        color = turbo(vValue);
    } else if (uColorMode == 1) {
        // 反射率采用 Gamma 0.55 矫正，增强暗部弱反光细节
        color = vec3(pow(vValue, 0.55));
    } else if (uColorMode == 2) {
        color = turbo(vValue);
    } else if (uColorMode == 3) {
        color = categorical(vValue);
    } else {
        color = vec3(0.85, 0.95, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
)";

PointCloudRenderer::PointCloudRenderer() = default;

PointCloudRenderer::~PointCloudRenderer() {
    cleanup();
}

bool PointCloudRenderer::init() {
    if (!compile_shaders()) {
        return false;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    // ulv_point_v1 stride = 20 bytes
    const GLsizei stride = sizeof(UlvPoint);

    // 0: vec3 aPos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(UlvPoint, x));

    // 1: float aIntensity
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(UlvPoint, intensity));

    // 2: uint aRing
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_SHORT, stride, (void*)offsetof(UlvPoint, ring));

    // 3: uint aReturnType
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, stride, (void*)offsetof(UlvPoint, return_type));

    // 4: uint aFlags
    glEnableVertexAttribArray(4);
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_BYTE, stride, (void*)offsetof(UlvPoint, flags));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void PointCloudRenderer::cleanup() {
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (shader_program_) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    current_point_count_ = 0;
    vbo_capacity_ = 0;
}

void PointCloudRenderer::upload_points(const UlvPoint* data, size_t count) {
    if (!data || count == 0) {
        current_point_count_ = 0;
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (count > vbo_capacity_) {
        vbo_capacity_ = count + (count / 5);
        glBufferData(GL_ARRAY_BUFFER, vbo_capacity_ * sizeof(UlvPoint), nullptr, GL_DYNAMIC_DRAW);
    }

    glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(UlvPoint), data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    current_point_count_ = count;
}

void PointCloudRenderer::render(const Mat4& mvp, const RenderSettings& settings) {
    if (current_point_count_ == 0 || shader_program_ == 0 || vao_ == 0) {
        return;
    }

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(shader_program_);

    glUniformMatrix4fv(u_mvp_loc_, 1, GL_FALSE, mvp.m.data());
    glUniform1i(u_color_mode_loc_, static_cast<int>(settings.color_mode));
    glUniform1f(u_point_size_loc_, settings.point_size);
    glUniform1i(u_circular_loc_, settings.circular_points ? 1 : 0);

    glUniform1f(u_max_distance_loc_, settings.max_distance);
    glUniform1f(u_z_min_loc_, settings.current_z_min);
    glUniform1f(u_z_max_loc_, settings.current_z_max);
    glUniform1f(u_intensity_max_loc_, settings.current_intensity_max);

    glUniform1i(u_roi_enable_loc_, settings.enable_roi_filter ? 1 : 0);
    glUniform3f(u_roi_min_loc_, settings.roi_min_x, settings.roi_min_y, settings.roi_min_z);
    glUniform3f(u_roi_max_loc_, settings.roi_max_x, settings.roi_max_y, settings.roi_max_z);

    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(current_point_count_));
    glBindVertexArray(0);

    glUseProgram(0);
}

bool PointCloudRenderer::compile_shaders() {
    auto check_compile = [](GLuint shader, const char* name) -> bool {
        GLint success = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cerr << "Shader compile error (" << name << "): " << log << std::endl;
            return false;
        }
        return true;
    };

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVertexShaderSource, nullptr);
    glCompileShader(vs);
    if (!check_compile(vs, "Vertex")) return false;

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* fragment_source=kFragmentShaderSource.c_str();
    glShaderSource(fs, 1, &fragment_source, nullptr);
    glCompileShader(fs);
    if (!check_compile(fs, "Fragment")) return false;

    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vs);
    glAttachShader(shader_program_, fs);
    glLinkProgram(shader_program_);

    GLint linked = 0;
    glGetProgramiv(shader_program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(shader_program_, sizeof(log), nullptr, log);
        std::cerr << "Program link error: " << log << std::endl;
        return false;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    u_mvp_loc_ = glGetUniformLocation(shader_program_, "uMVP");
    u_color_mode_loc_ = glGetUniformLocation(shader_program_, "uColorMode");
    u_point_size_loc_ = glGetUniformLocation(shader_program_, "uPointSize");
    u_circular_loc_ = glGetUniformLocation(shader_program_, "uCircular");

    u_max_distance_loc_ = glGetUniformLocation(shader_program_, "uMaxDistance");
    u_z_min_loc_ = glGetUniformLocation(shader_program_, "uZMin");
    u_z_max_loc_ = glGetUniformLocation(shader_program_, "uZMax");
    u_intensity_max_loc_ = glGetUniformLocation(shader_program_, "uIntensityMax");

    u_roi_min_loc_ = glGetUniformLocation(shader_program_, "uRoiMin");
    u_roi_max_loc_ = glGetUniformLocation(shader_program_, "uRoiMax");
    u_roi_enable_loc_ = glGetUniformLocation(shader_program_, "uRoiEnable");

    return true;
}

} // namespace ulv
