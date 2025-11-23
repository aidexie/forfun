// Grid.vs.hlsl
// Vertex Shader for infinite grid rendering
// Renders a full-screen quad covering the far plane

struct VSInput {
    uint vertexID : SV_VertexID;
};

struct VSOutput {
    float4 positionCS : SV_POSITION;  // Clip space position
    float2 clipPos    : TEXCOORD0;    // NDC xy for world pos reconstruction
};

VSOutput main(VSInput input) {
    VSOutput output;

    // Generate full-screen quad from vertex ID
    // Triangle strip: (0,0), (1,0), (0,1), (1,1)
    float2 uv = float2((input.vertexID << 1) & 2, input.vertexID & 2);

    // Convert to NDC (-1 to 1)
    float2 ndc = uv * 2.0 - 1.0;

    // Position at far plane (z = 1 in DX clip space)
    output.positionCS = float4(ndc, 1.0, 1.0);
    output.clipPos = ndc;

    return output;
}
