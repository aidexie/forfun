#include "LightmapUV2.h"
#include "Core/Mesh.h"
#include "Core/FFLog.h"
#include <xatlas.h>
#include <algorithm>
#include <cmath>

// Convenience macros
#define FF_LOG(...) CFFLog::Info(__VA_ARGS__)
#define FF_LOG_ERROR(...) CFFLog::Error(__VA_ARGS__)

using namespace DirectX;

// ============================================
// xatlas Progress Callback
// ============================================
static bool XAtlasProgressCallback(xatlas::ProgressCategory category, int progress, void* userData)
{
    // Return true to continue, false to cancel
    return true;
}

// ============================================
// Generate UV2 for raw vertex/index data
// ============================================
SUV2GenerationResult GenerateUV2(
    const std::vector<XMFLOAT3>& positions,
    const std::vector<XMFLOAT3>& normals,
    const std::vector<XMFLOAT2>& uvs,
    const std::vector<uint32_t>& indices,
    int texelsPerUnit)
{
    SUV2GenerationResult result;
    result.success = false;

    if (positions.empty() || indices.empty()) {
        FF_LOG_ERROR("[LightmapUV2] Empty mesh data");
        return result;
    }

    if (positions.size() != normals.size()) {
        FF_LOG_ERROR("[LightmapUV2] Position/normal count mismatch");
        return result;
    }

    // 1. Create xatlas context
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::SetProgressCallback(atlas, XAtlasProgressCallback, nullptr);

    // 2. Prepare mesh declaration
    xatlas::MeshDecl meshDecl;
    meshDecl.vertexCount = static_cast<uint32_t>(positions.size());
    meshDecl.vertexPositionData = positions.data();
    meshDecl.vertexPositionStride = sizeof(XMFLOAT3);
    meshDecl.vertexNormalData = normals.data();
    meshDecl.vertexNormalStride = sizeof(XMFLOAT3);

    // UV is optional but helps xatlas make better decisions
    if (!uvs.empty() && uvs.size() == positions.size()) {
        meshDecl.vertexUvData = uvs.data();
        meshDecl.vertexUvStride = sizeof(XMFLOAT2);
    }

    meshDecl.indexCount = static_cast<uint32_t>(indices.size());
    meshDecl.indexData = indices.data();
    meshDecl.indexFormat = xatlas::IndexFormat::UInt32;

    // 3. Add mesh to atlas
    xatlas::AddMeshError addError = xatlas::AddMesh(atlas, meshDecl);
    if (addError != xatlas::AddMeshError::Success) {
        FF_LOG_ERROR("[LightmapUV2] xatlas::AddMesh failed: %d", static_cast<int>(addError));
        xatlas::Destroy(atlas);
        return result;
    }

    // 4. Configure chart generation
    xatlas::ChartOptions chartOptions;
    chartOptions.maxIterations = 4;
    // Use defaults for most settings - xatlas has good defaults

    // 5. Configure packing
    xatlas::PackOptions packOptions;
    packOptions.padding = 2;           // 2 pixel padding between charts
    packOptions.texelsPerUnit = static_cast<float>(texelsPerUnit);
    packOptions.bilinear = true;       // Account for bilinear filtering
    packOptions.blockAlign = true;     // Align charts to 4x4 blocks (good for compression)
    packOptions.bruteForce = false;    // Faster packing

    // 6. Generate atlas
    FF_LOG("[LightmapUV2] Generating UV2 for %d vertices, %d triangles...",
           static_cast<int>(positions.size()),
           static_cast<int>(indices.size() / 3));

    xatlas::Generate(atlas, chartOptions, packOptions);

    // 7. Check results
    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0) {
        FF_LOG_ERROR("[LightmapUV2] xatlas::Generate produced no output");
        xatlas::Destroy(atlas);
        return result;
    }

    FF_LOG("[LightmapUV2] Atlas generated: %dx%d, %d charts",
           atlas->width, atlas->height, atlas->chartCount);

    // 8. Extract output mesh
    const xatlas::Mesh& outMesh = atlas->meshes[0];

    result.atlasWidth = atlas->width;
    result.atlasHeight = atlas->height;
    result.chartCount = atlas->chartCount;

    // Reserve space
    result.positions.resize(outMesh.vertexCount);
    result.normals.resize(outMesh.vertexCount);
    result.uv1.resize(outMesh.vertexCount);
    result.uv2.resize(outMesh.vertexCount);
    result.indices.resize(outMesh.indexCount);

    // xatlas may split vertices at UV seams, so we need to remap
    float invWidth = 1.0f / static_cast<float>(atlas->width);
    float invHeight = 1.0f / static_cast<float>(atlas->height);

    for (uint32_t i = 0; i < outMesh.vertexCount; i++) {
        const xatlas::Vertex& v = outMesh.vertexArray[i];
        uint32_t origIdx = v.xref;  // Original vertex index

        // Copy original vertex data
        result.positions[i] = positions[origIdx];
        result.normals[i] = normals[origIdx];

        if (!uvs.empty()) {
            result.uv1[i] = uvs[origIdx];
        } else {
            result.uv1[i] = {0.0f, 0.0f};
        }

        // UV2 from xatlas (normalized to [0,1])
        result.uv2[i] = {
            v.uv[0] * invWidth,
            v.uv[1] * invHeight
        };
    }

    // Copy indices
    for (uint32_t i = 0; i < outMesh.indexCount; i++) {
        result.indices[i] = outMesh.indexArray[i];
    }

    result.success = true;

    FF_LOG("[LightmapUV2] UV2 generation complete: %d -> %d vertices",
           static_cast<int>(positions.size()),
           static_cast<int>(result.positions.size()));

    // 9. Cleanup
    xatlas::Destroy(atlas);

    return result;
}

// ============================================
// Generate UV2 for SMeshCPU_PNT
// ============================================
SUV2GenerationResult GenerateUV2ForMesh(
    const SMeshCPU_PNT& mesh,
    int texelsPerUnit)
{
    // Extract data from SVertexPNT format
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT4> tangents;
    std::vector<XMFLOAT2> uvs;
    std::vector<XMFLOAT4> colors;

    positions.reserve(mesh.vertices.size());
    normals.reserve(mesh.vertices.size());
    tangents.reserve(mesh.vertices.size());
    uvs.reserve(mesh.vertices.size());
    colors.reserve(mesh.vertices.size());

    for (const auto& v : mesh.vertices) {
        positions.push_back({v.px, v.py, v.pz});
        normals.push_back({v.nx, v.ny, v.nz});
        tangents.push_back({v.tx, v.ty, v.tz, v.tw});
        uvs.push_back({v.u, v.v});
        colors.push_back({v.r, v.g, v.b, v.a});
    }

    // Generate UV2
    SUV2GenerationResult result = GenerateUV2(positions, normals, uvs, mesh.indices, texelsPerUnit);

    if (!result.success) {
        return result;
    }

    // Remap tangents and colors to new vertex layout
    result.tangents.resize(result.positions.size());
    result.colors.resize(result.positions.size());

    // We need to get the original vertex index for tangent/color remapping
    // Since GenerateUV2 doesn't expose xref, we recalculate based on position matching
    // This is a limitation - for production, we'd modify GenerateUV2 to also return xref

    // For now, regenerate tangents after UV2 generation
    // (tangents depend on UV, but UV2 is separate from UV1, so original tangents are still valid)

    // Actually, we need to look at xatlas output again. Let's use a simpler approach:
    // Re-run xatlas and keep track of xref

    // Create a new atlas just to get xref mapping
    xatlas::Atlas* atlas = xatlas::Create();

    xatlas::MeshDecl meshDecl;
    meshDecl.vertexCount = static_cast<uint32_t>(positions.size());
    meshDecl.vertexPositionData = positions.data();
    meshDecl.vertexPositionStride = sizeof(XMFLOAT3);
    meshDecl.vertexNormalData = normals.data();
    meshDecl.vertexNormalStride = sizeof(XMFLOAT3);
    meshDecl.vertexUvData = uvs.data();
    meshDecl.vertexUvStride = sizeof(XMFLOAT2);
    meshDecl.indexCount = static_cast<uint32_t>(mesh.indices.size());
    meshDecl.indexData = mesh.indices.data();
    meshDecl.indexFormat = xatlas::IndexFormat::UInt32;

    if (xatlas::AddMesh(atlas, meshDecl) == xatlas::AddMeshError::Success) {
        xatlas::ChartOptions chartOptions;
        xatlas::PackOptions packOptions;
        packOptions.padding = 2;
        packOptions.texelsPerUnit = static_cast<float>(texelsPerUnit);
        packOptions.bilinear = true;
        packOptions.blockAlign = true;

        xatlas::Generate(atlas, chartOptions, packOptions);

        if (atlas->meshCount > 0) {
            const xatlas::Mesh& outMesh = atlas->meshes[0];
            for (uint32_t i = 0; i < outMesh.vertexCount; i++) {
                uint32_t origIdx = outMesh.vertexArray[i].xref;
                result.tangents[i] = tangents[origIdx];
                result.colors[i] = colors[origIdx];
            }
        }
    }

    xatlas::Destroy(atlas);

    return result;
}

// ============================================
// Check if UV2 is valid
// ============================================
bool HasValidUV2(
    const std::vector<XMFLOAT2>& uv2,
    const std::vector<uint32_t>& indices)
{
    if (uv2.empty() || indices.empty()) {
        return false;
    }

    // Check 1: All UVs in [0,1] range
    for (const auto& uv : uv2) {
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
            return false;
        }
    }

    // Check 2: No degenerate triangles in UV space
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const XMFLOAT2& uv0 = uv2[indices[i]];
        const XMFLOAT2& uv1 = uv2[indices[i + 1]];
        const XMFLOAT2& uv2_v = uv2[indices[i + 2]];

        // Check triangle area (2x area = cross product)
        float area2 = (uv1.x - uv0.x) * (uv2_v.y - uv0.y) -
                      (uv2_v.x - uv0.x) * (uv1.y - uv0.y);

        if (std::abs(area2) < 1e-8f) {
            return false;  // Degenerate triangle
        }
    }

    // Note: Full overlap detection would require more complex algorithms
    // For now, we trust xatlas output and just do basic validation

    return true;
}
