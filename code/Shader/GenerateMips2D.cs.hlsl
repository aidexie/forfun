// GenerateMips2D.cs.hlsl
// Compute shader for generating mipmaps for 2D textures (box filter downsampling)
// sRGB textures: SRV auto-converts to linear on read, shader converts back to sRGB on write

// Constants passed per mip level generation
cbuffer CB_GenerateMips : register(b0)
{
    uint2 g_SrcMipSize;      // Source mip dimensions
    uint2 g_DstMipSize;      // Destination mip dimensions (SrcMipSize / 2)
    uint  g_SrcMipLevel;     // Source mip level index (for Load)
    uint  g_ArraySlice;      // Unused for 2D (kept for struct compatibility)
    uint  g_IsSRGB;          // 1 if source is SRGB (need gamma correction on output)
    uint  g_Padding;
};

// Source texture - sRGB SRV auto-converts to linear on read
Texture2D<float4> g_SrcTexture : register(t0);

// Destination texture - UNORM format (write sRGB-encoded values for sRGB textures)
RWTexture2D<float4> g_DstTexture : register(u0);

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
    float4 c00 = g_SrcTexture.Load(int3(srcBase.x,     srcBase.y,     g_SrcMipLevel));
    float4 c10 = g_SrcTexture.Load(int3(srcBase.x + 1, srcBase.y,     g_SrcMipLevel));
    float4 c01 = g_SrcTexture.Load(int3(srcBase.x,     srcBase.y + 1, g_SrcMipLevel));
    float4 c11 = g_SrcTexture.Load(int3(srcBase.x + 1, srcBase.y + 1, g_SrcMipLevel));

    // Average in linear space
    float4 result = (c00 + c10 + c01 + c11) * 0.25f;

    // Convert back to sRGB if needed (UAV is UNORM, so we encode sRGB manually)
    if (g_IsSRGB)
    {
        result.rgb = LinearToSRGB(result.rgb);
    }

    g_DstTexture[DTid.xy] = result;
}
