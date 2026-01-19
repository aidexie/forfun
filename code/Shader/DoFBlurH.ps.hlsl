// ============================================
// Depth of Field - Pass 3: Horizontal Gaussian Blur
// ============================================
// 11-tap separable Gaussian blur with bilateral weighting
// Blurs near and far layers separately based on CoC
// ============================================

cbuffer CB_Blur : register(b0) {
    float2 gTexelSize;
    float gMaxCoCRadius;
    int gSampleCount;
};

Texture2D gColorInput : register(t0);
Texture2D gCoCInput : register(t1);
SamplerState gLinearSampler : register(s0);

static const float2 kBlurDir = float2(1.0, 0.0);  // Horizontal

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

    // Scale blur radius by CoC
    float blurRadius = centerCoC * gMaxCoCRadius;

    float4 colorSum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;

    // 11-tap Gaussian blur
    for (int i = 0; i < 11; ++i) {
        float offset = (float)(i - 5);  // -5 to +5
        float2 sampleUV = input.uv + kBlurDir * gTexelSize * offset * blurRadius;

        // Clamp to valid UV range
        sampleUV = saturate(sampleUV);

        float4 sampleColor = gColorInput.SampleLevel(gLinearSampler, sampleUV, 0);
        float sampleCoC = gCoCInput.SampleLevel(gLinearSampler, sampleUV, 0).r;

        float weight = kGaussianWeights[i];

        // Bilateral weight: reduce contribution of samples with very different CoC
        float cocDiff = abs(sampleCoC - centerCoC);
        weight *= exp(-cocDiff * 4.0);

        colorSum += sampleColor * weight;
        weightSum += weight;
    }

    return colorSum / max(weightSum, 0.0001);
}
