// Fullscreen quad vertex shader for rendering to cubemap faces

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexID : SV_VertexID) {
    VSOut output;

    // Generate fullscreen triangle
    // 0: (-1, -1), 1: (3, -1), 2: (-1, 3)
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);

    return output;
}
