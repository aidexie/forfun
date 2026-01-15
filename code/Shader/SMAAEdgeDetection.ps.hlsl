// ============================================
// SMAAEdgeDetection.ps.hlsl - SMAA Edge Detection Pass
// ============================================
// First pass of SMAA: Detects edges using luma-based comparison.
// Outputs a 2-channel texture (RG8) marking horizontal and vertical edges.
//
// Reference:
//   "SMAA: Enhanced Subpixel Morphological Antialiasing"
//   Jorge Jimenez et al. (2012)
//   http://www.iryoku.com/smaa/
//
// Entry Point: main
// ============================================

// ============================================
// SMAA Configuration
// ============================================
#define SMAA_THRESHOLD 0.1
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0

// ============================================
// Constant Buffer
// ============================================
cbuffer CB_SMAAEdge : register(b0) {
    float4 gRTMetrics;  // (1/width, 1/height, width, height)
};

// ============================================
// Textures and Samplers
// ============================================
Texture2D<float4> gInputTexture : register(t0);
SamplerState gLinearSampler : register(s0);
SamplerState gPointSampler : register(s1);

// ============================================
// Helper Functions
// ============================================

// Convert RGB to perceptual luminance
float RGBToLuma(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

// ============================================
// Edge Detection
// ============================================
struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput {
    float2 edges : SV_Target0;  // RG8: (horizontal edge, vertical edge)
};

PSOutput main(PSInput input) {
    PSOutput output;
    output.edges = float2(0.0, 0.0);

    float2 uv = input.uv;
    float2 texelSize = gRTMetrics.xy;

    // Sample center and neighbors
    float L = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv, 0).rgb);
    float Lleft = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv + float2(-texelSize.x, 0), 0).rgb);
    float Ltop = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv + float2(0, -texelSize.y), 0).rgb);

    // Calculate deltas
    float2 delta;
    delta.x = abs(L - Lleft);
    delta.y = abs(L - Ltop);

    // Threshold edges
    float2 edges = step(SMAA_THRESHOLD, delta);

    // Early exit if no edges
    if (dot(edges, float2(1.0, 1.0)) == 0.0) {
        return output;
    }

    // Local contrast adaptation (reduces false positives in high-contrast areas)
    float Lright = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv + float2(texelSize.x, 0), 0).rgb);
    float Lbottom = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv + float2(0, texelSize.y), 0).rgb);

    float maxDelta = max(max(delta.x, delta.y),
                         max(abs(L - Lright), abs(L - Lbottom)));

    // Sample additional neighbors for better adaptation
    float Lleftleft = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv + float2(-2.0 * texelSize.x, 0), 0).rgb);
    float Ltoptop = RGBToLuma(gInputTexture.SampleLevel(gPointSampler, uv + float2(0, -2.0 * texelSize.y), 0).rgb);

    maxDelta = max(maxDelta, max(abs(Lleft - Lleftleft), abs(Ltop - Ltoptop)));

    // Apply local contrast adaptation
    edges *= step(maxDelta, delta * SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR);

    output.edges = edges;
    return output;
}
