#pragma once
#include "LightmapTypes.h"
#include <vector>
#include <DirectXMath.h>

// ============================================
// Lightmap Rasterizer
// ============================================
// Rasterizes mesh triangles into lightmap UV space.
// For each texel, computes the world position and normal.

class CLightmapRasterizer {
public:
    CLightmapRasterizer() = default;
    ~CLightmapRasterizer() = default;

    // Initialize rasterizer with atlas dimensions
    void Initialize(int atlasWidth, int atlasHeight);

    // Rasterize a mesh into the atlas
    // positions, normals, uv2: vertex data (must be same size)
    // indices: triangle indices
    // worldMatrix: transform from local to world space
    // atlasOffsetX/Y: offset in atlas (from packing)
    // regionWidth/Height: size of this mesh's region in atlas
    void RasterizeMesh(
        const std::vector<DirectX::XMFLOAT3>& positions,
        const std::vector<DirectX::XMFLOAT3>& normals,
        const std::vector<DirectX::XMFLOAT2>& uv2,
        const std::vector<uint32_t>& indices,
        const DirectX::XMMATRIX& worldMatrix,
        int atlasOffsetX, int atlasOffsetY,
        int regionWidth, int regionHeight
    );

    // Clear all texel data
    void Clear();

    // Get texel data
    const std::vector<STexelData>& GetTexels() const { return m_texels; }
    std::vector<STexelData>& GetTexelsMutable() { return m_texels; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // Get count of valid texels
    int GetValidTexelCount() const;

private:
    // Rasterize a single triangle
    void rasterizeTriangle(
        const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2,
        const DirectX::XMFLOAT3& n0, const DirectX::XMFLOAT3& n1, const DirectX::XMFLOAT3& n2,
        const DirectX::XMFLOAT2& uv0, const DirectX::XMFLOAT2& uv1, const DirectX::XMFLOAT2& uv2,
        int offsetX, int offsetY,
        int regionWidth, int regionHeight
    );

    // Compute barycentric coordinates
    // Returns true if point is inside triangle
    static bool computeBarycentric(
        float px, float py,
        float x0, float y0,
        float x1, float y1,
        float x2, float y2,
        float& lambda0, float& lambda1, float& lambda2
    );

    std::vector<STexelData> m_texels;
    int m_width = 0;
    int m_height = 0;
};
