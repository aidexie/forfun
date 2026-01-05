// BloomThreshold.ps.hlsl
// Extracts bright pixels from HDR buffer with soft threshold
// Output is half resolution

cbuffer CB_BloomThreshold : register(b0) {
    float2 gTexelSize;   // 1.0 / inputSize
    float gThreshold;    // Luminance threshold
    float gSoftKnee;     // Soft transition (0.0-1.0)
};

Texture2D gHDRInput : register(t0);
SamplerState gSampler : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Rec.709 luminance
float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Soft threshold with knee for smooth falloff
// Returns multiplier [0, 1] based on luminance vs threshold
float3 SoftThreshold(float3 color, float threshold, float knee) {
    float luma = Luminance(color);

    // Calculate soft knee curve
    float soft = luma - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);

    // Choose between hard and soft threshold
    float contribution = max(soft, luma - threshold);
    contribution /= max(luma, 1e-5);

    return color * saturate(contribution);
}

// 4-tap bilinear downsample with threshold
// Samples 4 pixels at once during the threshold pass
float4 main(PSIn input) : SV_Target {
    // Sample 4 pixels in a 2x2 block (bilinear filter does this automatically)
    // We're writing to half-res, so each output pixel represents 2x2 input pixels
    float2 uv = input.uv;

    // Sample with small offset to get proper bilinear filtering
    float3 color = gHDRInput.Sample(gSampler, uv).rgb;

    // Apply soft threshold
    float3 bloom = SoftThreshold(color, gThreshold, gSoftKnee * gThreshold);

    // Clamp to prevent fireflies (extremely bright pixels causing artifacts)
    bloom = min(bloom, 10.0);

    return float4(bloom, 1.0);
}
