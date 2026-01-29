// MainPass_DS.vs.hlsl
// Forward Pass Vertex Shader (Descriptor Set Path, SM 5.1)
// Used by TransparentForwardPass for alpha-blended objects

//==============================================
// Set 0: PerFrame (space0) - Global resources
//==============================================
// (No VS resources needed from Set 0)

//==============================================
// Set 1: PerPass (space1) - Frame data + CSM
//==============================================
cbuffer CB_MainPassFrame : register(b0, space1) {
    float4x4 gView;
    float4x4 gProj;

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
    float3 gCamPosWS;
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
    float4x4 gWorld;
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
    float _padObj;
};

struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
    float4 color : COLOR;
    float2 uv2 : TEXCOORD1;
};

struct VSOut {
    float4 posH : SV_Position;
    float3 posWS : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3x3 TBN : TEXCOORD2;
    // Light space positions for first 3 cascades
    float4 posLS0 : TEXCOORD5;
    float4 posLS1 : TEXCOORD6;
    float4 posLS2 : TEXCOORD7;
    float4 color : COLOR0;
    float2 uv2 : TEXCOORD8;
};

VSOut main(VSIn i) {
    VSOut o;

    // World space position
    float4 posWS = mul(float4(i.pos, 1.0), gWorld);
    o.posWS = posWS.xyz;

    // Build TBN matrix for normal mapping
    float3 nWS = normalize(mul(float4(i.normal, 0), gWorld).xyz);
    float3 tWS = normalize(mul(float4(i.tangent.xyz, 0), gWorld).xyz);
    float3 bWS = normalize(cross(nWS, tWS) * i.tangent.w);
    o.TBN = float3x3(tWS, bWS, nWS);

    // Pass through UVs and vertex color
    o.uv = i.uv;
    o.uv2 = i.uv2;
    o.color = i.color;

    // Clip-space position
    float4 posV = mul(posWS, gView);
    o.posH = mul(posV, gProj);

    // Pre-calculate light space positions for first 3 cascades
    o.posLS0 = mul(posWS, gLightSpaceVPs[0]);
    o.posLS1 = mul(posWS, gLightSpaceVPs[1]);
    o.posLS2 = mul(posWS, gLightSpaceVPs[2]);

    return o;
}
