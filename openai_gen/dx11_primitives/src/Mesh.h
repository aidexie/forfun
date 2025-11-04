
#pragma once
#include <vector>
#include <cstdint>
#include <DirectXMath.h>

struct VertexPC {
    float x, y, z;
    float r, g, b;
};

struct MeshCPU {
    std::vector<VertexPC> vertices;
    std::vector<uint32_t> indices;
};

MeshCPU MakeCube(float size);
MeshCPU MakeCuboid(float w, float h, float d);
MeshCPU MakeCylinder(float radius, float height, uint32_t slices, bool capTop = true, bool capBottom = true);
MeshCPU MakeSphere(float radius, uint32_t slices, uint32_t stacks);
