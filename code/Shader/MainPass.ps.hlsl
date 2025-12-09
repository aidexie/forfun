// MainPass Pixel Shader
// PBR rendering with CSM shadow mapping

#include "ClusteredShading.hlsl"      // Clustered point light support
#include "LightProbe.hlsl"            // Light Probe SH support
#include "VolumetricLightmap.hlsl"    // Volumetric Lightmap (Per-Pixel GI)

Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2DArray gShadowMaps : register(t2);  // CSM: Texture2DArray
TextureCubeArray gIrradianceArray : register(t3);   // IBL: Irradiance (32x32, 8 probes)
TextureCubeArray gPrefilteredArray : register(t4);  // IBL: Prefiltered (128x128, 8 probes)
Texture2D gBrdfLUT : register(t5);  // IBL: BRDF lookup table
Texture2D gMetallicRoughness : register(t6);  // G=Roughness, B=Metallic (glTF 2.0 standard)
Texture2D gEmissiveMap : register(t7);  // Emissive texture (sRGB)
SamplerState gSamp : register(s0);
SamplerComparisonState gShadowSampler : register(s1);

cbuffer CB_Frame : register(b0) {
    float4x4 gView;
    float4x4 gProj;

    // CSM parameters
    int gCascadeCount;
    int gDebugShowCascades;  // 0=off, 1=on
    int gEnableSoftShadows;  // 0=hard, 1=soft (PCF)
    float gCascadeBlendRange;  // Blend range at cascade boundaries (0-1)
    float4 gCascadeSplits;  // Changed from array to float4 for alignment
    float4x4 gLightSpaceVPs[4];

    // Lighting
    float3   gLightDirWS; float _pad1;
    float3   gLightColor; float _pad2;
    float3   gCamPosWS;   float _pad3;
    float    gShadowBias;
    float    gIblIntensity;  // IBL ambient multiplier
    int      gDiffuseGIMode; // EDiffuseGIMode: 0=VL, 1=GlobalIBL, 2=None
    float    _pad4;
}

// Diffuse GI Mode constants
static const int DIFFUSE_GI_VOLUMETRIC_LIGHTMAP = 0;
static const int DIFFUSE_GI_GLOBAL_IBL = 1;
static const int DIFFUSE_GI_NONE = 2;

cbuffer CB_Object : register(b1) {
    float4x4 gWorld;
    float3 gMatAlbedo; float gMatMetallic;
    float3 gMatEmissive; float gMatRoughness;
    float gMatEmissiveStrength;
    int gHasMetallicRoughnessTexture;
    int gHasEmissiveMap;
    int gAlphaMode;  // 0=Opaque, 1=Mask, 2=Blend
    float gAlphaCutoff;
    int gProbeIndex;  // Per-object probe selection (0 = global, 1-7 = local)
    float2 _padObj;
}

// ============================================
// Reflection Probe Data
// ============================================
#define MAX_PROBES 8

struct ProbeInfo {
    float3 position;
    float radius;
};

cbuffer CB_Probes : register(b4) {
    ProbeInfo gProbes[MAX_PROBES];
    int gProbeCount;
    float3 _padProbes;
}

struct PSIn {
    float4 posH : SV_Position;
    float3 posWS : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3x3 TBN : TEXCOORD2;
    // Light space positions for first 3 cascades (pre-calculated in VS)
    float4 posLS0 : TEXCOORD5;
    float4 posLS1 : TEXCOORD6;
    float4 posLS2 : TEXCOORD7;
    float4 color : COLOR0;  // Vertex color (moved from TEXCOORD9 to COLOR0)
};

// ============================================
// PBR Constants and Functions
// ============================================
// Note: BRDF functions (DistributionGGX, GeometrySmith, FresnelSchlick, etc.)
// are now in Common.hlsl (included via ClusteredShading.hlsl)

// Note: Probe selection moved to CPU (per-object) via gProbeIndex in CB_Object
// CB_Probes is kept for potential future use (blending, debug visualization)

// ============================================
// Shadow Functions
// ============================================
// Select cascade based on spherical distance from camera (Unity approach)
int SelectCascade(float3 posWS, float3 camPosWS) {
    float distance = length(posWS - camPosWS);  // Spherical distance
    for (int i = 0; i < gCascadeCount - 1; ++i) {
        if (distance < gCascadeSplits[i])
            return i;
    }
    return gCascadeCount - 1;
}

float CalcShadowFactor(float3 posWS, float4 posLS0, float4 posLS1, float4 posLS2) {
    // Select cascade using spherical distance
    int cascadeIndex = SelectCascade(posWS, gCamPosWS);

    // Select pre-calculated light space position for the chosen cascade
    // For cascade 3, calculate dynamically to save interpolators
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
    float cascadeIndexF = float(cascadeIndex);  // Explicit conversion to float

    // Soft shadows: PCF 3x3 (9 samples), Hard shadows: 1 sample
    if (gEnableSoftShadows == 1) {
        // PCF 3x3 for soft shadow edges
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
        // Single sample for hard shadows (faster)
        return gShadowMaps.SampleCmpLevelZero(gShadowSampler,
                                              float3(shadowUV.x, shadowUV.y, cascadeIndexF),
                                              currentDepth - gShadowBias);
    }
}

// ============================================
// Main Pixel Shader
// ============================================
float4 main(PSIn i) : SV_Target {
    // Debug: Visualize cascade levels
    if (gDebugShowCascades == 1) {
        int cascadeIndex = SelectCascade(i.posWS, gCamPosWS);
        float3 cascadeColors[4] = {
            float3(1.0, 0.0, 0.0),  // Cascade 0: Red
            float3(0.0, 1.0, 0.0),  // Cascade 1: Green
            float3(0.0, 0.0, 1.0),  // Cascade 2: Blue
            float3(1.0, 1.0, 0.0)   // Cascade 3: Yellow
        };
        return float4(cascadeColors[cascadeIndex], 1.0);
    }

    // Sample textures
    // Note: gAlbedo uses DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, GPU auto-converts sRGB→Linear
    float4 albedoTexFull = gAlbedo.Sample(gSamp, i.uv);
    float3 albedoTex = albedoTexFull.rgb;
    float alpha = albedoTexFull.a;
    
    // ============================================
    // Alpha Test (Mask mode)
    // ============================================
    // Alpha Mode: 0=Opaque, 1=Mask, 2=Blend
    // For Mask mode: discard pixels with alpha below cutoff threshold
    // CRITICAL: This creates binary transparency (hard edges) without alpha blending
    if (gAlphaMode == 1 && alpha < gAlphaCutoff) {
        discard;  // Early exit: pixel is fully transparent
    }
    float3 nTS = gNormal.Sample(gSamp, i.uv).xyz * 2.0 - 1.0;
    nTS.y = -nTS.y;  // CRITICAL: Flip Y channel (glTF uses OpenGL format, Y+up; DirectX uses Y+down)
    nTS = normalize(nTS);
    float3 N = normalize(mul(nTS, i.TBN));

    // Sample metallic/roughness texture (glTF 2.0 standard: G=Roughness, B=Metallic)
    float2 metallicRoughnessTex = gMetallicRoughness.Sample(gSamp, i.uv).gb;
    float roughnessTex = metallicRoughnessTex.x;  // Green channel
    float metallicTex = metallicRoughnessTex.y;   // Blue channel

    // DIAGNOSTIC: Visualize vertex color to check if it's causing darkening
    // return float4(i.color.rgb, 1.0);  // Uncomment to see vertex color

    // Material properties: Choose texture or CB values based on flag (Unity/UE approach)
    // If real texture exists: use texture values directly
    // If default texture (no texture): use CB_Object values

    // Use vertex color as baked AO (common in glTF models)
    // Vertex color R channel typically contains AO data
    float3 albedo = gMatAlbedo * albedoTex;

    float metallic = gHasMetallicRoughnessTexture ? metallicTex : gMatMetallic;
    float roughness = gHasMetallicRoughnessTexture ? roughnessTex : gMatRoughness;
    float ao = i.color.r;  // Use vertex color R channel as AO (common in glTF baked lighting)

    // Calculate vectors
    float3 L = normalize(-gLightDirWS);  // Light direction
    float3 V = normalize(gCamPosWS - i.posWS);  // View direction
    float3 H = normalize(L + V);  // Half vector

    // Calculate dot products
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Calculate F0 (surface reflection at zero incidence)
    // Dielectrics: 0.04, Metals: use albedo as F0
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);

    // Cook-Torrance BRDF
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    // Specular component
    float3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;  // Prevent divide by zero
    float3 specular = numerator / denominator;

    // Energy conservation: kS (specular) + kD (diffuse) = 1.0
    float3 kS = F;  // Fresnel already gives us specular contribution
    float3 kD = float3(1.0, 1.0, 1.0) - kS;
    kD *= 1.0 - metallic;  // Metals have no diffuse reflection

    // Lambert diffuse
    float3 diffuse = kD * albedo / PI;

    // Shadow factor
    float shadowFactor = CalcShadowFactor(i.posWS, i.posLS0, i.posLS1, i.posLS2);

    // Direct lighting (affected by shadow)
    float3 Lo = (diffuse + specular) * gLightColor * NdotL * shadowFactor;

    // ============================================
    // Clustered Point Lights
    // ============================================
    // Calculate view-space Z for cluster lookup
    float4 posView = mul(float4(i.posWS, 1.0), gView);
    float viewZ = posView.z;

    // Apply clustered point lights
    float3 pointLightContribution = ApplyClusteredPointLights(
        i.posH.xy,    // Screen position (pixels)
        viewZ,        // View-space depth
        i.posWS,      // World position
        N,            // Normal
        V,            // View direction
        albedo,       // Albedo
        metallic,     // Metallic
        roughness     // Roughness
    );

    // Add point lights to direct lighting
    Lo += pointLightContribution;


    // ============================================
    // IBL (Image-Based Lighting) - TextureCubeArray
    // ============================================
    // Calculate reflection vector for specular IBL
    float3 R = reflect(-V, N);

    // Per-object probe selection (from CB_Object, set by CPU)
    float probeIdxF = float(gProbeIndex);  // 0 = global, 1-7 = local probes

    // Sample from TextureCubeArray
    const float MAX_REFLECTION_LOD = 6.0;  // 7 mip levels (0-6)
    float3 prefilteredColor = gPrefilteredArray.SampleLevel(gSamp, float4(R, probeIdxF), roughness * MAX_REFLECTION_LOD).rgb;


    // ============================================
    // Diffuse IBL: Explicit Mode Selection
    // Mode 0: Volumetric Lightmap (Per-Pixel GI)
    // Mode 1: Global IBL (Skybox Irradiance)
    // Mode 2: None (Disabled, for baking)
    // ============================================
    float3 diffuseIBL = float3(0, 0, 0);

    if (gDiffuseGIMode == DIFFUSE_GI_VOLUMETRIC_LIGHTMAP) {
        // Volumetric Lightmap mode
        if (IsVolumetricLightmapEnabled()) {
            float3 vlIrradiance = GetVolumetricLightmapDiffuse(i.posWS, N);

            diffuseIBL = vlIrradiance * albedo;
        }
    } else if (gDiffuseGIMode == DIFFUSE_GI_GLOBAL_IBL) {
        // Global IBL mode (Skybox Irradiance)
        float3 irradiance = gIrradianceArray.Sample(gSamp, float4(N, probeIdxF)).rgb;
        diffuseIBL = irradiance * albedo;
    }
    // else: DIFFUSE_GI_NONE - diffuseIBL stays at 0 (no diffuse GI)
    // Sample BRDF LUT (X: NdotV, Y: roughness)
    float2 brdf = gBrdfLUT.Sample(gSamp, float2(NdotV, roughness)).rg;

    // Combine pre-filtered color with BRDF (Split Sum Approximation)
    // brdf.x = scale for F0, brdf.y = bias
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    // Calculate IBL contribution with energy conservation
    // For IBL, we use FresnelSchlickRoughness instead of the direct lighting Fresnel
    float3 F_IBL = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = float3(1.0, 1.0, 1.0) - kS_IBL;
    kD_IBL *= 1.0 - metallic;

    // Combine diffuse and specular IBL
    // CRITICAL: Divide diffuseIBL by PI for energy conservation with direct lighting
    // Direct lighting uses (albedo/PI), so IBL diffuse should match
    float3 ambient = (kD_IBL * diffuseIBL + specularIBL) * ao;

    // Final color (linear space)
    // Physically correct: only direct lighting (Lo) is affected by shadow
    // IBL (ambient) represents omnidirectional environment light, not affected by directional shadow
    // ============================================
    // Emissive
    // ============================================
    // Calculate emissive (self-emitted light, not affected by shadows/AO/IBL)
    float3 emissive = gMatEmissive * gMatEmissiveStrength;
    if (gHasEmissiveMap) {
        // Sample emissive texture (sRGB, GPU auto-converts to linear)
        float3 emissiveTex = gEmissiveMap.Sample(gSamp, i.uv).rgb;
        emissive = emissiveTex * gMatEmissiveStrength;
    }

    // Final color: Emissive + Reflected Light
    // CRITICAL: Emissive is added AFTER all lighting calculations
    // It is NOT affected by shadows, IBL, or AO (self-emitted light)
    float3 colorLin = emissive + (ambient * gIblIntensity) + Lo;

    // Alpha Output
    // Opaque (0): Always output alpha=1.0 (fully opaque)
    // Mask (1):   Already discarded transparent pixels, remaining are opaque (alpha=1.0)
    // Blend (2):  Output texture alpha for smooth blending
    float outputAlpha = (gAlphaMode == 2) ? alpha : 1.0;

    return float4(colorLin, outputAlpha);
}



