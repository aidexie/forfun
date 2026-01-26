// Shader/Common.hlsli
// Shared descriptor set declarations for all shaders
// Matches C++ slot constants in RHI/*Slots.h headers

#ifndef COMMON_HLSLI
#define COMMON_HLSLI

//==============================================
// Set 0: PerFrame (space0)
//==============================================

// Constant Buffers
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

// Global textures (t0-t3)
Texture2DArray gShadowMaps       : register(t0, space0);
Texture2D gBrdfLUT               : register(t1, space0);
TextureCubeArray gIrradiance     : register(t2, space0);
TextureCubeArray gPrefiltered    : register(t3, space0);

// Clustered lighting (t4-t6)
StructuredBuffer<uint2> gClusterData     : register(t4, space0);
StructuredBuffer<uint> gLightIndexList   : register(t5, space0);
StructuredBuffer<float4> gLightData      : register(t6, space0);

// Samplers
SamplerState gLinearClamp          : register(s0, space0);
SamplerState gLinearWrap           : register(s1, space0);
SamplerState gPointClamp           : register(s2, space0);
SamplerComparisonState gShadowSamp : register(s3, space0);
SamplerState gAniso                : register(s4, space0);

//==============================================
// Set 2: PerMaterial (space2)
//==============================================

cbuffer CB_Material : register(b0, space2) {
    float4 gAlbedoColor;
    float gRoughness;
    float gMetallic;
    float2 gMaterialPadding;
};

Texture2D gAlbedoTex             : register(t0, space2);
Texture2D gNormalTex             : register(t1, space2);
Texture2D gMetallicRoughnessTex  : register(t2, space2);
Texture2D gEmissiveTex           : register(t3, space2);
Texture2D gAOTex                 : register(t4, space2);
SamplerState gMaterialSampler    : register(s0, space2);

//==============================================
// Set 3: PerDraw (space3)
//==============================================

cbuffer CB_PerDraw : register(b0, space3) {
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};

#endif // COMMON_HLSLI
