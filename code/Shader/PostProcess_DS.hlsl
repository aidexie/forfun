// ============================================
// PostProcess_DS.hlsl - Post-Process (SM 5.1)
// ============================================
// Descriptor Set version using PerPass layout (space1)
// Handles tone mapping, color grading, and gamma correction
//
// Entry Points:
//   VSMain - Fullscreen vertex shader
//   PSMain - Tone mapping + color grading pixel shader
// ============================================

// ============================================
// Set 1: PerPass (space1)
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_PostProcess : register(b0, space1) {
    float gExposure;
    int gUseExposureBuffer;
    float gBloomIntensity;
    float _pad0;

    float3 gLift;
    float gSaturation;

    float3 gGamma;
    float gContrast;

    float3 gGain;
    float gTemperature;

    float gLutContribution;
    int gColorGradingEnabled;
    float2 _pad1;
};

// Texture SRVs (t0-t3, space1)
Texture2D gHDRTexture : register(t0, space1);
Texture2D gBloomTexture : register(t1, space1);
Texture3D gLUTTexture : register(t2, space1);
StructuredBuffer<float> gExposureBuffer : register(t3, space1);

// Sampler (s0, space1)
SamplerState gSampler : register(s0, space1);

// ============================================
// Structures
// ============================================
struct VSIn {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// ============================================
// Vertex Shader
// ============================================
VSOut VSMain(VSIn input) {
    VSOut output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

// ============================================
// Helper Functions
// ============================================

// ACES Filmic Tone Mapping
float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Lift/Gamma/Gain (ASC CDL style)
float3 ApplyLiftGammaGain(float3 color, float3 lift, float3 gamma, float3 gain) {
    // Lift: offset in shadows (add)
    color = color + lift * 0.1;

    // Gamma: power curve in midtones
    float3 gammaAdj = 1.0 / (1.0 + gamma);
    color = pow(max(color, 0.0001), gammaAdj);

    // Gain: multiply in highlights
    color = color * (1.0 + gain * 0.5);

    return color;
}

// Saturation adjustment
float3 ApplySaturation(float3 color, float saturation) {
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(float3(luma, luma, luma), color, 1.0 + saturation);
}

// Contrast adjustment (around 0.5 pivot)
float3 ApplyContrast(float3 color, float contrast) {
    return (color - 0.5) * (1.0 + contrast) + 0.5;
}

// Temperature adjustment (blue-orange axis)
float3 ApplyTemperature(float3 color, float temperature) {
    float3 warm = float3(1.05, 1.0, 0.95);
    float3 cool = float3(0.95, 1.0, 1.05);
    float3 tint = lerp(cool, warm, temperature * 0.5 + 0.5);
    return color * tint;
}

// 3D LUT sampling
float3 ApplyLUT(float3 color, float contribution) {
    if (contribution <= 0.0) return color;

    float lutSize = 32.0;
    float3 scale = (lutSize - 1.0) / lutSize;
    float3 offset = 0.5 / lutSize;
    float3 uvw = saturate(color) * scale + offset;

    float3 lutColor = gLUTTexture.Sample(gSampler, uvw).rgb;
    return lerp(color, lutColor, contribution);
}

// ============================================
// Pixel Shader
// ============================================
float4 PSMain(VSOut input) : SV_Target {
    // Sample HDR input (linear space)
    float3 hdrColor = gHDRTexture.Sample(gSampler, input.uv).rgb;

    // Add bloom contribution
    if (gBloomIntensity > 0.0) {
        float3 bloom = gBloomTexture.Sample(gSampler, input.uv).rgb;
        hdrColor += bloom * gBloomIntensity;
    }

    // Apply exposure
    float exposure = gExposure;
    if (gUseExposureBuffer) {
        exposure = gExposureBuffer[0];
    }
    hdrColor *= exposure;

    // Tone mapping: HDR -> LDR [0, 1]
    float3 ldrColor = ACESFilm(hdrColor);

    // Color grading (LDR space)
    if (gColorGradingEnabled) {
        ldrColor = ApplyLiftGammaGain(ldrColor, gLift, gGamma, gGain);
        ldrColor = ApplySaturation(ldrColor, gSaturation);
        ldrColor = ApplyContrast(ldrColor, gContrast);
        ldrColor = ApplyTemperature(ldrColor, gTemperature);
        ldrColor = ApplyLUT(ldrColor, gLutContribution);
        ldrColor = saturate(ldrColor);
    }

    return float4(ldrColor, 1.0);
}
