// FXAA 3.11 - Fast Approximate Anti-Aliasing
// Single-pass algorithm detecting edges via local contrast and applying directional blur.
// Reference: Timothy Lottes (NVIDIA, 2009)
//
// SM 5.1 with register spaces for descriptor set binding
// Uses space1 (PerPass) for all bindings

cbuffer CB_FXAA : register(b0, space1) {
    float2 gRcpFrame;           // 1.0 / resolution
    float gSubpixelQuality;     // Subpixel AA quality (0.0 = off, 1.0 = max blur)
    float gEdgeThreshold;       // Edge detection threshold (0.063 - 0.333)
    float gEdgeThresholdMin;    // Minimum edge threshold (0.0312 - 0.125)
    float3 _pad;
};

Texture2D<float4> gInputTexture : register(t0, space1);
SamplerState gLinearSampler : register(s0, space1);

float RGBToLuma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float4 SampleOffset(float2 uv, float2 offset) {
    return gInputTexture.SampleLevel(gLinearSampler, uv + offset * gRcpFrame, 0);
}

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float2 uv = input.uv;

    // Sample center and 4 neighbors
    float3 rgbM = gInputTexture.SampleLevel(gLinearSampler, uv, 0).rgb;
    float3 rgbN = SampleOffset(uv, float2(0, -1)).rgb;
    float3 rgbS = SampleOffset(uv, float2(0, 1)).rgb;
    float3 rgbE = SampleOffset(uv, float2(1, 0)).rgb;
    float3 rgbW = SampleOffset(uv, float2(-1, 0)).rgb;

    // Convert to luma
    float lumaM = RGBToLuma(rgbM);
    float lumaN = RGBToLuma(rgbN);
    float lumaS = RGBToLuma(rgbS);
    float lumaE = RGBToLuma(rgbE);
    float lumaW = RGBToLuma(rgbW);

    // Find min/max luma in neighborhood
    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    // Early exit: skip low-contrast areas (no visible aliasing)
    if (lumaRange < max(gEdgeThresholdMin, lumaMax * gEdgeThreshold)) {
        return float4(rgbM, 1.0);
    }

    // Sample diagonal neighbors for subpixel aliasing
    float3 rgbNW = SampleOffset(uv, float2(-1, -1)).rgb;
    float3 rgbNE = SampleOffset(uv, float2(1, -1)).rgb;
    float3 rgbSW = SampleOffset(uv, float2(-1, 1)).rgb;
    float3 rgbSE = SampleOffset(uv, float2(1, 1)).rgb;

    float lumaNW = RGBToLuma(rgbNW);
    float lumaNE = RGBToLuma(rgbNE);
    float lumaSW = RGBToLuma(rgbSW);
    float lumaSE = RGBToLuma(rgbSE);

    // Compute edge direction
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaNWSE = lumaNW + lumaSE;
    float lumaNESW = lumaNE + lumaSW;

    // Horizontal vs vertical edge detection
    float edgeHorz = abs(lumaNWSW - 2.0 * lumaW) + abs(lumaNS - 2.0 * lumaM) * 2.0 + abs(lumaNESW - 2.0 * lumaE);
    float edgeVert = abs(lumaNWSE - 2.0 * lumaN) + abs(lumaWE - 2.0 * lumaM) * 2.0 + abs(lumaNESW - 2.0 * lumaS);
    bool isHorizontal = edgeHorz >= edgeVert;

    // Select edge endpoints based on direction
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = abs(luma1 - lumaM);
    float gradient2 = abs(luma2 - lumaM);

    // Determine which side has steeper gradient
    bool is1Steeper = gradient1 >= gradient2;
    float gradientScaled = 0.25 * max(gradient1, gradient2);

    // Step size in pixels
    float stepLength = isHorizontal ? gRcpFrame.y : gRcpFrame.x;
    float lumaLocalAvg;

    if (is1Steeper) {
        stepLength = -stepLength;
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    // Shift UV to edge
    float2 currentUV = uv;
    if (isHorizontal) {
        currentUV.y += stepLength * 0.5;
    } else {
        currentUV.x += stepLength * 0.5;
    }

    // Search along edge in both directions
    float2 offset = isHorizontal ? float2(gRcpFrame.x, 0) : float2(0, gRcpFrame.y);
    float2 uv1 = currentUV - offset;
    float2 uv2 = currentUV + offset;

    // Search iterations
    bool reached1 = false;
    bool reached2 = false;
    float lumaEnd1, lumaEnd2;

    // Quality settings: number of search steps
    const int SEARCH_STEPS = 12;
    const float SEARCH_ACCELERATION = 1.0;

    [unroll]
    for (int i = 0; i < SEARCH_STEPS; i++) {
        if (!reached1) {
            lumaEnd1 = RGBToLuma(gInputTexture.SampleLevel(gLinearSampler, uv1, 0).rgb);
            lumaEnd1 -= lumaLocalAvg;
        }
        if (!reached2) {
            lumaEnd2 = RGBToLuma(gInputTexture.SampleLevel(gLinearSampler, uv2, 0).rgb);
            lumaEnd2 -= lumaLocalAvg;
        }

        reached1 = abs(lumaEnd1) >= gradientScaled;
        reached2 = abs(lumaEnd2) >= gradientScaled;

        if (reached1 && reached2) break;

        if (!reached1) uv1 -= offset * SEARCH_ACCELERATION;
        if (!reached2) uv2 += offset * SEARCH_ACCELERATION;
    }

    // Compute distances to edge endpoints
    float dist1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);

    // Determine which endpoint is closer
    bool isDir1 = dist1 < dist2;
    float distFinal = min(dist1, dist2);
    float edgeLength = dist1 + dist2;

    // Compute pixel offset
    float pixelOffset = -distFinal / edgeLength + 0.5;

    // Check if center luma is on correct side of local average
    bool isLumaMSmaller = lumaM < lumaLocalAvg;
    bool correctVariation = ((isDir1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaMSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Subpixel anti-aliasing
    float lumaAvg = (1.0 / 12.0) * (2.0 * (lumaNS + lumaWE) + lumaNWSW + lumaNESW);
    float subpixelOffset1 = saturate(abs(lumaAvg - lumaM) / lumaRange);
    float subpixelOffset2 = (-2.0 * subpixelOffset1 + 3.0) * subpixelOffset1 * subpixelOffset1;
    float subpixelOffsetFinal = subpixelOffset2 * subpixelOffset2 * gSubpixelQuality;

    // Use larger of edge offset and subpixel offset
    finalOffset = max(finalOffset, subpixelOffsetFinal);

    // Apply final offset and sample
    float2 finalUV = uv;
    if (isHorizontal) {
        finalUV.y += finalOffset * stepLength;
    } else {
        finalUV.x += finalOffset * stepLength;
    }

    float3 finalColor = gInputTexture.SampleLevel(gLinearSampler, finalUV, 0).rgb;
    return float4(finalColor, 1.0);
}
