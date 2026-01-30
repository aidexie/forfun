// TestDescriptorSet Pixel Shader
// SM 5.1 with register spaces for descriptor set testing

// Set 0 (space0): PerFrame resources
Texture2D gTestTexture : register(t0, space0);
SamplerState gSampler : register(s0, space0);

// Set 0 (space0): PerFrame constants
cbuffer CB_PerFrame : register(b0, space0) {
    float4x4 gViewProj;
    float gTime;
    float3 _pad0;
};

// Set 2 (space2): PerMaterial constants (using VolatileCBV)
cbuffer CB_Material : register(b0, space2) {
    float4 gTintColor;
    float gIntensity;
    float3 _pad1;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    // Sample texture
    float4 texColor = gTestTexture.Sample(gSampler, input.uv);

    // Apply tint and intensity
    float4 result = texColor * gTintColor * gIntensity;

    // Add time-based animation for visual verification
    float pulse = 0.5 + 0.5 * sin(gTime * 2.0);
    result.rgb *= 0.8 + 0.2 * pulse;

    return result;
}
