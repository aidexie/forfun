// Shadow_DS.vs.hlsl
// SM 5.1 depth-only vertex shader for shadow map rendering
// Uses descriptor set model with register spaces

//==============================================
// Set 1: PerPass (space1) - Shadow cascade data
//==============================================
cbuffer CB_ShadowPass : register(b0, space1) {
    float4x4 gLightSpaceVP;
    int gCascadeIndex;
    float3 _padPass;
};

//==============================================
// Set 3: PerDraw (space3) - Per-object transform
//==============================================
cbuffer CB_PerDraw : register(b0, space3) {
    float4x4 gWorld;
    float4x4 gWorldPrev;      // Not used in shadow pass
    int gLightmapIndex;       // Not used in shadow pass
    int gObjectID;            // Not used in shadow pass
    float2 _padDraw;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
};

float4 main(VSInput input) : SV_Position {
    float4 posWS = mul(float4(input.position, 1.0), gWorld);
    return mul(posWS, gLightSpaceVP);
}
