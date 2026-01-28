// ============================================
// Bloom_DS.ps.hlsl - Bloom Post-Process (SM 5.1)
// ============================================
// Descriptor Set version using PerPass layout (space1)
//
// Entry Points:
//   VSMain      - Fullscreen vertex shader
//   PSThreshold - Extract bright pixels
//   PSDownsample - Kawase 5-tap downsample
//   PSUpsample  - 9-tap tent filter upsample
// ============================================

// ============================================
// Set 1: PerPass (space1)
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_Bloom : register(b0, space1) {
    float2 gTexelSize;
    float gThreshold;
    float gSoftKnee;
    float gScatter;
    float3 _pad;
};

// Texture SRV (t0, space1)
Texture2D gInput : register(t0, space1);

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
float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 SoftThreshold(float3 color, float threshold, float knee) {
    float luma = Luminance(color);
    float soft = luma - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(soft, luma - threshold);
    contribution /= max(luma, 1e-5);
    return color * saturate(contribution);
}

// ============================================
// Threshold Pixel Shader
// ============================================
float4 PSThreshold(VSOut input) : SV_Target {
    float3 color = gInput.Sample(gSampler, input.uv).rgb;
    float3 bloom = SoftThreshold(color, gThreshold, gSoftKnee * gThreshold);
    bloom = min(bloom, 10.0);  // Clamp fireflies
    return float4(bloom, 1.0);
}

// ============================================
// Downsample Pixel Shader (Kawase 5-tap)
// ============================================
float4 PSDownsample(VSOut input) : SV_Target {
    float2 uv = input.uv;

    float3 center = gInput.Sample(gSampler, uv).rgb;
    float3 tl = gInput.Sample(gSampler, uv + float2(-1.0, -1.0) * gTexelSize).rgb;
    float3 tr = gInput.Sample(gSampler, uv + float2( 1.0, -1.0) * gTexelSize).rgb;
    float3 bl = gInput.Sample(gSampler, uv + float2(-1.0,  1.0) * gTexelSize).rgb;
    float3 br = gInput.Sample(gSampler, uv + float2( 1.0,  1.0) * gTexelSize).rgb;

    float3 result = center * 4.0 + tl + tr + bl + br;
    result *= (1.0 / 8.0);

    return float4(result, 1.0);
}

// ============================================
// Upsample Pixel Shader (9-tap tent filter)
// ============================================
float4 PSUpsample(VSOut input) : SV_Target {
    float2 uv = input.uv;

    // 9-tap tent filter
    float3 s0 = gInput.Sample(gSampler, uv + float2(-1.0, -1.0) * gTexelSize).rgb;
    float3 s1 = gInput.Sample(gSampler, uv + float2( 0.0, -1.0) * gTexelSize).rgb;
    float3 s2 = gInput.Sample(gSampler, uv + float2( 1.0, -1.0) * gTexelSize).rgb;
    float3 s3 = gInput.Sample(gSampler, uv + float2(-1.0,  0.0) * gTexelSize).rgb;
    float3 s4 = gInput.Sample(gSampler, uv).rgb;
    float3 s5 = gInput.Sample(gSampler, uv + float2( 1.0,  0.0) * gTexelSize).rgb;
    float3 s6 = gInput.Sample(gSampler, uv + float2(-1.0,  1.0) * gTexelSize).rgb;
    float3 s7 = gInput.Sample(gSampler, uv + float2( 0.0,  1.0) * gTexelSize).rgb;
    float3 s8 = gInput.Sample(gSampler, uv + float2( 1.0,  1.0) * gTexelSize).rgb;

    float3 result = s0 + s2 + s6 + s8;
    result += (s1 + s3 + s5 + s7) * 2.0;
    result += s4 * 4.0;
    result *= (1.0 / 16.0);

    // Apply scatter factor to control contribution from lower mip
    result *= gScatter;

    return float4(result, 1.0);
}
