
#pragma once
#include <vector>
#include <cstdint>

struct SVertexPNT {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz;
    float tw;
    float r, g, b, a;  // Vertex color (for baked AO or other per-vertex data)
};

struct SMeshCPU_PNT {
    std::vector<SVertexPNT> vertices;
    std::vector<uint32_t> indices;
};

void ComputeTangents(std::vector<SVertexPNT>& vtx, const std::vector<uint32_t>& idx);
