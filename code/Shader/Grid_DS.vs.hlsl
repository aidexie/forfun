// Grid_DS.vs.hlsl
// Vertex Shader for infinite grid rendering (SM 5.1 - Descriptor Set path)
// Renders a full-screen quad covering the far plane

// PerPass constant buffer (space1)
cbuffer CBPerFrame : register(b0, space1) {
    matrix gViewProj;          // View * Projection matrix
    matrix gInvViewProj;       // Inverse of View * Projection
    float3 gCameraPos;         // Camera world position
    float  gGridFadeStart;     // Distance where grid starts fading (e.g., 50m)
    float  gGridFadeEnd;       // Distance where grid fully fades (e.g., 100m)
    float3 gPadding;
};

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
