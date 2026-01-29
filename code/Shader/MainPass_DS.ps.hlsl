// MainPass_DS.ps.hlsl
// Forward Pass Pixel Shader (Descriptor Set Path, SM 5.1)
// Used by TransparentForwardPass for alpha-blended objects
// PBR rendering with CSM shadows, IBL, clustered lighting, volumetric lightmap

// NOTE: We don't include Common.hlsli because this shader uses a different
// binding model (PerPass in space1, PerMaterial in space2 with different layout)

// Include PBR helper functions (no resource bindings)
#include "Common.hlsl"

//==============================================
// Set 0: PerFrame (space0) - Global resources
//==============================================
cbuffer CB_PerFrame : register(b0, space0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float4x4 gInvView;
    float4x4 gInvProj;
    float4x4 gInvViewProj;
    float3 gCameraPos;
    float gTime;
    float2 gScreenSize;
    float gNearZ;
    float gFarZ;
};

// Global textures (space0)
Texture2DArray gShadowMaps       : register(t0, space0);
Texture2D gBrdfLUT               : register(t1, space0);
TextureCubeArray gIrradiance     : register(t2, space0);
TextureCubeArray gPrefiltered    : register(t3, space0);

// Samplers (space0)
SamplerState gLinearClamp          : register(s0, space0);
SamplerState gLinearWrap           : register(s1, space0);
SamplerState gPointClamp           : register(s2, space0);
SamplerComparisonState gShadowSamp : register(s3, space0);
SamplerState gAniso                : register(s4, space0);

// Include clustered lighting and volumetric lightmap AFTER samplers are defined
#include "ClusteredShading_DS.hlsli" // Clustered point/spot lights (DS version)
#include "VolumetricLightmap_DS.hlsli" // Volumetric lightmap (DS version)

//==============================================
// Set 1: PerPass (space1) - Frame data + CSM
//==============================================
cbuffer CB_MainPassFrame : register(b0, space1) {
    float4x4 gView_Pass;
    float4x4 gProj_Pass;

    // CSM parameters
    int gCascadeCount;
    int gDebugShowCascades;
    int gEnableSoftShadows;
    float gCascadeBlendRange;
    float4 gCascadeSplits;
    float4x4 gLightSpaceVPs[4];

    // Lighting
    float3 gLightDirWS;
    float _pad1;
    float3 gLightColor;
    float _pad2;
    float3 gCamPosWS_Pass;
    float _pad3;
    float gShadowBias;
    float gIblIntensity;
    int gDiffuseGIMode;
    float _pad4;
};

//==============================================
// Set 2: PerMaterial (space2) - Per-object + Material data
//==============================================
cbuffer CB_PerObject : register(b0, space2) {
    float4x4 gWorld;  // Not used in PS, but must match VS layout
    float3 gMatAlbedo;
    float gMatMetallic;
    float3 gMatEmissive;
    float gMatRoughness;
    float gMatEmissiveStrength;
    int gHasMetallicRoughnessTexture;
    int gHasEmissiveMap;
    int gAlphaMode;
    float gAlphaCutoff;
    int gProbeIndex;
    int gLightmapIndex;
    float _padMat;
};

Texture2D gAlbedoMap : register(t0, space2);
Texture2D gNormalMap : register(t1, space2);
Texture2D gMetallicRoughnessMap : register(t2, space2);
Texture2D gEmissiveMapTex : register(t3, space2);
SamplerState gMaterialSampler : register(s0, space2);

//==============================================
// Diffuse GI Mode constants
//==============================================
#define DIFFUSE_GI_VOLUMETRIC_LIGHTMAP 0
#define DIFFUSE_GI_GLOBAL_IBL 1
#define DIFFUSE_GI_NONE 2
#define DIFFUSE_GI_LIGHTMAP_2D 3

struct PSIn {
    float4 posH : SV_Position;
    float3 posWS : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3x3 TBN : TEXCOORD2;
    float4 posLS0 : TEXCOORD5;
    float4 posLS1 : TEXCOORD6;
    float4 posLS2 : TEXCOORD7;
    float4 color : COLOR0;
    float2 uv2 : TEXCOORD8;
};

//==============================================
// Shadow Functions
//==============================================
int SelectCascade(float3 posWS) {
    float distance = length(posWS - gCamPosWS_Pass);
    for (int i = 0; i < gCascadeCount - 1; ++i) {
        if (distance < gCascadeSplits[i])
            return i;
    }
    return gCascadeCount - 1;
}

float CalcShadowFactor(float3 posWS, float4 posLS0, float4 posLS1, float4 posLS2) {
    int cascadeIndex = SelectCascade(posWS);

    // Select pre-calculated light space position
    float4 posLS = (cascadeIndex == 0) ? posLS0 :
                   (cascadeIndex == 1) ? posLS1 :
                   (cascadeIndex == 2) ? posLS2 : mul(float4(posWS, 1.0), gLightSpaceVPs[3]);

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
    // Debug: Visualize cascade levels
    if (gDebugShowCascades == 1) {
        int cascadeIndex = SelectCascade(i.posWS);
        float3 cascadeColors[4] = {
            float3(1.0, 0.0, 0.0),  // Cascade 0: Red
            float3(0.0, 1.0, 0.0),  // Cascade 1: Green
            float3(0.0, 0.0, 1.0),  // Cascade 2: Blue
            float3(1.0, 1.0, 0.0)   // Cascade 3: Yellow
        };
        return float4(cascadeColors[cascadeIndex], 1.0);
    }

    // Sample textures
    float4 albedoTexFull = gAlbedoMap.Sample(gMaterialSampler, i.uv);
    float3 albedoTex = albedoTexFull.rgb;
    float alpha = albedoTexFull.a;

    // Alpha Test (Mask mode)
    if (gAlphaMode == 1 && alpha < gAlphaCutoff) {
        discard;
    }

    // Normal mapping
    float3 nTS = gNormalMap.Sample(gMaterialSampler, i.uv).xyz * 2.0 - 1.0;
    nTS.y = -nTS.y;  // Flip Y (glTF uses OpenGL format)
    nTS = normalize(nTS);
    float3 N = normalize(mul(nTS, i.TBN));

    // Metallic/Roughness
    float2 metallicRoughnessTex = gMetallicRoughnessMap.Sample(gMaterialSampler, i.uv).gb;
    float roughnessTex = metallicRoughnessTex.x;
    float metallicTex = metallicRoughnessTex.y;

    float3 albedo = gMatAlbedo * albedoTex;
    float metallic = gHasMetallicRoughnessTexture ? metallicTex : gMatMetallic;
    float roughness = gHasMetallicRoughnessTexture ? roughnessTex : gMatRoughness;
    float ao = i.color.r;

    // Calculate vectors
    float3 L = normalize(-gLightDirWS);
    float3 V = normalize(gCamPosWS_Pass - i.posWS);
    float3 H = normalize(L + V);
    float3 R = reflect(-V, N);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // F0 (reflectance at normal incidence)
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // Cook-Torrance BRDF
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // Shadow factor
    float shadowFactor = CalcShadowFactor(i.posWS, i.posLS0, i.posLS1, i.posLS2);

    // Direct lighting
    bool useLightmap2D = (gDiffuseGIMode == DIFFUSE_GI_LIGHTMAP_2D);
    float3 Lo;
    if (useLightmap2D) {
        Lo = specular * gLightColor * NdotL * shadowFactor;
    } else {
        Lo = (diffuse + specular) * gLightColor * NdotL * shadowFactor;
    }

    // Clustered Point/Spot Lights
    float4 posView = mul(float4(i.posWS, 1.0), gView_Pass);
    float viewZ = posView.z;

    float3 pointLightContribution = ApplyClusteredPointLights(
        i.posH.xy,
        viewZ,
        i.posWS,
        N,
        V,
        albedo,
        metallic,
        roughness
    );
    Lo += pointLightContribution;

    // IBL - Specular
    float probeIdxF = float(gProbeIndex);
    const float MAX_REFLECTION_LOD = 6.0;
    float3 prefilteredColor = gPrefiltered.SampleLevel(gLinearClamp,
        float4(R, probeIdxF), roughness * MAX_REFLECTION_LOD).rgb;

    // Diffuse GI
    float3 diffuseGI = float3(0, 0, 0);
    if (gDiffuseGIMode == DIFFUSE_GI_VOLUMETRIC_LIGHTMAP) {
        if (IsVolumetricLightmapEnabled()) {
            float3 vlIrradiance = GetVolumetricLightmapDiffuse(i.posWS, N);
            diffuseGI = vlIrradiance * albedo;
        }
    } else if (gDiffuseGIMode == DIFFUSE_GI_GLOBAL_IBL) {
        float3 irradiance = gIrradiance.Sample(gLinearClamp, float4(N, probeIdxF)).rgb;
        diffuseGI = irradiance * albedo;
    }

    // Specular IBL
    float2 brdf = gBrdfLUT.Sample(gLinearClamp, float2(NdotV, roughness)).rg;
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    // IBL energy conservation
    float3 F_IBL = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = (1.0 - kS_IBL) * (1.0 - metallic);

    // Combine Ambient
    float3 ambient;
    if (useLightmap2D) {
        ambient = diffuseGI + specularIBL * ao;
    } else {
        ambient = (kD_IBL * diffuseGI + specularIBL) * ao;
    }

    // Emissive
    float3 emissive = gMatEmissive * gMatEmissiveStrength;
    if (gHasEmissiveMap) {
        float3 emissiveTex = gEmissiveMapTex.Sample(gMaterialSampler, i.uv).rgb;
        emissive = emissiveTex * gMatEmissiveStrength;
    }

    // Final Color
    float3 colorLin;
    if (useLightmap2D) {
        colorLin = emissive + ambient + Lo;
    } else {
        colorLin = emissive + (ambient * gIblIntensity) + Lo;
    }

    // Alpha Output
    float outputAlpha = (gAlphaMode == 2) ? alpha : 1.0;

    return float4(colorLin, outputAlpha);
}
