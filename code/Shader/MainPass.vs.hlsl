// MainPass Vertex Shader
// PBR rendering with CSM shadow mapping

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
    float    gShadowBias; float3 _pad4;  // Removed Blinn-Phong parameters
}

cbuffer CB_Object : register(b1) {
    float4x4 gWorld;
    float3 gMatAlbedo; float gMatMetallic;
    float gMatRoughness; float3 _padObj;
}

struct VSIn {
    float3 p : POSITION;
    float3 n : NORMAL;
    float2 uv : TEXCOORD0;
    float4 t : TANGENT;
};

struct VSOut {
    float4 posH : SV_Position;
    float3 posWS : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3x3 TBN : TEXCOORD2;
    // Light space positions for all cascades (calculated in VS for performance)
    float4 posLS0 : TEXCOORD5;
    float4 posLS1 : TEXCOORD6;
    float4 posLS2 : TEXCOORD7;
    float4 posLS3 : TEXCOORD8;
};

VSOut main(VSIn i) {
    VSOut o;
    float4 posWS = mul(float4(i.p, 1), gWorld);
    float3 nWS = normalize(mul(float4(i.n, 0), gWorld).xyz);
    float3 tWS = normalize(mul(float4(i.t.xyz, 0), gWorld).xyz);
    float3 bWS = normalize(cross(nWS, tWS) * i.t.w);
    o.TBN = float3x3(tWS, bWS, nWS);
    o.posWS = posWS.xyz;
    o.uv = i.uv;
    float4 posV = mul(posWS, gView);
    o.posH = mul(posV, gProj);

    // Pre-calculate light space positions for all cascades in VS
    // This is much faster than calculating in PS (done once per vertex vs per fragment)
    o.posLS0 = mul(posWS, gLightSpaceVPs[0]);
    o.posLS1 = mul(posWS, gLightSpaceVPs[1]);
    o.posLS2 = mul(posWS, gLightSpaceVPs[2]);
    o.posLS3 = mul(posWS, gLightSpaceVPs[3]);

    return o;
}
