#pragma once
#include <vector>
#include <DirectXMath.h>
#include <cstdint>

// ============================================
// Lightmap Data Structures
// ============================================

// Per-object lightmap info (stored in MeshRenderer)
struct SLightmapInfo {
    int lightmapIndex = -1;                              // Which atlas (-1 = none)
    DirectX::XMFLOAT4 scaleOffset = {1.0f, 1.0f, 0.0f, 0.0f};  // xy: scale, zw: offset
};

// UV2 generation result from xatlas
struct SUV2GenerationResult {
    bool success = false;

    // Output vertex data (xatlas may split vertices at UV seams)
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT3> normals;
    std::vector<DirectX::XMFLOAT4> tangents;
    std::vector<DirectX::XMFLOAT2> uv1;       // Original UV
    std::vector<DirectX::XMFLOAT2> uv2;       // Generated lightmap UV
    std::vector<DirectX::XMFLOAT4> colors;    // Vertex colors
    std::vector<uint32_t> indices;

    // Atlas dimensions suggested by xatlas
    int atlasWidth = 0;
    int atlasHeight = 0;
    int chartCount = 0;
};

// Atlas packing entry
struct SAtlasEntry {
    int meshRendererIndex = -1;   // Which MeshRenderer
    int atlasIndex = 0;           // Which atlas texture (if multiple)
    int atlasX = 0;               // Position in atlas (pixels)
    int atlasY = 0;
    int width = 0;                // Size in atlas (pixels)
    int height = 0;
};

// Atlas configuration
struct SLightmapAtlasConfig {
    int resolution = 1024;        // Atlas texture size (square)
    int padding = 2;              // Pixels between charts
    int texelsPerUnit = 16;       // Texel density (texels per world unit)
};

// Texel data after rasterization
struct STexelData {
    DirectX::XMFLOAT3 worldPos = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 normal = {0.0f, 1.0f, 0.0f};
    bool valid = false;
};

// Baking configuration for 2D lightmaps
struct SLightmap2DBakeConfig {
    int samplesPerTexel = 64;     // Monte Carlo samples per texel
    int maxBounces = 3;           // Max ray bounces for GI
    float skyIntensity = 1.0f;    // Sky light intensity multiplier
    bool useGPU = true;           // Use DXR GPU baking if available
    bool enableDenoiser = true;   // Enable Intel OIDN denoising
};
