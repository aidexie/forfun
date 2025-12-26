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

// ============================================
// Constant Buffer (b7)
// ============================================
cbuffer CB_Lightmap2D : register(b7)
{
    float4 lm_scaleOffset;  // xy: scale, zw: offset (per-object)
    int lm_enabled;
    float lm_intensity;
    float2 _lm_pad;
};

// ============================================
// Sample 2D Lightmap
// UV2 should be in [0,1] range from mesh data
// scaleOffset transforms UV2 to atlas coordinates
// ============================================
float3 SampleLightmap2D(float2 uv2, SamplerState samp)
{
    if (lm_enabled == 0)
    {
        return float3(0, 0, 0);
    }

    // Transform UV2 with per-object scale/offset
    // finalUV = uv2 * scale + offset
    float2 atlasUV = uv2 * lm_scaleOffset.xy + lm_scaleOffset.zw;

    // Sample lightmap (HDR, linear space)
    float3 irradiance = g_Lightmap2D.Sample(samp, atlasUV).rgb;

    return irradiance * lm_intensity;
}

// ============================================
// Check if 2D Lightmap is enabled
// ============================================
bool IsLightmap2DEnabled()
{
    return lm_enabled != 0;
}

#endif // LIGHTMAP_2D_HLSL
