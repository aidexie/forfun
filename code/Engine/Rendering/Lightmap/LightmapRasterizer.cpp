#include "LightmapRasterizer.h"
#include "Core/FFLog.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

void CLightmapRasterizer::Initialize(int atlasWidth, int atlasHeight)
{
    m_width = atlasWidth;
    m_height = atlasHeight;
    m_texels.clear();
    m_texels.resize(static_cast<size_t>(m_width) * m_height);

    // Initialize all texels as invalid
    for (auto& texel : m_texels) {
        texel.valid = false;
        texel.worldPos = {0, 0, 0};
        texel.normal = {0, 1, 0};
    }
}

void CLightmapRasterizer::Clear()
{
    for (auto& texel : m_texels) {
        texel.valid = false;
    }
}

int CLightmapRasterizer::GetValidTexelCount() const
{
    int count = 0;
    for (const auto& texel : m_texels) {
        if (texel.valid) count++;
    }
    return count;
}

void CLightmapRasterizer::RasterizeMesh(
    const std::vector<XMFLOAT3>& positions,
    const std::vector<XMFLOAT3>& normals,
    const std::vector<XMFLOAT2>& uv2,
    const std::vector<uint32_t>& indices,
    const XMMATRIX& worldMatrix,
    int atlasOffsetX, int atlasOffsetY,
    int regionWidth, int regionHeight)
{
    if (positions.empty() || indices.empty()) {
        return;
    }

    if (positions.size() != normals.size() || positions.size() != uv2.size()) {
        CFFLog::Error("[LightmapRasterizer] Vertex data size mismatch");
        return;
    }

    // Transform positions and normals to world space
    std::vector<XMFLOAT3> worldPositions(positions.size());
    std::vector<XMFLOAT3> worldNormals(normals.size());

    // Normal matrix = transpose(inverse(worldMatrix))
    // For uniform scale, this is just the rotation part
    XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, worldMatrix));

    for (size_t i = 0; i < positions.size(); i++) {
        // Transform position
        XMVECTOR pos = XMLoadFloat3(&positions[i]);
        pos = XMVector3TransformCoord(pos, worldMatrix);
        XMStoreFloat3(&worldPositions[i], pos);

        // Transform normal
        XMVECTOR norm = XMLoadFloat3(&normals[i]);
        norm = XMVector3TransformNormal(norm, normalMatrix);
        norm = XMVector3Normalize(norm);
        XMStoreFloat3(&worldNormals[i], norm);
    }

    // Rasterize each triangle
    size_t triangleCount = indices.size() / 3;
    for (size_t t = 0; t < triangleCount; t++) {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        rasterizeTriangle(
            worldPositions[i0], worldPositions[i1], worldPositions[i2],
            worldNormals[i0], worldNormals[i1], worldNormals[i2],
            uv2[i0], uv2[i1], uv2[i2],
            atlasOffsetX, atlasOffsetY,
            regionWidth, regionHeight
        );
    }
}

void CLightmapRasterizer::rasterizeTriangle(
    const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2,
    const XMFLOAT3& n0, const XMFLOAT3& n1, const XMFLOAT3& n2,
    const XMFLOAT2& uv0, const XMFLOAT2& uv1, const XMFLOAT2& uv2,
    int offsetX, int offsetY,
    int regionWidth, int regionHeight)
{
    // Convert UV [0,1] to pixel coordinates within the region
    float x0 = uv0.x * regionWidth + offsetX;
    float y0 = uv0.y * regionHeight + offsetY;
    float x1 = uv1.x * regionWidth + offsetX;
    float y1 = uv1.y * regionHeight + offsetY;
    float x2 = uv2.x * regionWidth + offsetX;
    float y2 = uv2.y * regionHeight + offsetY;

    // Compute bounding box
    int minX = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
    int maxX = std::min(m_width - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
    int minY = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
    int maxY = std::min(m_height - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));

    // Rasterize
    for (int py = minY; py <= maxY; py++) {
        for (int px = minX; px <= maxX; px++) {
            // Texel center
            float cx = px + 0.5f;
            float cy = py + 0.5f;

            // Compute barycentric coordinates
            float lambda0, lambda1, lambda2;
            if (!computeBarycentric(cx, cy, x0, y0, x1, y1, x2, y2, lambda0, lambda1, lambda2)) {
                continue;  // Outside triangle
            }

            // Texel index
            int idx = py * m_width + px;

            // Interpolate world position
            m_texels[idx].worldPos.x = lambda0 * p0.x + lambda1 * p1.x + lambda2 * p2.x;
            m_texels[idx].worldPos.y = lambda0 * p0.y + lambda1 * p1.y + lambda2 * p2.y;
            m_texels[idx].worldPos.z = lambda0 * p0.z + lambda1 * p1.z + lambda2 * p2.z;

            // Interpolate and normalize normal
            float nx = lambda0 * n0.x + lambda1 * n1.x + lambda2 * n2.x;
            float ny = lambda0 * n0.y + lambda1 * n1.y + lambda2 * n2.y;
            float nz = lambda0 * n0.z + lambda1 * n1.z + lambda2 * n2.z;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) {
                m_texels[idx].normal = {nx / len, ny / len, nz / len};
            } else {
                m_texels[idx].normal = {0, 1, 0};
            }

            m_texels[idx].valid = true;
        }
    }
}

bool CLightmapRasterizer::computeBarycentric(
    float px, float py,
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float& lambda0, float& lambda1, float& lambda2)
{
    // Compute barycentric coordinates using edge function
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);

    // Check for degenerate triangle
    if (std::abs(denom) < 1e-8f) {
        return false;
    }

    float invDenom = 1.0f / denom;

    lambda0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) * invDenom;
    lambda1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) * invDenom;
    lambda2 = 1.0f - lambda0 - lambda1;

    // Check if inside triangle (with small epsilon for edge cases)
    const float eps = -1e-4f;
    return (lambda0 >= eps && lambda1 >= eps && lambda2 >= eps);
}
