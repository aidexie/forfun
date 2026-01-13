// HiZ.cs.hlsl
// Compute shaders for building Hierarchical-Z depth pyramid
// Uses MAX reduction for reversed-Z (near=1, far=0) to keep closest surface
//
// Two entry points:
//   CSCopyDepth - Copy depth buffer to mip 0
//   CSBuildMip  - Build mip N from mip N-1 using MAX(2x2)

// ============================================
// Constant Buffer
// ============================================
cbuffer CB_HiZ : register(b0)
{
    uint g_SrcMipSizeX;     // Source mip width
    uint g_SrcMipSizeY;     // Source mip height
    uint g_DstMipSizeX;     // Destination mip width
    uint g_DstMipSizeY;     // Destination mip height
    uint g_SrcMipLevel;     // Source mip level index (for Load)
    uint3 g_Padding;        // Padding to 16-byte alignment
};

// ============================================
// Resources
// ============================================
// Source depth buffer (for CSCopyDepth)
Texture2D<float> g_DepthBuffer : register(t0);

// Source mip texture (for CSBuildMip) - read via UAV to avoid SRV/UAV state conflict
RWTexture2D<float> g_SrcMipUAV : register(u1);

// Destination mip (UAV)
RWTexture2D<float> g_DstMip : register(u0);

// Point sampler (not used for Load, but available if needed)
SamplerState g_PointSampler : register(s0);

// ============================================
// CSCopyDepth - Copy depth buffer to Hi-Z mip 0
// ============================================
// Direct copy from depth buffer to mip 0 of Hi-Z pyramid
// No reduction, just format conversion (D32 -> R32)
[numthreads(8, 8, 1)]
void CSCopyDepth(uint3 DTid : SV_DispatchThreadID)
{
    // Bounds check
    if (DTid.x >= g_DstMipSizeX || DTid.y >= g_DstMipSizeY)
        return;

    // Load depth value directly
    float depth = g_DepthBuffer.Load(int3(DTid.xy, 0));

    // Write to mip 0
    g_DstMip[DTid.xy] = depth;
}

// ============================================
// CSBuildMip - Build mip N from mip N-1 using MAX
// ============================================
// For reversed-Z (near=1, far=0), MAX keeps the closest surface
// This provides conservative depth bounds for ray marching
// Uses UAV read to avoid SRV/UAV state conflict (texture stays in UAV state)
[numthreads(8, 8, 1)]
void CSBuildMip(uint3 DTid : SV_DispatchThreadID)
{
    // Bounds check
    if (DTid.x >= g_DstMipSizeX || DTid.y >= g_DstMipSizeY)
        return;

    // Calculate source texel coordinates (2x2 region)
    uint2 srcBase = DTid.xy * 2;

    // Load 4 texels from source mip via UAV
    // Clamp to source bounds to handle edge cases
    uint2 srcMax = uint2(g_SrcMipSizeX - 1, g_SrcMipSizeY - 1);

    float d00 = g_SrcMipUAV[uint2(min(srcBase.x,     srcMax.x), min(srcBase.y,     srcMax.y))];
    float d10 = g_SrcMipUAV[uint2(min(srcBase.x + 1, srcMax.x), min(srcBase.y,     srcMax.y))];
    float d01 = g_SrcMipUAV[uint2(min(srcBase.x,     srcMax.x), min(srcBase.y + 1, srcMax.y))];
    float d11 = g_SrcMipUAV[uint2(min(srcBase.x + 1, srcMax.x), min(srcBase.y + 1, srcMax.y))];

    // MAX for reversed-Z: keeps the closest (largest depth value) surface
    // This is conservative for ray marching - if ray is in front of MAX depth,
    // it's guaranteed to be in front of all surfaces in this tile
    float maxDepth = max(max(d00, d10), max(d01, d11));

    // Write to destination mip
    g_DstMip[DTid.xy] = maxDepth;
}
