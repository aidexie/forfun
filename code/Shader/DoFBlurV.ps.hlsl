// ============================================
// Depth of Field - Pass 4: Vertical Gaussian Blur
// ============================================
// 11-tap separable Gaussian blur with gather-as-scatter
// Uses max neighbor CoC to allow blur to spread into in-focus areas
// ============================================

cbuffer CB_Blur : register(b0) {
    float2 gTexelSize;
    float gMaxCoCRadius;
    int gSampleCount;
};

Texture2D gColorInput : register(t0);
Texture2D gCoCInput : register(t1);
SamplerState gLinearSampler : register(s0);

static const float2 kBlurDir = float2(0.0, 1.0);  // Vertical

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Gaussian weights for 11-tap blur
static const float kGaussianWeights[11] = {
    0.0093, 0.028, 0.0659, 0.1216, 0.1756,
    0.1974,  // Center
    0.1756, 0.1216, 0.0659, 0.028, 0.0093
};

float4 main(PSIn input) : SV_Target {
    float centerCoC = gCoCInput.SampleLevel(gLinearSampler, input.uv, 0).r;

    // Find max CoC in neighborhood - this allows blur to spread into lower-CoC areas
    float maxNeighborCoC = centerCoC;
    for (int p = 0; p < 11; ++p) {
        float preOffset = (float)(p - 5);
        float2 preUV = input.uv + kBlurDir * gTexelSize * preOffset * gMaxCoCRadius;
        preUV = saturate(preUV);
        float neighborCoC = gCoCInput.SampleLevel(gLinearSampler, preUV, 0).r;
        maxNeighborCoC = max(maxNeighborCoC, neighborCoC);
    }

    // Use max neighbor CoC for blur radius (gather-as-scatter)
    float blurRadius = maxNeighborCoC * gMaxCoCRadius;

    // Early out if no blur needed
    if (blurRadius < 0.5) {
        return gColorInput.SampleLevel(gLinearSampler, input.uv, 0);
    }

    float4 colorSum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;

    // 11-tap Gaussian blur
    for (int i = 0; i < 11; ++i) {
        float offset = (float)(i - 5);  // -5 to +5
        float2 sampleUV = input.uv + kBlurDir * gTexelSize * offset * blurRadius;
        sampleUV = saturate(sampleUV);

        float4 sampleColor = gColorInput.SampleLevel(gLinearSampler, sampleUV, 0);
        float sampleCoC = gCoCInput.SampleLevel(gLinearSampler, sampleUV, 0).r;

        float weight = kGaussianWeights[i];

        // Weight by sample's CoC - higher CoC samples contribute more to blur
        // This simulates scatter: blurry pixels "spread" their color to neighbors
        float cocContribution = saturate(sampleCoC * 2.0 + 0.1);
        weight *= cocContribution;

        // Soft bilateral weight to prevent extreme CoC differences from mixing
        float cocDiff = abs(sampleCoC - centerCoC);
        weight *= exp(-cocDiff * 1.5);

        colorSum += sampleColor * weight;
        weightSum += weight;
    }

    // Fallback to center color if no valid samples
    if (weightSum < 0.0001) {
        return gColorInput.SampleLevel(gLinearSampler, input.uv, 0);
    }

    return colorSum / weightSum;
}
