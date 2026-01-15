// ============================================
// Fullscreen.vs.hlsl - Fullscreen Triangle Vertex Shader
// ============================================
// Generates a fullscreen triangle from vertex ID (no vertex buffer needed).
// Can also be used with a fullscreen quad vertex buffer.
//
// Usage:
//   - Without VB: Draw(3, 0) with TriangleList topology
//   - With VB:    Draw(4, 0) with TriangleStrip topology
// ============================================

struct VSInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Entry point for vertex buffer path (fullscreen quad)
VSOutput main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

// Entry point for no-VB path (fullscreen triangle)
VSOutput mainNoVB(uint vertexID : SV_VertexID) {
    VSOutput output;
    // Generate fullscreen triangle covering [-1,1] NDC
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
