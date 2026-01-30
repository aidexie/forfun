// ============================================
// Fullscreen_DS.vs.hlsl - Fullscreen Vertex Shader (SM 5.1)
// ============================================
// Descriptor Set version for DX12 passes
// Supports vertex buffer path: Draw(4, 0) with TriangleStrip
// ============================================

struct VSInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}
