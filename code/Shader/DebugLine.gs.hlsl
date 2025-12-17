// DebugLine.gs.hlsl
// Geometry Shader to expand lines into thick quads

// Use b1 to avoid slot conflict with VS (which uses b0 for viewProj)
// DX12 uses unified CB slots across all stages, unlike DX11 per-stage slots
cbuffer CBPerFrame : register(b1) {
    float2 gViewportSize;      // Viewport dimensions (width, height)
    float  gLineThickness;     // Line thickness in pixels
    float  gPadding;
};

struct GSInput {
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR;
};

struct GSOutput {
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR;
};

[maxvertexcount(4)]
void main(line GSInput input[2], inout TriangleStream<GSOutput> outputStream) {
    // Get the two line endpoints in clip space
    float4 p0 = input[0].positionCS;
    float4 p1 = input[1].positionCS;

    // Perspective divide to get NDC coordinates
    float2 ndc0 = p0.xy / p0.w;
    float2 ndc1 = p1.xy / p1.w;

    // Convert NDC to screen space (pixels)
    float2 screen0 = (ndc0 * 0.5 + 0.5) * gViewportSize;
    float2 screen1 = (ndc1 * 0.5 + 0.5) * gViewportSize;
    screen0.y = gViewportSize.y - screen0.y;  // Flip Y
    screen1.y = gViewportSize.y - screen1.y;

    // Calculate line direction and perpendicular offset in screen space
    float2 dir = normalize(screen1 - screen0);
    float2 normal = float2(-dir.y, dir.x);  // Perpendicular to line direction

    // Half-thickness offset in pixels
    float2 offset = normal * (gLineThickness * 0.5);

    // Create 4 vertices for the quad (in screen space)
    float2 screenQuad[4];
    screenQuad[0] = screen0 - offset;  // Bottom-left
    screenQuad[1] = screen0 + offset;  // Top-left
    screenQuad[2] = screen1 - offset;  // Bottom-right
    screenQuad[3] = screen1 + offset;  // Top-right

    // Convert back to NDC, then to clip space
    GSOutput output;
    for (int i = 0; i < 4; i++) {
        // Screen to NDC
        float2 ndc;
        ndc.x = (screenQuad[i].x / gViewportSize.x) * 2.0 - 1.0;
        ndc.y = 1.0 - (screenQuad[i].y / gViewportSize.y) * 2.0;  // Flip Y back

        // Interpolate depth and w between the two endpoints
        float t = (i < 2) ? 0.0 : 1.0;  // 0 for first two verts, 1 for last two
        float z = lerp(p0.z, p1.z, t);
        float w = lerp(p0.w, p1.w, t);

        // Reconstruct clip space position
        output.positionCS = float4(ndc.x * w, ndc.y * w, z, w);
        output.color = lerp(input[0].color, input[1].color, t);

        outputStream.Append(output);
    }

    outputStream.RestartStrip();
}
