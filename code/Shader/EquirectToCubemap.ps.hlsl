// Equirectangular to Cubemap Conversion - Pixel Shader
// Converts equirectangular HDR map to cubemap faces

Texture2D equirectMap : register(t0);
SamplerState samp : register(s0);

static const float PI = 3.14159265359;
static const float INV_2PI = 0.15915494309189533577;  // 1/(2*PI)
static const float INV_PI = 0.31830988618379067154;   // 1/PI

float2 SampleSphericalMap(float3 v) {
    // Equirectangular mapping for DirectX left-handed coordinate system
    // Goal: +Z (forward) → U=0.5 (center), +X (right) → U=0.75
    float2 uv;

    // Horizontal mapping (longitude):
    uv.x = atan2(v.x, v.z) * INV_2PI + 0.5;

    // Vertical mapping (latitude):
    // - asin(v.y) maps sphere Y to angle [-PI/2, PI/2]
    // - Negate to flip: HDR images have sky at top (v=0), ground at bottom (v=1)
    // - Result: +Y(up)→0.0, 0(horizon)→0.5, -Y(down)→1.0
    uv.y = -asin(v.y) * INV_PI + 0.5;

    return uv;
}

struct PSIn {
    float4 posH : SV_Position;
    float3 localPos : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target {
    float3 dir = normalize(input.localPos);
    float2 uv = SampleSphericalMap(dir);
    float3 color = equirectMap.Sample(samp, uv).rgb;

    return float4(color, 1.0);
}
