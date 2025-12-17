// ============================================
// VolumetricLightmap.hlsl
// ============================================
// Per-Pixel Volumetric Lightmap Sampling
// Two-level lookup: Indirection → Brick Atlas
// Hardware trilinear interpolation for smooth results
// ============================================

#ifndef VOLUMETRIC_LIGHTMAP_HLSL
#define VOLUMETRIC_LIGHTMAP_HLSL

// ============================================
// Constant Buffer (b6)
// ============================================
cbuffer CB_VolumetricLightmap : register(b6)
{
    float3 vl_volumeMin;
    float _vl_pad0;
    float3 vl_volumeMax;
    float _vl_pad1;
    float3 vl_volumeInvSize;        // 1.0 / (max - min)
    float _vl_pad2;

    float3 vl_indirectionInvSize;   // 1.0 / indirectionResolution
    float _vl_pad3;
    float3 vl_brickAtlasInvSize;    // 1.0 / brickAtlasSize
    float _vl_pad4;

    int vl_indirectionResolution;
    int vl_brickAtlasSize;
    int vl_maxLevel;
    int vl_enabled;

    int vl_brickCount;
    int3 _vl_pad5;
};

// ============================================
// Resources
// ============================================
// t11: Indirection Texture (R32_UINT)
// t12-14: Brick Atlas SH0/SH1/SH2 (R16G16B16A16_FLOAT)
// t15: Brick Info Buffer

Texture3D<uint> g_IndirectionTexture : register(t11);
Texture3D<float4> g_BrickAtlasSH0 : register(t12);
Texture3D<float4> g_BrickAtlasSH1 : register(t13);
Texture3D<float4> g_BrickAtlasSH2 : register(t14);

// Brick Info Structure (matches SBrickInfo in C++)
struct BrickInfo
{
    float3 worldMin;
    float _pad0;
    float3 worldMax;
    float _pad1;
    float3 atlasOffset;
    float _pad2;
};

StructuredBuffer<BrickInfo> g_BrickInfoBuffer : register(t15);

// Sampler for trilinear interpolation
SamplerState g_VLSampler : register(s2);

// ============================================
// Constants
// ============================================
static const int VL_BRICK_SIZE = 4;
static const uint VL_INVALID_BRICK = 0xFFFF;

// ============================================
// Indirection Entry Unpacking
// Format: [brickIndex: 16bit][level: 8bit][padding: 8bit]
// ============================================
struct IndirectionEntry
{
    uint brickIndex;
    uint level;
};

IndirectionEntry UnpackIndirectionEntry(uint packed)
{
    IndirectionEntry entry;
    entry.brickIndex = packed & 0xFFFF;
    entry.level = (packed >> 16) & 0xFF;
    return entry;
}

// ============================================
// SH L2 Coefficients (unpacked from atlas)
// ============================================
struct SHCoeffsL2
{
    float3 L0;      // sh[0]
    float3 L1_0;    // sh[1]
    float3 L1_1;    // sh[2]
    float3 L1_2;    // sh[3]
    // Note: Full L2 would need sh[4-8], but simplified version uses L0+L1
};

// ============================================
// Unpack SH from 3 atlas textures
// SH0: sh[0].RGB, sh[1].R
// SH1: sh[1].GB, sh[2].RG
// SH2: sh[2].B, sh[3].RGB
// ============================================
SHCoeffsL2 UnpackSHFromAtlas(float4 sh0, float4 sh1, float4 sh2)
{
    SHCoeffsL2 sh;
    sh.L0 = float3(sh0.x, sh0.y, sh0.z);
    sh.L1_0 = float3(sh0.w, sh1.x, sh1.y);
    sh.L1_1 = float3(sh1.z, sh1.w, sh2.x);
    sh.L1_2 = float3(sh2.y, sh2.z, sh2.w);
    return sh;
}

// ============================================
// Evaluate SH Irradiance (L1 only for diffuse)
// Normal should be normalized
// ============================================
float3 EvaluateSHIrradiance(SHCoeffsL2 sh, float3 normal)
{
    // SH basis functions (unnormalized)
    // Y_0^0 = 1
    // Y_1^-1 = y, Y_1^0 = z, Y_1^1 = x

    // Cosine lobe convolution coefficients (A_l)
    // A_0 = π
    // A_1 = 2π/3

    // Combined with SH normalization:
    // For diffuse irradiance from radiance stored in SH:
    // E(n) = A_0 * Y_0^0 * L_00 + A_1 * (Y_1^-1 * L_1-1 + Y_1^0 * L_10 + Y_1^1 * L_11)

    // Simplified coefficients after incorporating everything:
    // c0 = 0.886227 (sqrt(π/4) for L0)
    // c1 = 1.023327 (sqrt(π/3) for L1)

    float c0 = 0.886227f;   // L0 coefficient
    float c1 = 1.023327f;   // L1 coefficient

    float3 irradiance = float3(0, 0, 0);

    // L0 (constant term)
    irradiance += c0 * sh.L0;

    // L1 (directional terms)
    // Y_1^-1 = y, Y_1^0 = z, Y_1^1 = x
    irradiance += c1 * normal.y * sh.L1_0;  // L1^-1 (Y direction)
    irradiance += c1 * normal.z * sh.L1_1;  // L1^0  (Z direction)
    irradiance += c1 * normal.x * sh.L1_2;  // L1^1  (X direction)

    return max(irradiance, 0.0f);
}

// ============================================
// Sample Volumetric Lightmap at World Position
// Returns SH-evaluated irradiance for the given normal
// ============================================
float3 SampleVolumetricLightmap(float3 worldPos, float3 normal)
{
    // Check if VL is enabled
    if (vl_enabled == 0 || vl_brickCount == 0)
    {
        return float3(0, 0, 0);
    }

    // ============================================
    // Step 1: World Position → Indirection UV
    // ============================================
    float3 volumeUV = (worldPos - vl_volumeMin) * vl_volumeInvSize;

    // Check if position is inside volume
    if (any(volumeUV < 0.0f) || any(volumeUV > 1.0f))
    {
        return float3(0, 0, 0);
    }

    // ============================================
    // Step 2: Sample Indirection Texture
    // ============================================
    int3 indirCoord = int3(volumeUV * vl_indirectionResolution);
    indirCoord = clamp(indirCoord, int3(0, 0, 0), int3(vl_indirectionResolution - 1, vl_indirectionResolution - 1, vl_indirectionResolution - 1));

    uint packed = g_IndirectionTexture.Load(int4(indirCoord, 0));
    IndirectionEntry entry = UnpackIndirectionEntry(packed);

    // Check if valid brick
    if (entry.brickIndex == VL_INVALID_BRICK || entry.brickIndex >= (uint)vl_brickCount)
    {
        return float3(0, 0, 0);
    }

    // ============================================
    // Step 3: Get Brick Info
    // ============================================
    BrickInfo brick = g_BrickInfoBuffer[entry.brickIndex];

    // ============================================
    // Step 4: Calculate UV within Brick
    // ============================================
    float3 brickSize = brick.worldMax - brick.worldMin;
    float3 brickUV = (worldPos - brick.worldMin) / max(brickSize, 0.0001f);
    brickUV = saturate(brickUV);

    // ============================================
    // Step 5: Calculate Atlas UV
    // ============================================
    float3 voxelCoordInBrick = 0.5 + brickUV * (VL_BRICK_SIZE-1);
    float3 atlasCoord = brick.atlasOffset + voxelCoordInBrick;

    // Convert to normalized UV for texture sampling
    float3 atlasUV = atlasCoord * vl_brickAtlasInvSize;
    // ============================================
    // Step 6: Sample SH Atlas with Trilinear Interpolation
    // ============================================
    float4 sh0 = g_BrickAtlasSH0.SampleLevel(g_VLSampler, atlasUV, 0);
    float4 sh1 = g_BrickAtlasSH1.SampleLevel(g_VLSampler, atlasUV, 0);
    float4 sh2 = g_BrickAtlasSH2.SampleLevel(g_VLSampler, atlasUV, 0);

    // ============================================
    // Step 7: Unpack and Evaluate SH
    // ============================================
    SHCoeffsL2 shCoeffs = UnpackSHFromAtlas(sh0, sh1, sh2);
    float3 irradiance = EvaluateSHIrradiance(shCoeffs, normal);

    return irradiance;
}

// ============================================
// Get Volumetric Lightmap Diffuse Lighting
// Main entry point for shaders
// ============================================
float3 GetVolumetricLightmapDiffuse(float3 worldPos, float3 worldNormal)
{
    return SampleVolumetricLightmap(worldPos, normalize(worldNormal));
}

// ============================================
// Check if Volumetric Lightmap is enabled and has data
// ============================================
bool IsVolumetricLightmapEnabled()
{
    return vl_enabled != 0 && vl_brickCount > 0;
}

#endif // VOLUMETRIC_LIGHTMAP_HLSL
