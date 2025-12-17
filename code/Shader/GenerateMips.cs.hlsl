// GenerateMips.cs.hlsl
// Compute shader for generating mipmaps (box filter downsampling)
// Supports: 2D texture arrays, Cubemaps (via array slices)
// sRGB textures: SRV auto-converts to linear on read, shader converts back to sRGB on write

// Constants passed per mip level generation
cbuffer CB_GenerateMips : register(b0)
{
    uint2 g_SrcMipSize;      // Source mip dimensions
    uint2 g_DstMipSize;      // Destination mip dimensions (SrcMipSize / 2)
    uint  g_SrcMipLevel;     // Source mip level index
    uint  g_ArraySlice;      // For cubemaps/arrays: which slice to process
    uint  g_IsSRGB;          // 1 if source is SRGB (need gamma correction on output)
    uint  g_Padding;
};

// Source texture - sRGB SRV auto-converts to linear on read
Texture2DArray<float4> g_SrcTexture : register(t0);

// Destination texture - UNORM format (write sRGB-encoded values for sRGB textures)
RWTexture2DArray<float4> g_DstTexture : register(u0);

// Linear -> sRGB conversion (gamma 2.2 approximation)
float3 LinearToSRGB(float3 linearColor)
{
    return pow(saturate(linearColor), 1.0f / 2.2f);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    // Check bounds
    if (DTid.x >= g_DstMipSize.x || DTid.y >= g_DstMipSize.y)
        return;

    // Calculate source texel coordinates (2x2 box filter)
    uint2 srcBase = DTid.xy * 2;

    // Load 4 source texels (already in linear space due to sRGB SRV auto-conversion)
    float4 c00 = g_SrcTexture.Load(int4(srcBase.x,     srcBase.y,     g_ArraySlice, g_SrcMipLevel));
    float4 c10 = g_SrcTexture.Load(int4(srcBase.x + 1, srcBase.y,     g_ArraySlice, g_SrcMipLevel));
    float4 c01 = g_SrcTexture.Load(int4(srcBase.x,     srcBase.y + 1, g_ArraySlice, g_SrcMipLevel));
    float4 c11 = g_SrcTexture.Load(int4(srcBase.x + 1, srcBase.y + 1, g_ArraySlice, g_SrcMipLevel));

    // Average in linear space
    float4 result = (c00 + c10 + c01 + c11) * 0.25f;

    // Convert back to sRGB if needed (UAV is UNORM, so we encode sRGB manually)
    if (g_IsSRGB)
    {
        result.rgb = LinearToSRGB(result.rgb);
    }

    g_DstTexture[uint3(DTid.xy, g_ArraySlice)] = result;
}
