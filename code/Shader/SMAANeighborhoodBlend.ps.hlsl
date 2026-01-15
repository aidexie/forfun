// ============================================
// SMAANeighborhoodBlend.ps.hlsl - SMAA Neighborhood Blending Pass
// ============================================
// Third and final pass of SMAA: Blends pixels with neighbors using
// the calculated blending weights from pass 2.
//
// Reference:
//   "SMAA: Enhanced Subpixel Morphological Antialiasing"
//   Jorge Jimenez et al. (2012)
//   http://www.iryoku.com/smaa/
//
// Entry Point: main
// ============================================

// ============================================
// Constant Buffer
// ============================================
cbuffer CB_SMAANeighbor : register(b0) {
    float4 gRTMetrics;  // (1/width, 1/height, width, height)
};

// ============================================
// Textures and Samplers
// ============================================
Texture2D<float4> gInputTexture : register(t0);   // Original LDR input
Texture2D<float4> gBlendTex : register(t1);       // Blending weights from pass 2
SamplerState gLinearSampler : register(s0);
SamplerState gPointSampler : register(s1);

// ============================================
// Neighborhood Blending
// ============================================
struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float2 uv = input.uv;

    // Fetch blending weights for current pixel
    float4 a;
    a.x = gBlendTex.SampleLevel(gLinearSampler, uv + float2(-gRTMetrics.x, 0), 0).a;  // Left
    a.y = gBlendTex.SampleLevel(gLinearSampler, uv + float2(0, -gRTMetrics.y), 0).g;  // Top
    a.wz = gBlendTex.SampleLevel(gLinearSampler, uv, 0).xz;  // Right, Bottom

    // Check if blending is needed
    [branch]
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
        // No blending needed, return original color
        return gInputTexture.SampleLevel(gPointSampler, uv, 0);
    }

    // Calculate blending offsets
    bool h = max(a.x, a.z) > max(a.y, a.w);  // Horizontal vs vertical

    float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
    float2 blendingWeight = float2(a.y, a.w);

    if (h) {
        blendingOffset = float4(a.x, 0.0, a.z, 0.0);
        blendingWeight = float2(a.x, a.z);
    }

    blendingWeight /= dot(blendingWeight, float2(1.0, 1.0));

    // Calculate texture coordinates for neighbor sampling
    float4 blendingCoord = blendingOffset * float4(gRTMetrics.xy, -gRTMetrics.xy) + uv.xyxy;

    // Sample neighbors and blend
    float4 color = blendingWeight.x * gInputTexture.SampleLevel(gLinearSampler, blendingCoord.xy, 0);
    color += blendingWeight.y * gInputTexture.SampleLevel(gLinearSampler, blendingCoord.zw, 0);

    return color;
}
