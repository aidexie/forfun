// Skybox Pixel Shader
// Samples HDR environment cubemap

TextureCube envMap : register(t0);
SamplerState samp : register(s0);

struct PSIn {
    float4 posH : SV_Position;
    float3 localPos : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target {
    float3 envColor = envMap.Sample(samp, input.localPos).rgb;

    // Output HDR linear space (no tone mapping, no gamma correction)
    // Post-process pass will handle tone mapping and gamma correction
    return float4(envColor, 1.0);
}
