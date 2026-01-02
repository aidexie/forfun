// ============================================
// Lightmap2D.hlsl
// ============================================
// 2D Lightmap Sampling for baked diffuse GI
// UV2-based texture lookup
// ============================================

#ifndef LIGHTMAP_2D_HLSL
#define LIGHTMAP_2D_HLSL

// ============================================
// Resources
// ============================================
// t16: 2D Lightmap texture (R16G16B16A16_FLOAT)
Texture2D<float4> g_Lightmap2D : register(t16);

// t17: Lightmap scaleOffset buffer (StructuredBuffer<float4>)
StructuredBuffer<float4> g_LightmapScaleOffsets : register(t17);

// ============================================
// Sample 2D Lightmap
// UV2 should be in [0,1] range from mesh data
// scaleOffset transforms UV2 to atlas coordinates
// lightmapIndex: index into g_LightmapScaleOffsets buffer
// ============================================
float3 SampleLightmap2D(float2 uv2, int lightmapIndex, SamplerState samp)
{
    if (lightmapIndex < 0)
    {
        return float3(0, 0, 0);
    }

    // Fetch per-object scale/offset from structured buffer
    float4 scaleOffset = g_LightmapScaleOffsets[lightmapIndex];

    // Transform UV2 with per-object scale/offset
    // finalUV = uv2 * scale + offset
    float2 atlasUV = uv2 * scaleOffset.xy + scaleOffset.zw;

    // Sample lightmap (HDR, linear space)
    float3 irradiance = g_Lightmap2D.Sample(samp, atlasUV).rgb;

    return irradiance;
}

#endif // LIGHTMAP_2D_HLSL
