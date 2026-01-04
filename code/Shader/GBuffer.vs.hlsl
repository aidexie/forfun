// G-Buffer Vertex Shader
// Outputs data for deferred rendering G-Buffer pass

cbuffer CB_Frame : register(b0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProjPrev;  // Previous frame's ViewProj (for velocity)
    float3   gCamPosWS;
    float    _pad0;
}

cbuffer CB_Object : register(b1) {
    float4x4 gWorld;
    float4x4 gWorldPrev;     // Previous frame's world matrix (for velocity)
    float3 gMatAlbedo; float gMatMetallic;
    float3 gMatEmissive; float gMatRoughness;
    float gMatEmissiveStrength;
    int gHasMetallicRoughnessTexture;
    int gHasEmissiveMap;
    int gAlphaMode;  // 0=Opaque, 1=Mask, 2=Blend
    float gAlphaCutoff;
    int gLightmapIndex;
    float gMaterialID;
    float _padObj;
}

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
    float4 color : COLOR0;
    float2 uv2 : TEXCOORD5;
    float4 posCurr : TEXCOORD6;   // Current clip-space position
    float4 posPrev : TEXCOORD7;   // Previous frame clip-space position
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

    // Current frame clip-space position
    float4 posV = mul(posWS, gView);
    o.posH = mul(posV, gProj);
    o.posCurr = o.posH;

    // Previous frame clip-space position (for velocity calculation)
    float4 posWSPrev = mul(float4(i.pos, 1.0), gWorldPrev);
    o.posPrev = mul(posWSPrev, gViewProjPrev);

    return o;
}
