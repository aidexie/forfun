// TestDescriptorSet Vertex Shader
// SM 5.1 with register spaces for descriptor set testing

struct VSInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Set 0 (space0): PerFrame constants
cbuffer CB_PerFrame : register(b0, space0) {
    float4x4 gViewProj;
    float gTime;
    float3 _pad0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), gViewProj);
    output.uv = input.uv;
    return output;
}
