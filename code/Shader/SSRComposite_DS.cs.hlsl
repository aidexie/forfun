// ============================================
// SSRComposite_DS.cs.hlsl - SSR Composite (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
// Composites Screen-Space Reflections with the HDR scene buffer
//
// The SSR result contains reflection color (RGB) and hit confidence (A).
// We blend SSR with existing IBL reflections based on confidence.
// ============================================

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_SSRComposite : register(b0, space1)
{
    float2 g_ScreenSize;         // Full resolution
    float2 g_TexelSize;          // 1.0 / screenSize
    float g_SSRIntensity;        // Overall SSR intensity multiplier
    float g_IBLFallbackWeight;   // How much to keep IBL when SSR misses (0-1)
    float g_RoughnessFade;       // Roughness cutoff for reflections
    float g_Pad;
    float3 g_CamPosWS;           // Camera world position
    float g_Pad2;
};

// Texture SRVs (t0-t7, space1)
Texture2D<float4> g_HDRInput : register(t0, space1);       // HDR buffer (read)
Texture2D<float4> g_SSRResult : register(t1, space1);      // SSR: rgb=reflection, a=confidence
Texture2D<float4> g_WorldPosMetallic : register(t2, space1);  // G-Buffer RT0
Texture2D<float4> g_NormalRoughness : register(t3, space1);   // G-Buffer RT1

// UAVs (u0-u3, space1)
RWTexture2D<float4> g_HDRBuffer : register(u0, space1);    // HDR buffer (write)

// Samplers (s0-s3, space1)
SamplerState g_LinearSampler : register(s0, space1);
SamplerState g_PointSampler : register(s1, space1);

// ============================================
// Helper Functions
// ============================================

// Fresnel-Schlick approximation
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// ============================================
// Main Compute Shader
// ============================================
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Bounds check
    if (DTid.x >= (uint)g_ScreenSize.x || DTid.y >= (uint)g_ScreenSize.y)
    {
        return;
    }

    float2 uv = (DTid.xy + 0.5) * g_TexelSize;

    // Sample inputs - read HDR via UAV to avoid SRV/UAV state conflict
    float4 hdrColor = g_HDRBuffer[DTid.xy];
    // Use linear sampler for SSR to support resolution scaling (bilinear upsample)
    float4 ssrData = g_SSRResult.SampleLevel(g_LinearSampler, uv, 0);
    float4 rt0 = g_WorldPosMetallic.SampleLevel(g_PointSampler, uv, 0);
    float4 rt1 = g_NormalRoughness.SampleLevel(g_PointSampler, uv, 0);

    // Unpack G-Buffer
    float3 posWS = rt0.xyz;
    float metallic = rt0.w;
    float3 N = normalize(rt1.xyz);
    float roughness = rt1.w;

    // SSR data
    float3 ssrColor = ssrData.rgb;
    float ssrConfidence = ssrData.a;

    // Early out for non-reflective surfaces
    if (ssrConfidence <= 0.001 || roughness >= g_RoughnessFade)
    {
        g_HDRBuffer[DTid.xy] = hdrColor;
        return;
    }

    // Calculate view direction and Fresnel
    float3 V = normalize(g_CamPosWS - posWS);
    float NdotV = saturate(dot(N, V));

    // F0 (reflectance at normal incidence)
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), float3(1.0, 1.0, 1.0), metallic);
    float3 fresnel = FresnelSchlick(NdotV, F0);

    // Roughness-based attenuation (rough surfaces reflect less via SSR)
    float roughnessWeight = 1.0 - saturate(roughness / g_RoughnessFade);
    roughnessWeight = roughnessWeight * roughnessWeight;  // Quadratic falloff

    // Final SSR contribution weight
    // The DeferredLighting pass already applied IBL specular.
    // We blend SSR over it: high confidence = use SSR, low = keep IBL
    float blendWeight = ssrConfidence * roughnessWeight * g_SSRIntensity;
    blendWeight = saturate(blendWeight);

    // Fresnel-weighted reflection
    float3 ssrContribution = ssrColor * fresnel * blendWeight;

    // For proper energy conservation, we should subtract the IBL specular
    // that was already applied and replace it with SSR. But this requires
    // knowing the IBL contribution, which we don't have separately.
    //
    // Pragmatic approach: Blend additively but attenuate by (1 - metallic*blendWeight)
    // to reduce over-brightening on metallic surfaces.
    //
    // A more correct future approach would be:
    // 1. Store IBL specular separately in the lighting pass
    // 2. Composite: output = hdr - iblSpecular*(1-blendWeight) + ssrContribution
    //
    // For now, use additive blend with intensity control:
    float3 finalColor = hdrColor.rgb + ssrContribution * (1.0 - metallic * 0.5 * blendWeight);

    g_HDRBuffer[DTid.xy] = float4(finalColor, hdrColor.a);
}
