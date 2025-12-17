// GenerateMips.cs.hlsl
// Compute shader for generating mipmaps (box filter downsampling)
// Supports: 2D texture arrays, Cubemaps (via array slices)
// Uses UNORM SRV (no auto sRGB conversion) for explicit gamma handling

// Constants passed per mip level generation
cbuffer CB_GenerateMips : register(b0)
{
    uint2 g_SrcMipSize;      // Source mip dimensions
    uint2 g_DstMipSize;      // Destination mip dimensions (SrcMipSize / 2)
    uint  g_SrcMipLevel;     // Source mip level index
    uint  g_ArraySlice;      // For cubemaps/arrays: which slice to process
    uint  g_IsSRGB;          // 1 if source is SRGB (need gamma correction)
    uint  g_Padding;
};

// Source texture - UNORM format (no auto gamma conversion)
Texture2DArray<float4> g_SrcTexture : register(t0);

// Destination texture - UNORM format
RWTexture2DArray<float4> g_DstTexture : register(u0);

// sRGB <-> Linear conversion (gamma 2.2 approximation)
float3 SRGBToLinear(float3 srgb)
{
    return pow(saturate(srgb), 2.2f);
}

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

    // Load 4 source texels (raw UNORM values, stored in sRGB space for sRGB textures)
    float4 c00 = g_SrcTexture.Load(int4(srcBase.x,     srcBase.y,     g_ArraySlice, g_SrcMipLevel));
    float4 c10 = g_SrcTexture.Load(int4(srcBase.x + 1, srcBase.y,     g_ArraySlice, g_SrcMipLevel));
    float4 c01 = g_SrcTexture.Load(int4(srcBase.x,     srcBase.y + 1, g_ArraySlice, g_SrcMipLevel));
    float4 c11 = g_SrcTexture.Load(int4(srcBase.x + 1, srcBase.y + 1, g_ArraySlice, g_SrcMipLevel));

    float4 result;
    if (g_IsSRGB)
    {
        // sRGB textures: convert to linear, average, convert back to sRGB
        // Data flow: sRGB -> linear -> average -> sRGB (stored as UNORM)
        float3 lin00 = SRGBToLinear(c00.rgb);
        float3 lin10 = SRGBToLinear(c10.rgb);
        float3 lin01 = SRGBToLinear(c01.rgb);
        float3 lin11 = SRGBToLinear(c11.rgb);

        float3 avgLinear = (lin00 + lin10 + lin01 + lin11) * 0.25f;
        result.rgb = LinearToSRGB(avgLinear);
        result.a = (c00.a + c10.a + c01.a + c11.a) * 0.25f;  // Alpha is linear
    }
    else
    {
        // Linear formats: simple average
        result = (c00 + c10 + c01 + c11) * 0.25f;
    }

    g_DstTexture[uint3(DTid.xy, g_ArraySlice)] = result;
}
