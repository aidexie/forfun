// Engine/Components/MeshRenderer.cpp
#include "MeshRenderer.h"
#include "../../Core/Renderer.h"
#include "../../Core/Mesh.h"
#include "../../Core/ObjLoader.h"
#include "../../Core/GltfLoader.h"
// 如需贴图：#include "../../Core/TextureLoader.h"

#include <algorithm>
using namespace DirectX;

static MeshCPU_PNT MakePlane(float size = 10.0f) {
    MeshCPU_PNT m; float s = size * 0.5f;
    VertexPNT v[4] = {
        {-s,0,-s, 0,1,0, 0,0, 1,0,0,1},
        { s,0,-s, 0,1,0, 1,0, 1,0,0,1},
        { s,0, s, 0,1,0, 1,1, 1,0,0,1},
        {-s,0, s, 0,1,0, 0,1, 1,0,0,1},
    };
    m.vertices.assign(v, v + 4);
    uint32_t idx[6] = { 0,1,2, 0,2,3 };
    m.indices.assign(idx, idx + 6);
    ComputeTangents(m.vertices, m.indices);
    return m;
}

static MeshCPU_PNT MakeCube(float s = 1.0f) {
    MeshCPU_PNT m; float h = s * 0.5f;
    struct V { float x, y, z, nx, ny, nz, u, v; };
    V vv[] = {
        { h,-h,-h, 1,0,0,0,1},{ h, h,-h,1,0,0,0,0},{ h, h, h,1,0,0,1,0},{ h,-h, h,1,0,0,1,1},
        {-h,-h, h,-1,0,0,0,1},{-h, h, h,-1,0,0,0,0},{-h, h,-h,-1,0,0,1,0},{-h,-h,-h,-1,0,0,1,1},
        {-h, h,-h,0,1,0,0,1},{ h, h,-h,0,1,0,1,1},{ h, h, h,0,1,0,1,0},{-h, h, h,0,1,0,0,0},
        {-h,-h, h,0,-1,0,0,1},{ h,-h, h,0,-1,0,1,1},{ h,-h,-h,0,-1,0,1,0},{-h,-h,-h,0,-1,0,0,0},
        {-h,-h, h,0,0,1,0,1},{ h,-h, h,0,0,1,1,1},{ h, h, h,0,0,1,1,0},{-h, h, h,0,0,1,0,0},
        { h,-h,-h,0,0,-1,0,1},{-h,-h,-h,0,0,-1,1,1},{-h, h,-h,0,0,-1,1,0},{ h, h,-h,0,0,-1,0,0},
    };
    for (auto& a : vv) { VertexPNT t{ a.x,a.y,a.z,a.nx,a.ny,a.nz,a.u,a.v,1,0,0,1 }; m.vertices.push_back(t); }
    uint32_t idx[] = { 0,1,2,0,2,3, 4,5,6,4,6,7, 8,9,10,8,10,11, 12,13,14,12,14,15, 16,17,18,16,18,19, 20,21,22,20,22,23 };
    m.indices.assign(idx, idx + 36);
    ComputeTangents(m.vertices, m.indices);
    return m;
}

bool MeshRenderer::EnsureUploaded(Renderer& r, const XMMATRIX& world) {
    if (!indices.empty()) return true;
    indices.clear();

    if (kind == MeshKind::Plane) {
        indices.push_back(r.AddMesh(MakePlane(20.0f), world));
        return true;
    }
    if (kind == MeshKind::Cube) {
        indices.push_back(r.AddMesh(MakeCube(1.0f), world));
        return true;
    }

    // kind==Obj：根据扩展名在此选择 .obj / .gltf
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".obj") {
        MeshCPU_PNT m;
        if (!LoadOBJ_PNT(path, m, /*flipZ*/true, /*flipWinding*/true)) return false;
        RecenterAndScale(m, 2.0f);
        indices.push_back(r.AddMesh(m, world));
        return true;
    }
    if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".gltf") {
        std::vector<GltfMeshCPU> meshes;
        if (!LoadGLTF_PNT(path, meshes, /*flipZ_to_LH*/true, /*flipWinding*/true)) return false;
        for (auto& gm : meshes) {
            size_t idx = r.AddMesh(gm, world); // 使用新的重载，自动加载纹理
            indices.push_back(idx);
        }
        return !indices.empty();
    }
    return false;
}

void MeshRenderer::UpdateTransform(Renderer& r, const XMMATRIX& world) {
    for (auto idx : indices) r.SetMeshWorld(idx, world);
}
