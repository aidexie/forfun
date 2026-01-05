// BloomUpsample.ps.hlsl
// Dual Kawase Blur - Upsample pass
// 9-tap tent filter with additive blending

cbuffer CB_BloomUpsample : register(b0) {
    float2 gTexelSize;   // 1.0 / inputSize (the texture being sampled)
    float gScatter;      // Blend factor with previous level (0-1)
    float _pad;
};

Texture2D gInput : register(t0);         // Lower resolution mip (being upsampled)
Texture2D gPreviousLevel : register(t1); // Previous level result (same res as output)
SamplerState gSampler : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// 9-tap tent filter upsample
// Weights:
//    [1]   [2]   [1]
//    [2]   [4]   [2]   / 16
//    [1]   [2]   [1]

float4 main(PSIn input) : SV_Target {
    float2 uv = input.uv;

    // 9-tap tent filter
    float3 s0 = gInput.Sample(gSampler, uv + float2(-1.0, -1.0) * gTexelSize).rgb; // TL
    float3 s1 = gInput.Sample(gSampler, uv + float2( 0.0, -1.0) * gTexelSize).rgb; // T
    float3 s2 = gInput.Sample(gSampler, uv + float2( 1.0, -1.0) * gTexelSize).rgb; // TR
    float3 s3 = gInput.Sample(gSampler, uv + float2(-1.0,  0.0) * gTexelSize).rgb; // L
    float3 s4 = gInput.Sample(gSampler, uv).rgb;                                    // Center
    float3 s5 = gInput.Sample(gSampler, uv + float2( 1.0,  0.0) * gTexelSize).rgb; // R
    float3 s6 = gInput.Sample(gSampler, uv + float2(-1.0,  1.0) * gTexelSize).rgb; // BL
    float3 s7 = gInput.Sample(gSampler, uv + float2( 0.0,  1.0) * gTexelSize).rgb; // B
    float3 s8 = gInput.Sample(gSampler, uv + float2( 1.0,  1.0) * gTexelSize).rgb; // BR

    // Apply tent weights: corners=1, edges=2, center=4
    float3 result = s0 + s2 + s6 + s8;           // Corners: weight 1
    result += (s1 + s3 + s5 + s7) * 2.0;         // Edges: weight 2
    result += s4 * 4.0;                           // Center: weight 4
    result *= (1.0 / 16.0);                       // Normalize

    // Blend with previous level using scatter factor
    // Higher scatter = more contribution from lower (blurrier) mips
    float3 previousLevel = gPreviousLevel.Sample(gSampler, uv).rgb;
    result = lerp(previousLevel, result, gScatter);

    return float4(result, 1.0);
}
