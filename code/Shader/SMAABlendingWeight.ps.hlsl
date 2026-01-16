// SMAA Blending Weight Calculation Pass - Uses official SMAA implementation
// Reference: "SMAA: Enhanced Subpixel Morphological Antialiasing" (Jimenez et al., 2012)

// Configuration
#define SMAA_HLSL_4 1
#define SMAA_PRESET_HIGH 1

cbuffer CB_SMAABlend : register(b0) {
    float4 gRTMetrics;  // (1/width, 1/height, width, height)
};

#define SMAA_RT_METRICS gRTMetrics

// Include official SMAA implementation
#include "SMAA.hlsl"

Texture2D gEdgesTex : register(t0);
Texture2D gAreaTex : register(t1);
Texture2D gSearchTex : register(t2);
SamplerState gLinearSampler : register(s0);
SamplerState gPointSampler : register(s1);

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset[3] : TEXCOORD2;
};

struct VSInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

// Vertex shader with pre-calculated offsets
PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    SMAABlendingWeightCalculationVS(input.uv, output.pixcoord, output.offset);
    return output;
}

// Pixel shader using official SMAA blending weight calculation
float4 main(PSInput input) : SV_Target {
    // SMAA 1x: subsampleIndices = float4(0, 0, 0, 0)
    return SMAABlendingWeightCalculationPS(
        input.uv,
        input.pixcoord,
        input.offset,
        gEdgesTex,
        gAreaTex,
        gSearchTex,
        float4(0.0, 0.0, 0.0, 0.0)
    );
}
