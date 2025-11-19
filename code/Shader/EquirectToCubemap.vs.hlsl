// Equirectangular to Cubemap Conversion - Vertex Shader
// Used during initialization to convert HDR equirectangular map to cubemap

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
    output.posH = mul(float4(input.pos, 1.0), viewProj);
    return output;
}
