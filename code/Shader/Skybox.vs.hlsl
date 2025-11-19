// Skybox Vertex Shader
// Renders skybox at far plane using cube mesh

cbuffer CB : register(b0) {
    float4x4 viewProj;
}

struct VSIn {
    float3 pos : POSITION;
};

struct VSOut {
    float4 posH : SV_Position;
    float3 localPos : TEXCOORD0;
};

VSOut main(VSIn input) {
    VSOut output;
    output.localPos = input.pos;
    // Set z=w to ensure skybox renders at far plane (depth = 1.0)
    output.posH = mul(float4(input.pos, 1.0), viewProj).xyww;
    return output;
}
