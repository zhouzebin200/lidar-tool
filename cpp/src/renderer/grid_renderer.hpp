#pragma once

#include "math3d.hpp"
#include <cstdint>
#include <vector>

namespace ulv {

class GridRenderer {
public:
    GridRenderer();
    ~GridRenderer();

    bool init();
    void cleanup();

    void render(const Mat4& mvp);

private:
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    uint32_t shader_program_ = 0;
    int u_mvp_loc_ = -1;
    size_t vertex_count_ = 0;

    struct Vertex {
        float x, y, z;
        float r, g, b, a;
    };

    void generate_geometry(std::vector<Vertex>& out_vertices);
    bool compile_shaders();
};

} // namespace ulv
