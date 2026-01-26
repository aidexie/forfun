// DeferredLighting_DS.ps.hlsl
// Deferred Lighting Pixel Shader with Descriptor Sets (SM 5.1)
// Uses register spaces for clean separation of binding frequencies

#include "Common.hlsli"              // PerFrame bindings (space0)
#include "ClusteredShading_DS.hlsli" // Clustered point/spot lights (DS version)
#include "VolumetricLightmap_DS.hlsli" // Volumetric lightmap (DS version)

//==============================================
// Set 1: PerPass (space1) - G-Buffer and pass-specific resources
//==============================================

// G-Buffer textures (t0-t5, space1)
Texture2D gGBuffer_Albedo   : register(t0, space1);  // Albedo.rgb + AO
Texture2D gGBuffer_Normal   : register(t1, space1);  // Normal.xyz + Roughness
Texture2D gGBuffer_WorldPos : register(t2, space1);  // WorldPosition.xyz + Metallic
Texture2D gGBuffer_Emissive : register(t3, space1);  // Emissive.rgb + MaterialID
Texture2D gGBuffer_Velocity : register(t4, space1);  // Velocity.xy
Texture2D gGBuffer_Depth    : register(t5, space1);  // Scene depth

// Post-process inputs (t7, space1)
Texture2D gSSAO : register(t7, space1);

// PerPass constant buffer (b0, space1)
cbuffer CB_DeferredLightingPerPass : register(b0, space1) {
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

    // IBL settings
    float gIblIntensity;
    int gDiffuseGIMode;
    int gProbeIndex;
    uint gUseReversedZ;
}

// PerPass samplers (s0-s1, space1)
SamplerState gPointSampler  : register(s0, space1);
SamplerState gLinearSampler_PerPass : register(s1, space1);

//==============================================
// Diffuse GI modes
//==============================================
#define DIFFUSE_GI_VOLUMETRIC_LIGHTMAP 0
#define DIFFUSE_GI_GLOBAL_IBL 1
#define DIFFUSE_GI_NONE 2
#define DIFFUSE_GI_LIGHTMAP_2D 3

// Material types
#define MATERIAL_STANDARD   0
#define MATERIAL_SUBSURFACE 1
#define MATERIAL_CLOTH      2
#define MATERIAL_HAIR       3
#define MATERIAL_CLEAR_COAT 4
#define MATERIAL_UNLIT      5

//==============================================
// Input/Output
//==============================================
struct PSIn {
    float4 posH : SV_Position;
    float2 uv : TEXCOORD0;
};

//==============================================
// Shadow Functions
//==============================================
int SelectCascade(float3 posWS) {
    float distance = length(posWS - gCameraPos);
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
                shadow += gShadowMaps.SampleCmpLevelZero(gShadowSamp,
                    float3(uv.x, uv.y, cascadeIndexF),
                    currentDepth - gShadowBias);
            }
        }
        return shadow / 9.0;
    } else {
        return gShadowMaps.SampleCmpLevelZero(gShadowSamp,
            float3(shadowUV.x, shadowUV.y, cascadeIndexF),
            currentDepth - gShadowBias);
    }
}

//==============================================
// Main Pixel Shader
//==============================================
float4 main(PSIn i) : SV_Target {
    // Sample G-Buffer (new order: Albedo, Normal, WorldPos, Emissive, Velocity, Depth)
    float4 albedoAO = gGBuffer_Albedo.Sample(gPointSampler, i.uv);
    float4 normalRoughness = gGBuffer_Normal.Sample(gPointSampler, i.uv);
    float4 worldPosMetallic = gGBuffer_WorldPos.Sample(gPointSampler, i.uv);
    float4 emissiveMaterialID = gGBuffer_Emissive.Sample(gPointSampler, i.uv);
    float depth = gGBuffer_Depth.Sample(gPointSampler, i.uv).r;

    // Skip sky pixels
    bool isSky = gUseReversedZ ? (depth <= 0.001) : (depth >= 0.999);
    if (isSky) {
        return float4(0, 0, 0, 1);
    }

    // Unpack G-Buffer data
    float3 albedo = albedoAO.rgb;
    float materialAO = albedoAO.a;
    float3 N = normalize(normalRoughness.xyz);
    float roughness = normalRoughness.w;
    float3 posWS = worldPosMetallic.xyz;
    float metallic = worldPosMetallic.w;
    float3 emissive = emissiveMaterialID.rgb;
    int materialID = (int)(emissiveMaterialID.a * 255.0 + 0.5);

    // Sample SSAO and combine with material AO
    float ssao = gSSAO.Sample(gLinearClamp, i.uv).r;
    float ao = materialAO * ssao;

    // MATERIAL_UNLIT: Skip all lighting
    if (materialID == MATERIAL_UNLIT) {
        return float4(emissive + albedo, 1.0);
    }

    // Calculate vectors
    float3 V = normalize(gCameraPos - posWS);
    float3 L = normalize(-gLightDirWS);
    float3 H = normalize(L + V);
    float3 R = reflect(-V, N);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // F0 (reflectance at normal incidence)
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // Direct Lighting (Directional Light)
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

    // Clustered Point/Spot Lights
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

    // Diffuse GI
    float3 diffuseGI = float3(0, 0, 0);
    float probeIdxF = float(gProbeIndex);

    if (gDiffuseGIMode == DIFFUSE_GI_VOLUMETRIC_LIGHTMAP) {
        if (IsVolumetricLightmapEnabled()) {
            float3 vlIrradiance = GetVolumetricLightmapDiffuse(posWS, N);
            diffuseGI = vlIrradiance * albedo;
        }
    } else if (gDiffuseGIMode == DIFFUSE_GI_GLOBAL_IBL) {
        float3 irradiance = gIrradiance.Sample(gLinearClamp, float4(N, probeIdxF)).rgb;
        diffuseGI = irradiance * albedo;
    }

    // Specular IBL
    const float MAX_REFLECTION_LOD = 6.0;
    float3 prefilteredColor = gPrefiltered.SampleLevel(gLinearClamp,
        float4(R, probeIdxF), roughness * MAX_REFLECTION_LOD).rgb;

    float2 brdf = gBrdfLUT.Sample(gLinearClamp, float2(NdotV, roughness)).rg;
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    // IBL energy conservation
    float3 F_IBL = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = (1.0 - kS_IBL) * (1.0 - metallic);

    // Combine Ambient
    float3 ambient = (kD_IBL * diffuseGI + specularIBL) * ao;

    // Final Color
    float3 colorLin = emissive + (ambient * gIblIntensity) + Lo;

    return float4(colorLin, 1.0);
}
