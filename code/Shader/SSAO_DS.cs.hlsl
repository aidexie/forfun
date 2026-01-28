// ============================================
// SSAO_DS.cs.hlsl - Screen-Space Ambient Occlusion (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
//
// Entry Points:
//   CSMain              - SSAO compute at half-res
//   CSBlurH             - Horizontal bilateral blur
//   CSBlurV             - Vertical bilateral blur
//   CSBilateralUpsample - Edge-preserving upsample to full-res
//   CSDownsampleDepth   - Depth downsample for bilateral upsample
// ============================================

#define PI 3.14159265359
#define HALF_PI 1.5707963268

// Algorithm IDs
#define SSAO_ALGORITHM_GTAO   0
#define SSAO_ALGORITHM_HBAO   1
#define SSAO_ALGORITHM_CRYTEK 2

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_SSAO : register(b0, space1) {
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gView;
    float2 gTexelSize;
    float2 gNoiseScale;
    float gRadius;
    float gIntensity;
    float gFalloffStart;
    float gFalloffEnd;
    int gNumSlices;
    int gNumSteps;
    float gThicknessHeuristic;
    int gAlgorithm;
    uint gUseReversedZ;
    float3 _pad;
};

// Texture SRVs (t0-t7, space1)
Texture2D<float> gDepth : register(t0, space1);
Texture2D<float4> gNormal : register(t1, space1);
Texture2D<float4> gNoise : register(t2, space1);
Texture2D<float> gSSAOInput : register(t3, space1);    // For blur passes
Texture2D<float> gDepthInput : register(t4, space1);   // For blur/upsample
Texture2D<float> gDepthFullRes : register(t5, space1); // For upsample

// UAVs (u0-u3, space1)
RWTexture2D<float> gSSAOOutput : register(u0, space1);

// Samplers (s0-s3, space1)
SamplerState gPointSampler : register(s0, space1);
SamplerState gLinearSampler : register(s1, space1);

// ============================================
// Helper Functions
// ============================================

float3 ReconstructViewPos(float2 uv, float depth) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 viewPos = mul(clipPos, gInvProj);
    return viewPos.xyz / viewPos.w;
}

float3 GetViewNormal(float2 uv) {
    float3 worldNormal = gNormal.SampleLevel(gPointSampler, uv, 0).xyz;
    float3 viewNormal = mul(float4(worldNormal, 0), gView).xyz;
    return normalize(viewNormal);
}

float ComputeFalloff(float dist, float radius) {
    float t = saturate((dist - gFalloffStart * radius) / ((gFalloffEnd - gFalloffStart) * radius));
    return 1.0 - t * t;
}

bool IsSkyDepth(float depth) {
    return gUseReversedZ ? (depth <= 0.001) : (depth >= 0.999);
}

// ============================================
// GTAO Functions
// ============================================

float GTAOIntegral(float cosN, float sinN, float h, float n) {
    float cos2hMinusN = cos(2.0 * h - n);
    return 0.25 * (-cos2hMinusN + cosN + 2.0 * h * sinN);
}

float ComputeSliceAO(float h1, float h2, float n) {
    float cosN = cos(n);
    float sinN = sin(n);
    h1 = max(h1, n - HALF_PI);
    h2 = min(h2, n + HALF_PI);
    return GTAOIntegral(cosN, sinN, h2, n) + GTAOIntegral(cosN, sinN, h1, n);
}

float ComputeGTAO(float3 viewPos, float3 viewNormal, float2 uv, float2 noiseVec) {
    float totalAO = 0.0;
    float screenRadius = gRadius * gProj[1][1] * 0.5 / max(abs(viewPos.z), 0.1);
    float sliceAngleStep = PI / float(gNumSlices);

    for (int slice = 0; slice < gNumSlices; slice++) {
        float sliceAngle = float(slice) * sliceAngleStep;
        float cosA = cos(sliceAngle);
        float sinA = sin(sliceAngle);

        float2 sliceDir;
        sliceDir.x = cosA * noiseVec.x - sinA * noiseVec.y;
        sliceDir.y = sinA * noiseVec.x + cosA * noiseVec.y;

        float3 sliceDirVS = float3(sliceDir.x, sliceDir.y, 0);
        float3 orthoDir = normalize(cross(sliceDirVS, viewNormal));
        float3 projNormal = viewNormal - dot(viewNormal, orthoDir) * orthoDir;
        float projNormalLen = length(projNormal);

        float signN = sign(dot(projNormal, sliceDirVS));
        float cosN = saturate(dot(normalize(projNormal), float3(0, 0, -1)));
        float n = signN * acos(cosN);

        float h1 = -HALF_PI;
        float h2 = HALF_PI;

        float stepSize = screenRadius / float(gNumSteps);

        [unroll]
        for (int step = 1; step <= 8; step++) {
            if (step > gNumSteps) break;

            float2 sampleOffset = sliceDir * stepSize * float(step);

            // Positive direction
            float2 sampleUV_pos = uv + sampleOffset;
            if (sampleUV_pos.x >= 0 && sampleUV_pos.x <= 1 && sampleUV_pos.y >= 0 && sampleUV_pos.y <= 1) {
                float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV_pos, 0);
                if (!IsSkyDepth(sampleDepth)) {
                    float3 samplePos = ReconstructViewPos(sampleUV_pos, sampleDepth);
                    float3 diff = samplePos - viewPos;
                    float dist = length(diff);
                    if (dist > 0.001 && dist < gRadius) {
                        float falloff = ComputeFalloff(dist, gRadius);
                        float sampleH = atan2(diff.z, length(diff.xy));
                        h2 = max(h2, sampleH * falloff + (1 - falloff) * h2);
                    }
                }
            }

            // Negative direction
            float2 sampleUV_neg = uv - sampleOffset;
            if (sampleUV_neg.x >= 0 && sampleUV_neg.x <= 1 && sampleUV_neg.y >= 0 && sampleUV_neg.y <= 1) {
                float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV_neg, 0);
                if (!IsSkyDepth(sampleDepth)) {
                    float3 samplePos = ReconstructViewPos(sampleUV_neg, sampleDepth);
                    float3 diff = samplePos - viewPos;
                    float dist = length(diff);
                    if (dist > 0.001 && dist < gRadius) {
                        float falloff = ComputeFalloff(dist, gRadius);
                        float sampleH = atan2(diff.z, length(diff.xy));
                        h1 = min(h1, -sampleH * falloff + (1 - falloff) * h1);
                    }
                }
            }
        }

        totalAO += ComputeSliceAO(h1, h2, n) * projNormalLen;
    }

    return saturate(1.0 - totalAO / float(gNumSlices) * gIntensity);
}

// ============================================
// Main SSAO Compute Shader
// ============================================

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    uint width, height;
    gSSAOOutput.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height) return;

    float2 uv = (float2(DTid.xy) + 0.5) * gTexelSize;
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0);

    if (IsSkyDepth(depth)) {
        gSSAOOutput[DTid.xy] = 1.0;
        return;
    }

    float3 viewPos = ReconstructViewPos(uv, depth);
    float3 viewNormal = GetViewNormal(uv);

    float2 noiseUV = uv * gNoiseScale;
    float2 noiseVec = gNoise.SampleLevel(gPointSampler, noiseUV, 0).xy * 2.0 - 1.0;
    noiseVec = normalize(noiseVec);

    float ao = ComputeGTAO(viewPos, viewNormal, uv, noiseVec);
    gSSAOOutput[DTid.xy] = ao;
}

// ============================================
// Blur Shaders
// ============================================

// Blur constant buffer uses same b0 slot with different data
// gTexelSize.xy = blur direction (1,0) or (0,1)
// gRadius (reused) = blur radius
// gIntensity (reused) = depth sigma

[numthreads(8, 8, 1)]
void CSBlurH(uint3 DTid : SV_DispatchThreadID) {
    uint width, height;
    gSSAOOutput.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height) return;

    float2 uv = (float2(DTid.xy) + 0.5) * gTexelSize;
    float centerAO = gSSAOInput.SampleLevel(gPointSampler, uv, 0);
    float centerDepth = gDepthInput.SampleLevel(gPointSampler, uv, 0);

    float totalAO = centerAO;
    float totalWeight = 1.0;

    int blurRadius = int(gRadius);
    float depthSigma = gIntensity;

    [unroll]
    for (int i = -4; i <= 4; i++) {
        if (i == 0 || abs(i) > blurRadius) continue;

        float2 sampleUV = uv + float2(float(i) * gTexelSize.x, 0);
        float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0);
        float sampleDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0);

        float depthDiff = abs(centerDepth - sampleDepth);
        float weight = exp(-depthDiff * depthSigma);

        totalAO += sampleAO * weight;
        totalWeight += weight;
    }

    gSSAOOutput[DTid.xy] = totalAO / totalWeight;
}

[numthreads(8, 8, 1)]
void CSBlurV(uint3 DTid : SV_DispatchThreadID) {
    uint width, height;
    gSSAOOutput.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height) return;

    float2 uv = (float2(DTid.xy) + 0.5) * gTexelSize;
    float centerAO = gSSAOInput.SampleLevel(gPointSampler, uv, 0);
    float centerDepth = gDepthInput.SampleLevel(gPointSampler, uv, 0);

    float totalAO = centerAO;
    float totalWeight = 1.0;

    int blurRadius = int(gRadius);
    float depthSigma = gIntensity;

    [unroll]
    for (int i = -4; i <= 4; i++) {
        if (i == 0 || abs(i) > blurRadius) continue;

        float2 sampleUV = uv + float2(0, float(i) * gTexelSize.y);
        float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0);
        float sampleDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0);

        float depthDiff = abs(centerDepth - sampleDepth);
        float weight = exp(-depthDiff * depthSigma);

        totalAO += sampleAO * weight;
        totalWeight += weight;
    }

    gSSAOOutput[DTid.xy] = totalAO / totalWeight;
}

// ============================================
// Upsample Shader
// ============================================

[numthreads(8, 8, 1)]
void CSBilateralUpsample(uint3 DTid : SV_DispatchThreadID) {
    uint width, height;
    gSSAOOutput.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height) return;

    float2 uv = (float2(DTid.xy) + 0.5) * gTexelSize;
    float fullResDepth = gDepthFullRes.SampleLevel(gPointSampler, uv, 0);

    // Bilateral upsample from half-res
    float totalAO = 0;
    float totalWeight = 0;

    float depthSigma = gIntensity;

    [unroll]
    for (int y = -1; y <= 1; y++) {
        [unroll]
        for (int x = -1; x <= 1; x++) {
            float2 sampleUV = uv + float2(x, y) * gNoiseScale; // gNoiseScale = halfResTexelSize
            float halfResAO = gSSAOInput.SampleLevel(gLinearSampler, sampleUV, 0);
            float halfResDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0);

            float depthDiff = abs(fullResDepth - halfResDepth);
            float weight = exp(-depthDiff * depthSigma);

            totalAO += halfResAO * weight;
            totalWeight += weight;
        }
    }

    gSSAOOutput[DTid.xy] = totalWeight > 0 ? totalAO / totalWeight : 1.0;
}

// ============================================
// Downsample Shader
// ============================================

[numthreads(8, 8, 1)]
void CSDownsampleDepth(uint3 DTid : SV_DispatchThreadID) {
    uint width, height;
    gSSAOOutput.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height) return;

    float2 uv = (float2(DTid.xy) + 0.5) * gTexelSize;

    // Sample 4 depths and take min (closest) for reversed-Z, max for standard-Z
    float d0 = gDepth.SampleLevel(gPointSampler, uv + float2(-0.25, -0.25) * gTexelSize, 0);
    float d1 = gDepth.SampleLevel(gPointSampler, uv + float2( 0.25, -0.25) * gTexelSize, 0);
    float d2 = gDepth.SampleLevel(gPointSampler, uv + float2(-0.25,  0.25) * gTexelSize, 0);
    float d3 = gDepth.SampleLevel(gPointSampler, uv + float2( 0.25,  0.25) * gTexelSize, 0);

    float result;
    if (gUseReversedZ) {
        result = max(max(d0, d1), max(d2, d3)); // Reversed-Z: max = closest
    } else {
        result = min(min(d0, d1), min(d2, d3)); // Standard-Z: min = closest
    }

    gSSAOOutput[DTid.xy] = result;
}
