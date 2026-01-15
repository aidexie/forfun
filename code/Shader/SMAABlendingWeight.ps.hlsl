// ============================================
// SMAABlendingWeight.ps.hlsl - SMAA Blending Weight Calculation
// ============================================
// Second pass of SMAA: Calculates blending weights using pattern matching.
// Uses pre-computed AreaTex and SearchTex lookup tables.
//
// Reference:
//   "SMAA: Enhanced Subpixel Morphological Antialiasing"
//   Jorge Jimenez et al. (2012)
//   http://www.iryoku.com/smaa/
//
// Entry Point: main
// ============================================

// ============================================
// SMAA Configuration
// ============================================
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)

// ============================================
// Constant Buffer
// ============================================
cbuffer CB_SMAABlend : register(b0) {
    float4 gRTMetrics;  // (1/width, 1/height, width, height)
};

// ============================================
// Textures and Samplers
// ============================================
Texture2D<float2> gEdgesTex : register(t0);    // Edge detection output
Texture2D<float4> gAreaTex : register(t1);     // Pre-computed area texture
Texture2D<float> gSearchTex : register(t2);    // Pre-computed search texture
SamplerState gLinearSampler : register(s0);
SamplerState gPointSampler : register(s1);

// ============================================
// Helper Functions
// ============================================

// Decode search texture value
float SearchLength(float2 e, float offset) {
    // Scale and bias for search texture lookup
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);

    scale += float2(-1.0, 1.0);
    bias += float2(0.5, -0.5);

    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;

    return gSearchTex.SampleLevel(gLinearSampler, e * scale + bias, 0);
}

// Search for edge end (horizontal)
float SearchXLeft(float2 uv, float end) {
    float2 e = float2(0.0, 1.0);

    [loop]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; i++) {
        if (uv.x <= end || e.g <= 0.8281) break;

        e = gEdgesTex.SampleLevel(gLinearSampler, uv, 0);
        uv -= float2(2.0, 0.0) * gRTMetrics.xy;
    }

    float offset = -(255.0 / 127.0) * SearchLength(e, 0.0) + 3.25;
    return gRTMetrics.x * offset + uv.x;
}

float SearchXRight(float2 uv, float end) {
    float2 e = float2(0.0, 1.0);

    [loop]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; i++) {
        if (uv.x >= end || e.g <= 0.8281) break;

        e = gEdgesTex.SampleLevel(gLinearSampler, uv, 0);
        uv += float2(2.0, 0.0) * gRTMetrics.xy;
    }

    float offset = -(255.0 / 127.0) * SearchLength(e, 0.5) + 3.25;
    return -gRTMetrics.x * offset + uv.x;
}

// Search for edge end (vertical)
float SearchYUp(float2 uv, float end) {
    float2 e = float2(1.0, 0.0);

    [loop]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; i++) {
        if (uv.y <= end || e.r <= 0.8281) break;

        e = gEdgesTex.SampleLevel(gLinearSampler, uv, 0);
        uv -= float2(0.0, 2.0) * gRTMetrics.xy;
    }

    float offset = -(255.0 / 127.0) * SearchLength(e.gr, 0.0) + 3.25;
    return gRTMetrics.y * offset + uv.y;
}

float SearchYDown(float2 uv, float end) {
    float2 e = float2(1.0, 0.0);

    [loop]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; i++) {
        if (uv.y >= end || e.r <= 0.8281) break;

        e = gEdgesTex.SampleLevel(gLinearSampler, uv, 0);
        uv += float2(0.0, 2.0) * gRTMetrics.xy;
    }

    float offset = -(255.0 / 127.0) * SearchLength(e.gr, 0.5) + 3.25;
    return -gRTMetrics.y * offset + uv.y;
}

// Sample area texture for blending weights
float2 Area(float2 dist, float e1, float e2, float offset) {
    // Remap to area texture coordinates
    float2 texcoord = SMAA_AREATEX_MAX_DISTANCE * round(4.0 * float2(e1, e2)) + dist;

    // Apply subpixel offset
    texcoord = SMAA_AREATEX_PIXEL_SIZE * (texcoord + 0.5);
    texcoord.y = SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;

    return gAreaTex.SampleLevel(gLinearSampler, texcoord, 0).rg;
}

// ============================================
// Blending Weight Calculation
// ============================================
struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float2 uv = input.uv;
    float4 weights = float4(0.0, 0.0, 0.0, 0.0);

    float2 e = gEdgesTex.SampleLevel(gPointSampler, uv, 0);

    // Edge at left?
    [branch]
    if (e.g > 0.0) {
        // Search for edge endpoints
        float2 d;
        float3 coords;

        coords.x = SearchXLeft(uv - float2(0.0, 0.25) * gRTMetrics.xy, 0.0);
        coords.y = uv.y;
        d.x = coords.x;

        float e1 = gEdgesTex.SampleLevel(gLinearSampler, coords.xy, 0).r;

        coords.z = SearchXRight(uv + float2(0.0, 0.25) * gRTMetrics.xy, 1.0);
        d.y = coords.z;

        d = abs(round(gRTMetrics.zz * d - input.position.xx));

        float2 sqrt_d = sqrt(d);

        float e2 = gEdgesTex.SampleLevel(gLinearSampler, float2(coords.z + gRTMetrics.x, coords.y), 0).r;

        weights.rg = Area(sqrt_d, e1, e2, 0.0);
    }

    // Edge at top?
    [branch]
    if (e.r > 0.0) {
        // Search for edge endpoints
        float2 d;
        float3 coords;

        coords.y = SearchYUp(uv - float2(0.25, 0.0) * gRTMetrics.xy, 0.0);
        coords.x = uv.x;
        d.x = coords.y;

        float e1 = gEdgesTex.SampleLevel(gLinearSampler, coords.xy, 0).g;

        coords.z = SearchYDown(uv + float2(0.25, 0.0) * gRTMetrics.xy, 1.0);
        d.y = coords.z;

        d = abs(round(gRTMetrics.ww * d - input.position.yy));

        float2 sqrt_d = sqrt(d);

        float e2 = gEdgesTex.SampleLevel(gLinearSampler, float2(coords.x, coords.z + gRTMetrics.y), 0).g;

        weights.ba = Area(sqrt_d, e1, e2, 0.0);
    }

    return weights;
}
