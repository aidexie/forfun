
#pragma once
#include <vector>
#include <cstdint>

struct VertexPNT {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz;
    float tw;
};

struct MeshCPU_PNT {
    std::vector<VertexPNT> vertices;
    std::vector<uint32_t> indices;
};

void ComputeTangents(std::vector<VertexPNT>& vtx, const std::vector<uint32_t>& idx);
