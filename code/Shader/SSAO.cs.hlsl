// ============================================
// SSAO.cs.hlsl - GTAO (Ground Truth Ambient Occlusion)
// ============================================
// Reference: "Practical Real-Time Strategies for Accurate Indirect Occlusion"
//            Jorge Jimenez, Xian-Chun Wu, Angelo Pesce, Adrian Jarabo (2016)
//
// Entry Points:
//   CSMain              - GTAO compute at half-res
//   CSBlurH             - Horizontal bilateral blur
//   CSBlurV             - Vertical bilateral blur
//   CSBilateralUpsample - Edge-preserving upsample to full-res
//   CSDownsampleDepth   - Depth downsample for bilateral upsample
// ============================================

#define PI 3.14159265359
#define HALF_PI 1.5707963268

// ============================================
// Constant Buffers
// ============================================

cbuffer CB_SSAO : register(b0) {
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gView;             // World → View transform
    float2 gTexelSize;          // 1.0 / resolution (half-res)
    float2 gNoiseScale;         // resolution / 4.0 (noise tiling)
    float gRadius;              // AO radius in view-space units
    float gIntensity;           // AO strength multiplier
    float gFalloffStart;        // Distance falloff start (0.0-1.0)
    float gFalloffEnd;          // Distance falloff end (1.0)
    int gNumSlices;             // Number of direction slices (2-4)
    int gNumSteps;              // Steps per direction (4-8)
    float gThicknessHeuristic;  // Thin object heuristic threshold
    float _pad;
};

cbuffer CB_SSAOBlur : register(b0) {
    float2 gBlurDirection;      // (1,0) horizontal, (0,1) vertical
    float2 gBlurTexelSize;
    float gDepthSigma;
    int gBlurRadius;
    float2 _blurPad;
};

cbuffer CB_SSAOUpsample : register(b0) {
    float2 gFullResTexelSize;
    float2 gHalfResTexelSize;
    float gUpsampleDepthSigma;
    float3 _upsamplePad;
};

cbuffer CB_SSAODownsample : register(b0) {
    float2 gDownsampleTexelSize;
    float2 _downsamplePad;
};

// ============================================
// Textures and Samplers
// ============================================

// GTAO inputs
Texture2D<float> gDepth : register(t0);
Texture2D<float4> gNormal : register(t1);
Texture2D<float4> gNoise : register(t2);  // R8G8B8A8, only RG used for rotation

// Blur inputs
Texture2D<float> gSSAOInput : register(t0);
Texture2D<float> gDepthInput : register(t1);

// Upsample inputs
Texture2D<float> gSSAOHalfRes : register(t0);
Texture2D<float> gDepthHalfRes : register(t1);
Texture2D<float> gDepthFullRes : register(t2);

// Downsample input (reuses t0)
Texture2D<float> gDepthSource : register(t0);

SamplerState gPointSampler : register(s0);
SamplerState gLinearSampler : register(s1);

// Outputs
RWTexture2D<float> gSSAOOutput : register(u0);

// ============================================
// Helper Functions
// ============================================

// Reconstruct view-space position from UV and depth
float3 ReconstructViewPos(float2 uv, float depth) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;  // DirectX convention
    float4 viewPos = mul(clipPos, gInvProj);
    return viewPos.xyz / viewPos.w;
}

// Get view-space normal from G-Buffer
float3 GetViewNormal(float2 uv) {
    // G-Buffer RT1 stores world-space normal in xyz
    float3 worldNormal = gNormal.SampleLevel(gPointSampler, uv, 0).xyz;
    // Transform world normal to view space
    float3 viewNormal = mul((float3x3)gView, worldNormal);
    return normalize(viewNormal);
}

// Distance-based falloff
float ComputeFalloff(float dist, float radius) {
    float t = saturate((dist - gFalloffStart * radius) / ((gFalloffEnd - gFalloffStart) * radius));
    return 1.0 - t * t;
}

// ============================================
// GTAO Integral Functions
// ============================================

// Compute the inner integral for a single horizon
// h = horizon angle, n = projected normal angle
float GTAOIntegral(float cosN, float sinN, float h) {
    // Integral: 0.25 * (-cos(2h) + cos^2(n) + 2h*sin(n))
    float cos2h = cos(2.0 * h);
    return 0.25 * (-cos2h + cosN * cosN + 2.0 * h * sinN);
}

// Compute AO contribution from a single slice
float ComputeSliceAO(float h1, float h2, float n) {
    // h1 = horizon angle in negative direction (should be negative)
    // h2 = horizon angle in positive direction (should be positive)
    // n  = projected normal angle

    float cosN = cos(n);
    float sinN = sin(n);

    // Clamp horizons to hemisphere around normal
    h1 = max(h1, n - HALF_PI);
    h2 = min(h2, n + HALF_PI);

    // Compute integral for both horizons
    float ao = GTAOIntegral(cosN, sinN, h2) - GTAOIntegral(cosN, sinN, h1);

    return ao;
}

// ============================================
// Main GTAO Function
// ============================================

float ComputeGTAO(float3 viewPos, float3 viewNormal, float2 uv, float2 noiseVec) {
    float totalAO = 0.0;

    // Radius in screen-space (approximate)
    float screenRadius = gRadius / max(-viewPos.z, 0.1);

    // Slice angle step
    float sliceAngleStep = PI / float(gNumSlices);

    for (int slice = 0; slice < gNumSlices; slice++) {
        // Slice direction in screen space (rotated by noise)
        float sliceAngle = float(slice) * sliceAngleStep;
        float cosA = cos(sliceAngle);
        float sinA = sin(sliceAngle);

        // Apply noise rotation
        float2 sliceDir;
        sliceDir.x = cosA * noiseVec.x - sinA * noiseVec.y;
        sliceDir.y = sinA * noiseVec.x + cosA * noiseVec.y;

        // Convert to view-space direction on the tangent plane
        float3 tangent = normalize(float3(sliceDir.x, sliceDir.y, 0.0));

        // Project normal onto slice plane
        float3 orthoDir = normalize(cross(tangent, float3(0, 0, 1)));
        float3 projNormal = viewNormal - orthoDir * dot(viewNormal, orthoDir);
        float projNormalLen = length(projNormal);

        if (projNormalLen < 0.001) {
            totalAO += 1.0;  // Normal perpendicular to slice
            continue;
        }

        projNormal /= projNormalLen;

        // Normal angle relative to view direction
        float n = atan2(dot(projNormal, tangent), -projNormal.z);

        // Initialize horizons to minimum (looking straight down)
        float h1 = -HALF_PI;  // Negative direction
        float h2 = -HALF_PI;  // Positive direction

        // Step size in UV space
        float uvStepSize = screenRadius * gTexelSize.x / float(gNumSteps);

        // March in negative direction
        for (int s = 1; s <= gNumSteps; s++) {
            float2 sampleUV = uv - sliceDir * gTexelSize * float(s) * (screenRadius / float(gNumSteps));

            // Bounds check
            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (sampleDepth >= 1.0) continue;  // Skip sky

            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist > 0.001 && dist < gRadius) {
                float falloff = ComputeFalloff(dist, gRadius);
                float3 sampleDir = diff / dist;

                // Horizon angle: angle between sample direction and view direction (Z)
                float horizonAngle = atan2(dot(sampleDir, tangent), -sampleDir.z);

                // Blend with falloff
                h1 = max(h1, lerp(-HALF_PI, horizonAngle, falloff));
            }
        }

        // March in positive direction
        for (int s = 1; s <= gNumSteps; s++) {
            float2 sampleUV = uv + sliceDir * gTexelSize * float(s) * (screenRadius / float(gNumSteps));

            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (sampleDepth >= 1.0) continue;

            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist > 0.001 && dist < gRadius) {
                float falloff = ComputeFalloff(dist, gRadius);
                float3 sampleDir = diff / dist;

                float horizonAngle = atan2(dot(sampleDir, tangent), -sampleDir.z);
                h2 = max(h2, lerp(-HALF_PI, horizonAngle, falloff));
            }
        }

        // Compute slice AO (negate h1 for proper integral)
        float sliceAO = ComputeSliceAO(-h1, h2, n);
        totalAO += sliceAO;
    }

    // Average and apply intensity
    totalAO /= float(gNumSlices);

    // Convert to occlusion factor: 1 = fully lit, 0 = fully occluded
    float ao = saturate(1.0 - totalAO * gIntensity);

    return ao;
}

// ============================================
// Main Compute Shader - GTAO
// ============================================

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixelCoord = dispatchThreadID.xy;
    float2 uv = (float2(pixelCoord) + 0.5) * gTexelSize;

    // Sample depth
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;

    // Skip sky pixels
    if (depth >= 1.0) {
        gSSAOOutput[pixelCoord] = 1.0;
        return;
    }

    // Reconstruct view-space position and normal
    float3 viewPos = ReconstructViewPos(uv, depth);
    float3 viewNormal = GetViewNormal(uv);

    // Sample noise for random rotation (tiles across screen)
    float2 noiseUV = uv * gNoiseScale;
    float2 noise = gNoise.SampleLevel(gPointSampler, noiseUV, 0).rg;

    // Convert noise from [0,1] to [-1,1] and normalize
    float2 noiseVec = normalize(noise * 2.0 - 1.0);

    // Compute GTAO
    float ao = ComputeGTAO(viewPos, viewNormal, uv, noiseVec);

    gSSAOOutput[pixelCoord] = ao;
}

// ============================================
// Bilateral Blur - Horizontal
// ============================================

static const float GAUSSIAN_WEIGHTS[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };

[numthreads(8, 8, 1)]
void CSBlurH(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixelCoord = dispatchThreadID.xy;
    float2 uv = (float2(pixelCoord) + 0.5) * gBlurTexelSize;

    float centerDepth = gDepthInput.SampleLevel(gPointSampler, uv, 0).r;
    float centerAO = gSSAOInput.SampleLevel(gPointSampler, uv, 0).r;

    // Skip sky
    if (centerDepth >= 1.0) {
        gSSAOOutput[pixelCoord] = 1.0;
        return;
    }

    float aoSum = centerAO * GAUSSIAN_WEIGHTS[0];
    float weightSum = GAUSSIAN_WEIGHTS[0];

    // Horizontal blur
    for (int i = 1; i <= gBlurRadius; i++) {
        // Positive direction
        {
            float2 sampleUV = uv + float2(float(i), 0.0) * gBlurTexelSize;
            float sampleDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0).r;

            // Bilateral weight based on depth difference
            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * depthDiff / (gDepthSigma * gDepthSigma + 0.0001));

            float weight = GAUSSIAN_WEIGHTS[i] * depthWeight;
            aoSum += sampleAO * weight;
            weightSum += weight;
        }

        // Negative direction
        {
            float2 sampleUV = uv - float2(float(i), 0.0) * gBlurTexelSize;
            float sampleDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0).r;

            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * depthDiff / (gDepthSigma * gDepthSigma + 0.0001));

            float weight = GAUSSIAN_WEIGHTS[i] * depthWeight;
            aoSum += sampleAO * weight;
            weightSum += weight;
        }
    }

    gSSAOOutput[pixelCoord] = aoSum / max(weightSum, 0.0001);
}

// ============================================
// Bilateral Blur - Vertical
// ============================================

[numthreads(8, 8, 1)]
void CSBlurV(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixelCoord = dispatchThreadID.xy;
    float2 uv = (float2(pixelCoord) + 0.5) * gBlurTexelSize;

    float centerDepth = gDepthInput.SampleLevel(gPointSampler, uv, 0).r;
    float centerAO = gSSAOInput.SampleLevel(gPointSampler, uv, 0).r;

    // Skip sky
    if (centerDepth >= 1.0) {
        gSSAOOutput[pixelCoord] = 1.0;
        return;
    }

    float aoSum = centerAO * GAUSSIAN_WEIGHTS[0];
    float weightSum = GAUSSIAN_WEIGHTS[0];

    // Vertical blur
    for (int i = 1; i <= gBlurRadius; i++) {
        // Positive direction
        {
            float2 sampleUV = uv + float2(0.0, float(i)) * gBlurTexelSize;
            float sampleDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0).r;

            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * depthDiff / (gDepthSigma * gDepthSigma + 0.0001));

            float weight = GAUSSIAN_WEIGHTS[i] * depthWeight;
            aoSum += sampleAO * weight;
            weightSum += weight;
        }

        // Negative direction
        {
            float2 sampleUV = uv - float2(0.0, float(i)) * gBlurTexelSize;
            float sampleDepth = gDepthInput.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0).r;

            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * depthDiff / (gDepthSigma * gDepthSigma + 0.0001));

            float weight = GAUSSIAN_WEIGHTS[i] * depthWeight;
            aoSum += sampleAO * weight;
            weightSum += weight;
        }
    }

    gSSAOOutput[pixelCoord] = aoSum / max(weightSum, 0.0001);
}

// ============================================
// Bilateral Upsample (Half-Res → Full-Res)
// ============================================

[numthreads(8, 8, 1)]
void CSBilateralUpsample(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 fullResCoord = dispatchThreadID.xy;
    float2 fullResUV = (float2(fullResCoord) + 0.5) * gFullResTexelSize;

    // Sample full-res depth
    float centerDepth = gDepthFullRes.SampleLevel(gPointSampler, fullResUV, 0).r;

    // Skip sky
    if (centerDepth >= 1.0) {
        gSSAOOutput[fullResCoord] = 1.0;
        return;
    }

    // Sample 4 nearest half-res samples
    float aoSum = 0.0;
    float weightSum = 0.0;

    // 2x2 bilinear with depth weighting
    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            float2 offset = (float2(x, y) - 0.25) * gHalfResTexelSize;
            float2 sampleUV = fullResUV + offset;

            float sampleDepth = gDepthHalfRes.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOHalfRes.SampleLevel(gLinearSampler, sampleUV, 0).r;

            // Depth-aware weight
            float depthDiff = abs(centerDepth - sampleDepth);
            float weight = exp(-depthDiff * depthDiff * gUpsampleDepthSigma * 100.0);

            aoSum += sampleAO * weight;
            weightSum += weight;
        }
    }

    gSSAOOutput[fullResCoord] = aoSum / max(weightSum, 0.0001);
}

// ============================================
// Depth Downsample (Full-Res → Half-Res)
// ============================================

[numthreads(8, 8, 1)]
void CSDownsampleDepth(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 halfResCoord = dispatchThreadID.xy;
    float2 halfResUV = (float2(halfResCoord) + 0.5) * gDownsampleTexelSize * 2.0;

    // Sample 4 full-res depth values and take closest (min depth = closest)
    float d0 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2(-0.25, -0.25) * gDownsampleTexelSize, 0).r;
    float d1 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2( 0.25, -0.25) * gDownsampleTexelSize, 0).r;
    float d2 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2(-0.25,  0.25) * gDownsampleTexelSize, 0).r;
    float d3 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2( 0.25,  0.25) * gDownsampleTexelSize, 0).r;

    // Use closest depth (preserves edges better for bilateral operations)
    float closestDepth = min(min(d0, d1), min(d2, d3));

    gSSAOOutput[halfResCoord] = closestDepth;
}
