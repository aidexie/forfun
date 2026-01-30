// DebugLine_DS.vs.hlsl
// Vertex Shader for debug line rendering (SM 5.1 - Descriptor Set path)

// PerPass constant buffer (space1)
cbuffer CBPerFrameVS : register(b0, space1) {
    matrix gViewProj;
};

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOutput {
    float4 positionCS : SV_POSITION;  // Clip space position
    float4 color      : COLOR;
};

VSOutput main(VSInput input) {
    VSOutput output;

    // Transform to clip space
    output.positionCS = mul(float4(input.position, 1.0), gViewProj);
    output.color = input.color;

    return output;
}
