
#include "Mesh.h"
#include <cmath>
using namespace DirectX;

static VertexPC V(float x,float y,float z, float r,float g,float b){ return {x,y,z,r,g,b}; }

MeshCPU MakeCube(float size) {
    const float s = size * 0.5f;
    std::vector<VertexPC> v = {
        V(-s,-s,-s, 1,0,0), V(-s, s,-s, 0,1,0), V( s, s,-s, 0,0,1), V( s,-s,-s, 1,1,0),
        V(-s,-s, s, 1,0,1), V(-s, s, s, 0,1,1), V( s, s, s, 1,1,1), V( s,-s, s, 0,0,0)
    };
    std::vector<uint32_t> i = {
        0,1,2,  0,2,3,
        4,6,5,  4,7,6,
        0,5,1,  0,4,5,
        3,2,6,  3,6,7,
        0,3,7,  0,7,4,
        1,5,6,  1,6,2
    };
    return {std::move(v), std::move(i)};
}

MeshCPU MakeCuboid(float w, float h, float d) {
    const float hw = w*0.5f, hh = h*0.5f, hd = d*0.5f;
    std::vector<VertexPC> v = {
        V(-hw,-hh,-hd, 1,0,0), V(-hw, hh,-hd, 0,1,0), V( hw, hh,-hd, 0,0,1), V( hw,-hh,-hd, 1,1,0),
        V(-hw,-hh, hd, 1,0,1), V(-hw, hh, hd, 0,1,1), V( hw, hh, hd, 1,1,1), V( hw,-hh, hd, 0,0,0)
    };
    std::vector<uint32_t> i = {
        0,1,2,  0,2,3,
        4,6,5,  4,7,6,
        0,5,1,  0,4,5,
        3,2,6,  3,6,7,
        0,3,7,  0,7,4,
        1,5,6,  1,6,2
    };
    return {std::move(v), std::move(i)};
}

MeshCPU MakeCylinder(float radius, float height, uint32_t slices, bool capTop, bool capBottom) {
    MeshCPU m;
    const float hh = height * 0.5f;
    uint32_t baseIndex = 0;
    for (uint32_t s=0; s<=slices; ++s) {
        float a = (float)s / slices * XM_2PI;
        float x = std::cos(a) * radius;
        float z = std::sin(a) * radius;
        float r = 0.5f*(std::cos(a)+1.f), g = 0.5f*(std::sin(a)+1.f), b = 1.f-r;
        m.vertices.push_back({x, -hh, z, r,g,b});
        m.vertices.push_back({x,  hh, z, r,g,b});
    }
    for (uint32_t s=0; s<slices; ++s) {
        uint32_t i0 = baseIndex + s*2;
        uint32_t i1 = i0 + 1;
        uint32_t i2 = i0 + 2;
        uint32_t i3 = i0 + 3;
        m.indices.insert(m.indices.end(), { i0,i2,i1,  i1,i2,i3 });
    }
    if (capTop) {
        uint32_t center = (uint32_t)m.vertices.size();
        m.vertices.push_back({0, hh, 0, 1,1,1});
        for (uint32_t s=0; s<=slices; ++s) {
            float a = (float)s / slices * XM_2PI;
            m.vertices.push_back({std::cos(a)*radius, hh, std::sin(a)*radius, 1,1,1});
        }
        for (uint32_t s=0; s<slices; ++s) {
            m.indices.insert(m.indices.end(), { center, center+1+s, center+1+s+1 });
        }
    }
    if (capBottom) {
        uint32_t center = (uint32_t)m.vertices.size();
        m.vertices.push_back({0, -hh, 0, 0.2f,0.2f,0.2f});
        for (uint32_t s=0; s<=slices; ++s) {
            float a = (float)s / slices * XM_2PI;
            m.vertices.push_back({std::cos(a)*radius, -hh, std::sin(a)*radius, 0.2f,0.2f,0.2f});
        }
        for (uint32_t s=0; s<slices; ++s) {
            m.indices.insert(m.indices.end(), { center, center+1+s+1, center+1+s });
        }
    }
    return m;
}

MeshCPU MakeSphere(float radius, uint32_t slices, uint32_t stacks) {
    MeshCPU m;
    for (uint32_t y=0; y<=stacks; ++y) {
        float v = (float)y / stacks;
        float phi = v * XM_PI;
        float yPos = radius * std::cos(phi - XM_PIDIV2);
        float r = radius * std::sin(phi);
        for (uint32_t x=0; x<=slices; ++x) {
            float u = (float)x / slices;
            float theta = u * XM_2PI;
            float xPos = r * std::cos(theta);
            float zPos = r * std::sin(theta);
            float cr = 0.5f*(xPos/radius + 1.f);
            float cg = 0.5f*(yPos/radius + 1.f);
            float cb = 0.5f*(zPos/radius + 1.f);
            m.vertices.push_back({xPos, yPos, zPos, cr, cg, cb});
        }
    }
    uint32_t row = slices + 1;
    for (uint32_t y=0; y<stacks; ++y) {
        for (uint32_t x=0; x<slices; ++x) {
            uint32_t i0 = y*row + x;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + row;
            uint32_t i3 = i2 + 1;
            m.indices.insert(m.indices.end(), { i0,i2,i1,  i1,i2,i3 });
        }
    }
    return m;
}
