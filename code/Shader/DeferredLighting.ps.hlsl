// Deferred Lighting Pixel Shader
// Full-screen pass that evaluates lighting from G-Buffer data

#include "Common.hlsl"              // PBR BRDF functions
#include "ClusteredShading.hlsl"    // Clustered point/spot lights
#include "VolumetricLightmap.hlsl"  // Volumetric Lightmap GI

// ============================================
// G-Buffer Textures (t0-t5)
// ============================================
Texture2D gRT0_WorldPosMetallic : register(t0);     // WorldPosition.xyz + Metallic
Texture2D gRT1_NormalRoughness : register(t1);      // Normal.xyz + Roughness
Texture2D gRT2_AlbedoAO : register(t2);             // Albedo.rgb + AO (sRGB)
Texture2D gRT3_EmissiveMaterialID : register(t3);   // Emissive.rgb + MaterialID
Texture2D gRT4_Velocity : register(t4);             // Velocity.xy (unused in lighting)
Texture2D gDepth : register(t5);                    // Scene depth (for sky masking)

// ============================================
// Shadow & IBL Textures (t6-t7, t16-t17)
// Note: t8-t10 used by ClusteredShading.hlsl
//       t11-t15 used by VolumetricLightmap.hlsl
// ============================================
Texture2DArray gShadowMaps : register(t6);          // CSM shadow maps
Texture2D gBrdfLUT : register(t7);                  // BRDF lookup table
TextureCubeArray gIrradianceArray : register(t16);  // IBL diffuse irradiance
TextureCubeArray gPrefilteredArray : register(t17); // IBL specular pre-filtered

// ============================================
// Samplers (s0-s1, s3)
// Note: s2 used by VolumetricLightmap.hlsl
// ============================================
SamplerState gLinearSampler : register(s0);
SamplerComparisonState gShadowSampler : register(s1);
SamplerState gPointSampler : register(s3);

// ============================================
// Constant Buffers
// ============================================
cbuffer CB_DeferredLighting : register(b0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gInvViewProj;          // For reconstructing world pos from depth

    // CSM parameters
    int gCascadeCount;
    int gEnableSoftShadows;
    float gCascadeBlendRange;
    float gShadowBias;
    float4 gCascadeSplits;
    float4x4 gLightSpaceVPs[4];

    // Directional light
    float3 gLightDirWS;
    float _pad0;
    float3 gLightColor;
    float _pad1;

    // Camera
    float3 gCamPosWS;
    float _pad2;

    // IBL
    float gIblIntensity;
    int gDiffuseGIMode;             // 0=VL, 1=GlobalIBL, 2=None, 3=Lightmap2D
    int gProbeIndex;                // Default probe index (0 = global)
    float _pad3;
}

// Diffuse GI modes (must match MainPass.ps.hlsl)
#define DIFFUSE_GI_VOLUMETRIC_LIGHTMAP 0
#define DIFFUSE_GI_GLOBAL_IBL 1
#define DIFFUSE_GI_NONE 2
#define DIFFUSE_GI_LIGHTMAP_2D 3

// ============================================
// Input/Output
// ============================================
struct PSIn {
    float4 posH : SV_Position;
    float2 uv : TEXCOORD0;
};

// ============================================
// Shadow Functions
// ============================================
int SelectCascade(float3 posWS) {
    float distance = length(posWS - gCamPosWS);
    for (int i = 0; i < gCascadeCount - 1; ++i) {
        if (distance < gCascadeSplits[i])
            return i;
    }
    return gCascadeCount - 1;
}

float CalcShadowFactor(float3 posWS) {
    int cascadeIndex = SelectCascade(posWS);

    // Transform to light space
    float4 posLS = mul(float4(posWS, 1.0), gLightSpaceVPs[cascadeIndex]);
    float3 projCoords = posLS.xyz / posLS.w;

    // Transform to [0,1] UV space
    float2 shadowUV = projCoords.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    // Outside shadow map = no shadow
    if (shadowUV.x < 0 || shadowUV.x > 1 ||
        shadowUV.y < 0 || shadowUV.y > 1 ||
        projCoords.z > 1.0)
        return 1.0;

    float currentDepth = projCoords.z;
    float cascadeIndexF = float(cascadeIndex);

    // PCF 3x3 or single sample
    if (gEnableSoftShadows == 1) {
        float shadow = 0.0;
        float2 texelSize = 1.0 / 2048.0;
        [unroll]
        for (int x = -1; x <= 1; x++) {
            [unroll]
            for (int y = -1; y <= 1; y++) {
                float2 uv = shadowUV + float2(x, y) * texelSize;
                shadow += gShadowMaps.SampleCmpLevelZero(gShadowSampler,
                    float3(uv.x, uv.y, cascadeIndexF),
                    currentDepth - gShadowBias);
            }
        }
        return shadow / 9.0;
    } else {
        return gShadowMaps.SampleCmpLevelZero(gShadowSampler,
            float3(shadowUV.x, shadowUV.y, cascadeIndexF),
            currentDepth - gShadowBias);
    }
}

// ============================================
// Main Pixel Shader
// ============================================
float4 main(PSIn i) : SV_Target {
    // ============================================
    // Sample G-Buffer
    // ============================================
    float4 rt0 = gRT0_WorldPosMetallic.Sample(gPointSampler, i.uv);
    float4 rt1 = gRT1_NormalRoughness.Sample(gPointSampler, i.uv);
    float4 rt2 = gRT2_AlbedoAO.Sample(gPointSampler, i.uv);
    float4 rt3 = gRT3_EmissiveMaterialID.Sample(gPointSampler, i.uv);
    float depth = gDepth.Sample(gPointSampler, i.uv).r;

    // Skip sky pixels (depth == 1.0)
    if (depth >= 1.0) {
        return float4(0, 0, 0, 1);  // Sky will be rendered separately
    }

    // Unpack G-Buffer data
    float3 posWS = rt0.xyz;
    float metallic = rt0.w;
    float3 N = normalize(rt1.xyz);
    float roughness = rt1.w;
    float3 albedo = rt2.rgb;
    float ao = rt2.a;
    float3 emissive = rt3.rgb;
    float materialID = rt3.a * 255.0;  // Decode material ID

    // ============================================
    // Calculate Vectors
    // ============================================
    float3 V = normalize(gCamPosWS - posWS);
    float3 L = normalize(-gLightDirWS);
    float3 H = normalize(L + V);
    float3 R = reflect(-V, N);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // F0 (reflectance at normal incidence)
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // ============================================
    // Direct Lighting (Directional Light)
    // ============================================
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // Shadow
    float shadowFactor = CalcShadowFactor(posWS);

    // Direct lighting result
    float3 Lo = (diffuse + specular) * gLightColor * NdotL * shadowFactor;

    // ============================================
    // Clustered Point/Spot Lights
    // ============================================
    float4 posView = mul(float4(posWS, 1.0), gView);
    float viewZ = posView.z;

    float3 clusteredLighting = ApplyClusteredPointLights(
        i.posH.xy,
        viewZ,
        posWS,
        N,
        V,
        albedo,
        metallic,
        roughness
    );

    Lo += clusteredLighting;

    // ============================================
    // Diffuse GI
    // ============================================
    float3 diffuseGI = float3(0, 0, 0);
    float probeIdxF = float(gProbeIndex);

    if (gDiffuseGIMode == DIFFUSE_GI_VOLUMETRIC_LIGHTMAP) {
        if (IsVolumetricLightmapEnabled()) {
            float3 vlIrradiance = GetVolumetricLightmapDiffuse(posWS, N);
            diffuseGI = vlIrradiance * albedo;
        }
    } else if (gDiffuseGIMode == DIFFUSE_GI_GLOBAL_IBL) {
        float3 irradiance = gIrradianceArray.Sample(gLinearSampler, float4(N, probeIdxF)).rgb;
        diffuseGI = irradiance * albedo;
    }
    // Note: DIFFUSE_GI_LIGHTMAP_2D is handled in G-Buffer pass (baked to emissive)

    // ============================================
    // Specular IBL
    // ============================================
    const float MAX_REFLECTION_LOD = 6.0;
    float3 prefilteredColor = gPrefilteredArray.SampleLevel(gLinearSampler,
        float4(R, probeIdxF), roughness * MAX_REFLECTION_LOD).rgb;

    float2 brdf = gBrdfLUT.Sample(gLinearSampler, float2(NdotV, roughness)).rg;
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    // IBL energy conservation
    float3 F_IBL = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = (1.0 - kS_IBL) * (1.0 - metallic);

    // ============================================
    // Combine Ambient
    // ============================================
    float3 ambient = (kD_IBL * diffuseGI + specularIBL) * ao;

    // ============================================
    // Final Color
    // ============================================
    float3 colorLin = emissive + (ambient * gIblIntensity) + Lo;

    return float4(colorLin, 1.0);
}
