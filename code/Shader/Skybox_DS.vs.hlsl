// Skybox Vertex Shader (SM 5.1 - Descriptor Set path)
// Renders skybox at far plane using cube mesh

cbuffer CB_Skybox : register(b0, space1) {
    float4x4 viewProj;
    uint useReversedZ;
    uint3 padding;
};

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

    // Render skybox at far plane
    // Standard Z: far plane = 1.0 (z=w)
    // Reversed Z: far plane = 0.0 (z=0)
    float4 clipPos = mul(float4(input.pos, 1.0), viewProj);
    if (useReversedZ) {
        output.posH = float4(clipPos.xy, 0.0, clipPos.w);
    } else {
        output.posH = clipPos.xyww;  // z=w means depth=1.0 after perspective divide
    }
    return output;
}
