// Pre-filter Environment Map Vertex Shader
// Fullscreen triangle using SV_VertexID trick

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexID : SV_VertexID) {
    VSOut output;

    // Generate fullscreen triangle
    // Vertex 0: (-1, -1) -> UV (0, 1)
    // Vertex 1: ( 3, -1) -> UV (2, 1)
    // Vertex 2: (-1,  3) -> UV (0,-1)
    float2 texcoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(texcoord * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = texcoord;

    return output;
}
