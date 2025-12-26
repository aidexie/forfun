#pragma once
#include "LightmapTypes.h"
#include <vector>
#include <DirectXMath.h>

struct SVertexPNT;
struct SMeshCPU_PNT;

// ============================================
// Lightmap UV2 Generation (xatlas wrapper)
// ============================================

// Generate UV2 for a mesh using xatlas
//
// Parameters:
//   mesh        - Input mesh data (positions, normals, uvs, indices)
//   texelsPerUnit - Target texel density (texels per world unit)
//
// Returns:
//   SUV2GenerationResult with new vertex/index data including UV2
//   Note: xatlas may split vertices at UV seams, so vertex count may increase
//
SUV2GenerationResult GenerateUV2ForMesh(
    const SMeshCPU_PNT& mesh,
    int texelsPerUnit = 16
);

// Generate UV2 for raw vertex/index data
SUV2GenerationResult GenerateUV2(
    const std::vector<DirectX::XMFLOAT3>& positions,
    const std::vector<DirectX::XMFLOAT3>& normals,
    const std::vector<DirectX::XMFLOAT2>& uvs,
    const std::vector<uint32_t>& indices,
    int texelsPerUnit = 16
);

// Check if a mesh already has valid UV2 (non-overlapping, in [0,1] range)
bool HasValidUV2(
    const std::vector<DirectX::XMFLOAT2>& uv2,
    const std::vector<uint32_t>& indices
);
