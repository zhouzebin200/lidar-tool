#include "grid_renderer.hpp"
#include "ulv_gl.hpp"
#include <cmath>
#include <iostream>

namespace ulv {

static const char* kGridVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

uniform mat4 uMVP;
out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
}
)";

static const char* kGridFS = R"(#version 330 core
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)";

GridRenderer::GridRenderer() = default;

GridRenderer::~GridRenderer() {
    cleanup();
}

bool GridRenderer::init() {
    if (!compile_shaders()) return false;

    std::vector<Vertex> vertices;
    generate_geometry(vertices);
    vertex_count_ = vertices.size();

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    // 0: vec3 aPos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, x));

    // 1: vec4 aColor
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, r));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void GridRenderer::cleanup() {
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
    vertex_count_ = 0;
}

void GridRenderer::render(const Mat4& mvp) {
    if (vertex_count_ == 0 || shader_program_ == 0 || vao_ == 0) return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(shader_program_);
    glUniformMatrix4fv(u_mvp_loc_, 1, GL_FALSE, mvp.m.data());

    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertex_count_));
    glBindVertexArray(0);

    glUseProgram(0);
}

void GridRenderer::generate_geometry(std::vector<Vertex>& out) {
    const float kGridSize = 50.0f;
    const float kGridStep = 5.0f;
    const Vertex kGridColor = {0, 0, 0, 0.2f, 0.2f, 0.25f, 0.4f};

    // 1. Ground Grid lines (-50m to +50m)
    for (float x = -kGridSize; x <= kGridSize; x += kGridStep) {
        if (std::abs(x) < 0.01f) continue; // Axis line handled separately
        out.push_back({x, -kGridSize, 0.0f, kGridColor.r, kGridColor.g, kGridColor.b, kGridColor.a});
        out.push_back({x,  kGridSize, 0.0f, kGridColor.r, kGridColor.g, kGridColor.b, kGridColor.a});
    }
    for (float y = -kGridSize; y <= kGridSize; y += kGridStep) {
        if (std::abs(y) < 0.01f) continue;
        out.push_back({-kGridSize, y, 0.0f, kGridColor.r, kGridColor.g, kGridColor.b, kGridColor.a});
        out.push_back({ kGridSize, y, 0.0f, kGridColor.r, kGridColor.g, kGridColor.b, kGridColor.a});
    }

    // 2. Concentric Distance Rings: 10m, 20m, 30m, 50m, 100m
    const std::vector<float> kRadii = {10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
    const int kSegments = 120;
    for (float r : kRadii) {
        float alpha = (r <= 30.0f) ? 0.6f : 0.4f;
        for (int i = 0; i < kSegments; ++i) {
            float theta1 = (2.0f * 3.14159265f * i) / kSegments;
            float theta2 = (2.0f * 3.14159265f * (i + 1)) / kSegments;
            out.push_back({r * std::cos(theta1), r * std::sin(theta1), 0.0f, 0.35f, 0.45f, 0.55f, alpha});
            out.push_back({r * std::cos(theta2), r * std::sin(theta2), 0.0f, 0.35f, 0.45f, 0.55f, alpha});
        }
    }

    // 3. Coordinate Axes at (0,0,0)
    const float kAxisLen = 6.0f;
    // X axis: Red
    out.push_back({0.0f, 0.0f, 0.0f, 0.95f, 0.2f, 0.2f, 1.0f});
    out.push_back({kAxisLen, 0.0f, 0.0f, 0.95f, 0.2f, 0.2f, 1.0f});

    // Y axis: Green
    out.push_back({0.0f, 0.0f, 0.0f, 0.2f, 0.95f, 0.2f, 1.0f});
    out.push_back({0.0f, kAxisLen, 0.0f, 0.2f, 0.95f, 0.2f, 1.0f});

    // Z axis: Blue
    out.push_back({0.0f, 0.0f, 0.0f, 0.25f, 0.5f, 1.0f, 1.0f});
    out.push_back({0.0f, 0.0f, kAxisLen, 0.25f, 0.5f, 1.0f, 1.0f});
}

bool GridRenderer::compile_shaders() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kGridVS, nullptr);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kGridFS, nullptr);
    glCompileShader(fs);

    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vs);
    glAttachShader(shader_program_, fs);
    glLinkProgram(shader_program_);

    glDeleteShader(vs);
    glDeleteShader(fs);

    u_mvp_loc_ = glGetUniformLocation(shader_program_, "uMVP");
    return (u_mvp_loc_ >= 0);
}

} // namespace ulv
