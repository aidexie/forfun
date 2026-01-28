// HiZ_DS.cs.hlsl
// SM 5.1 Compute shaders for building Hierarchical-Z depth pyramid
// Uses unified compute layout (space1)
//
// Two entry points:
//   CSCopyDepth - Copy depth buffer to mip 0
//   CSBuildMip  - Build mip N from mip N-1 using MAX(2x2)

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_HiZ : register(b0, space1)
{
    uint g_SrcMipSizeX;     // Source mip width
    uint g_SrcMipSizeY;     // Source mip height
    uint g_DstMipSizeX;     // Destination mip width
    uint g_DstMipSizeY;     // Destination mip height
    uint g_SrcMipLevel;     // Source mip level index (for Load)
    uint3 g_Padding;        // Padding to 16-byte alignment
};

// Texture SRVs (t0-t7, space1)
Texture2D<float> g_DepthBuffer : register(t0, space1);

// UAVs (u0-u3, space1)
RWTexture2D<float> g_DstMip : register(u0, space1);
RWTexture2D<float> g_SrcMipUAV : register(u1, space1);

// Samplers (s0-s3, space1)
SamplerState g_PointSampler : register(s0, space1);

// ============================================
// CSCopyDepth - Copy depth buffer to Hi-Z mip 0
// ============================================
[numthreads(8, 8, 1)]
void CSCopyDepth(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= g_DstMipSizeX || DTid.y >= g_DstMipSizeY)
        return;

    float depth = g_DepthBuffer.Load(int3(DTid.xy, 0));
    g_DstMip[DTid.xy] = depth;
}

// ============================================
// CSBuildMip - Build mip N from mip N-1 using MAX
// ============================================
[numthreads(8, 8, 1)]
void CSBuildMip(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= g_DstMipSizeX || DTid.y >= g_DstMipSizeY)
        return;

    uint2 srcBase = DTid.xy * 2;
    uint2 srcMax = uint2(g_SrcMipSizeX - 1, g_SrcMipSizeY - 1);

    float d00 = g_SrcMipUAV[uint2(min(srcBase.x,     srcMax.x), min(srcBase.y,     srcMax.y))];
    float d10 = g_SrcMipUAV[uint2(min(srcBase.x + 1, srcMax.x), min(srcBase.y,     srcMax.y))];
    float d01 = g_SrcMipUAV[uint2(min(srcBase.x,     srcMax.x), min(srcBase.y + 1, srcMax.y))];
    float d11 = g_SrcMipUAV[uint2(min(srcBase.x + 1, srcMax.x), min(srcBase.y + 1, srcMax.y))];

    float maxDepth = max(max(d00, d10), max(d01, d11));
    g_DstMip[DTid.xy] = maxDepth;
}
