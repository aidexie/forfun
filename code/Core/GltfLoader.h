#pragma once
#include <string>
#include <vector>
#include "Mesh.h"

struct GltfTextures {
    std::string baseColorPath;          // sRGB (Albedo)
    std::string normalPath;             // Linear (Tangent-space normal)
    std::string metallicRoughnessPath;  // Linear (G=Roughness, B=Metallic, glTF 2.0 standard)
};

struct SGltfMeshCPU {
    SMeshCPU_PNT mesh;     // 我们现有的 P/N/UV + tangent.w
    GltfTextures textures;
    // 可扩展 metallic-roughness、ao 等
};

// 返回多个实例化后的 mesh（已应用节点变换合并到 world 中的话，可扩展）
bool LoadGLTF_PNT(const std::string& gltfPath,
                  std::vector<SGltfMeshCPU>& outMeshes,
                  bool flipZ_to_LH = true, bool flipWinding = true);
