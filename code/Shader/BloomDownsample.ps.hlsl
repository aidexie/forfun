// BloomDownsample.ps.hlsl
// Dual Kawase Blur - Downsample pass
// 5-tap diagonal sampling pattern

cbuffer CB_BloomDownsample : register(b0) {
    float2 gTexelSize;   // 1.0 / inputSize
    float2 _pad;
};

Texture2D gInput : register(t0);
SamplerState gSampler : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Kawase downsample: 5-tap pattern
// Center (weight 4) + 4 diagonal corners (weight 1 each), divide by 8
//
//    [-1,-1]     [+1,-1]
//         \     /
//          [0,0]  (center x 4)
//         /     \
//    [-1,+1]     [+1,+1]

float4 main(PSIn input) : SV_Target {
    float2 uv = input.uv;
    float2 halfTexel = gTexelSize * 0.5;

    // Sample center (we're at half res, so bilinear gives us 4x average already)
    float3 center = gInput.Sample(gSampler, uv).rgb;

    // Sample 4 diagonal corners (offset by 1 texel in each direction)
    float3 tl = gInput.Sample(gSampler, uv + float2(-1.0, -1.0) * gTexelSize).rgb;
    float3 tr = gInput.Sample(gSampler, uv + float2( 1.0, -1.0) * gTexelSize).rgb;
    float3 bl = gInput.Sample(gSampler, uv + float2(-1.0,  1.0) * gTexelSize).rgb;
    float3 br = gInput.Sample(gSampler, uv + float2( 1.0,  1.0) * gTexelSize).rgb;

    // Weighted average: center*4 + corners*1 = 8 total weight
    float3 result = center * 4.0 + tl + tr + bl + br;
    result *= (1.0 / 8.0);

    return float4(result, 1.0);
}
