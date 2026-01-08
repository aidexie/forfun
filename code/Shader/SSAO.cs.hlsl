// ============================================
// SSAO.cs.hlsl - Screen-Space Ambient Occlusion
// ============================================
// Implements three algorithms (selectable via gAlgorithm):
//   0: GTAO   - Ground Truth AO (UE5, Unity HDRP) - Most accurate
//   1: HBAO   - Horizon-Based AO (NVIDIA) - Good balance
//   2: Crytek - Original SSAO (Crysis 2007) - Classic hemisphere sampling
//
// References:
//   GTAO:   "Practical Real-Time Strategies for Accurate Indirect Occlusion"
//           Jorge Jimenez et al. (2016)
//   HBAO:   "Image-Space Horizon-Based Ambient Occlusion"
//           Louis Bavoil, Miguel Sainz (NVIDIA, 2008)
//   Crytek: "Finding Next Gen - CryEngine 2" (GDC 2007)
//           Martin Mittring
//
// Entry Points:
//   CSMain              - SSAO compute at half-res (algorithm selected by gAlgorithm)
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

// Debug visualization modes (set via gAlgorithm when >= 100)
// 100 = Raw depth value
// 101 = Linearized depth (view-space Z)
// 102 = View-space position.z normalized
// 103 = View-space normal.z (facing camera = white)
// 104 = Reconstructed position difference for first sample
#define SSAO_DEBUG_RAW_DEPTH       100
#define SSAO_DEBUG_LINEAR_DEPTH    101
#define SSAO_DEBUG_VIEW_POS_Z      102
#define SSAO_DEBUG_VIEW_NORMAL_Z   103
#define SSAO_DEBUG_SAMPLE_DIFF     104

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
    int gAlgorithm;             // 0=GTAO, 1=HBAO, 2=Crytek
    uint gUseReversedZ;         // 0 = standard-Z, 1 = reversed-Z
    float3 _pad;                // Padding to 16-byte alignment
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
    uint gDownsampleUseReversedZ;   // 0 = standard-Z, 1 = reversed-Z
    float _downsamplePad;
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
    // IMPORTANT: Use mul(vector, matrix) for row-major transposed matrices
    float3 viewNormal = mul(float4(worldNormal, 0), gView).xyz;
    return normalize(viewNormal);
}

// Distance-based falloff
float ComputeFalloff(float dist, float radius) {
    float t = saturate((dist - gFalloffStart * radius) / ((gFalloffEnd - gFalloffStart) * radius));
    return 1.0 - t * t;
}

// Check if depth value represents sky (far plane)
// Standard-Z: sky at 1.0, Reversed-Z: sky at 0.0
bool IsSkyDepth(float depth) {
    return gUseReversedZ ? (depth <= 0.001) : (depth >= 0.999);
}

// Get closest depth from multiple samples
// Standard-Z: min = closest, Reversed-Z: max = closest
float GetClosestDepth(float d0, float d1, float d2, float d3) {
    if (gUseReversedZ) {
        return max(max(d0, d1), max(d2, d3));
    }
    return min(min(d0, d1), min(d2, d3));
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

    // UE-style: World radius → UV space
    // gProj[1][1] = cot(FOV/2), maps world radius to NDC, then /2 to UV
    float screenRadius = gRadius * gProj[1][1] * 0.5 / max(abs(viewPos.z), 0.1);

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
        float n = atan2(dot(projNormal, tangent), projNormal.z);

        // Initialize horizons to minimum (looking straight down)
        float h1 = -HALF_PI;  // Negative direction
        float h2 = -HALF_PI;  // Positive direction

        // March in negative direction
        for (int s = 1; s <= gNumSteps; s++) {
            // screenRadius is in UV space, so no gTexelSize needed
            float2 sampleUV = uv - sliceDir * float(s) * (screenRadius / float(gNumSteps));

            // Bounds check
            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (IsSkyDepth(sampleDepth)) continue;  // Skip sky

            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist > 0.001 && dist < gRadius) {
                float falloff = ComputeFalloff(dist, gRadius);
                float3 sampleDir = diff / dist;

                // Horizon angle: angle between sample direction and view direction (Z)
                float horizonAngle = atan2(dot(sampleDir, tangent), sampleDir.z);

                // Blend with falloff
                h1 = max(h1, lerp(-HALF_PI, horizonAngle, falloff));
            }
        }

        // March in positive direction
        for (int s2 = 1; s2 <= gNumSteps; s2++) {
            // screenRadius is in UV space, so no gTexelSize needed
            float2 sampleUV = uv + sliceDir * float(s2) * (screenRadius / float(gNumSteps));

            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (IsSkyDepth(sampleDepth)) continue;

            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist > 0.001 && dist < gRadius) {
                float falloff = ComputeFalloff(dist, gRadius);
                float3 sampleDir = diff / dist;

                float horizonAngle = atan2(dot(sampleDir, tangent), sampleDir.z);
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
// HBAO - Horizon-Based Ambient Occlusion (NVIDIA 2008)
// ============================================
// Marches along directions in screen space, finding the horizon angle
// at each step. Uses sin(horizon) approximation for AO contribution.
// Simpler than GTAO but still physically motivated.

float ComputeHBAO(float3 viewPos, float3 viewNormal, float2 uv, float2 noiseVec) {
    float totalAO = 0.0;

    // Screen-space radius
    float screenRadius = gRadius * gProj[1][1] * 0.5 / max(abs(viewPos.z), 0.1);

    // Direction step (evenly distributed around hemisphere)
    float dirStep = 2.0 * PI / float(gNumSlices);

    for (int dir = 0; dir < gNumSlices; dir++) {
        // Direction angle with noise rotation
        float angle = float(dir) * dirStep;
        float2 direction;
        direction.x = cos(angle) * noiseVec.x - sin(angle) * noiseVec.y;
        direction.y = sin(angle) * noiseVec.x + cos(angle) * noiseVec.y;

        // Ray march in this direction to find horizon
        float maxHorizonCos = -1.0;  // cos(horizon angle), start at -1 (horizon at -90 deg)

        for (int step = 1; step <= gNumSteps; step++) {
            // Sample position along ray
            float t = float(step) / float(gNumSteps);
            float2 sampleUV = uv + direction * screenRadius * t;

            // Bounds check
            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            // Sample depth
            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (IsSkyDepth(sampleDepth)) continue;

            // Reconstruct view position
            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist < 0.001 || dist > gRadius) continue;

            // Compute horizon angle
            // Horizon = angle between view direction and direction to sample
            float3 horizonDir = diff / dist;

            // Cosine of angle between normal and horizon direction
            // Higher value = horizon is more "up" relative to normal = less occlusion
            float horizonCos = dot(viewNormal, horizonDir);

            // Apply distance falloff
            float falloff = ComputeFalloff(dist, gRadius);
            horizonCos = lerp(-1.0, horizonCos, falloff);

            // Track maximum horizon (highest point we can see)
            maxHorizonCos = max(maxHorizonCos, horizonCos);
        }

        // HBAO uses sin(horizon) as AO approximation
        // sin(acos(x)) = sqrt(1 - x^2)
        // When horizon is at normal level (cos=0), sin=1, full visibility
        // When horizon is above normal (cos>0), sin<1, some occlusion from above
        // When horizon is below normal (cos<0), sin approaches 1, full visibility in that direction

        // Clamp horizon to hemisphere (can't occlude from behind surface)
        maxHorizonCos = max(maxHorizonCos, 0.0);

        // AO contribution: 1 - sin(horizon) = 1 - sqrt(1 - cos^2)
        // But we want visibility, so: sin(horizon)
        float horizonSin = sqrt(1.0 - maxHorizonCos * maxHorizonCos);
        totalAO += horizonSin;
    }

    // Average over all directions
    totalAO /= float(gNumSlices);

    // Apply intensity and convert to final AO
    float ao = saturate(pow(totalAO, gIntensity));

    return ao;
}

// ============================================
// Crytek SSAO - Original Algorithm (Crysis 2007)
// ============================================
// Classic hemisphere sampling with random kernel
// Samples random points in a hemisphere around the surface normal
// and checks depth to estimate occlusion.

// Pre-computed hemisphere sample kernel (16 samples)
// Distributed using cosine-weighted hemisphere sampling
static const float3 SSAO_KERNEL[16] = {
    float3( 0.5381, 0.1856,-0.4319),
    float3( 0.1379, 0.2486, 0.4430),
    float3( 0.3371, 0.5679,-0.0057),
    float3(-0.6999,-0.0451,-0.0019),
    float3( 0.0689,-0.1598,-0.8547),
    float3( 0.0560, 0.0069,-0.1843),
    float3(-0.0146, 0.1402, 0.0762),
    float3( 0.0100,-0.1924,-0.0344),
    float3(-0.3577,-0.5301,-0.4358),
    float3(-0.3169, 0.1063, 0.0158),
    float3( 0.0103,-0.5869, 0.0046),
    float3(-0.0897,-0.4940, 0.3287),
    float3( 0.7119,-0.0154,-0.0918),
    float3(-0.0533, 0.0596,-0.5411),
    float3( 0.0352,-0.0631, 0.5460),
    float3(-0.4776, 0.2847,-0.0271)
};

float ComputeCrytekSSAO(float3 viewPos, float3 viewNormal, float2 uv, float2 noiseVec) {
    float occlusion = 0.0;

    // Build TBN matrix for orienting samples along normal
    // Use Gram-Schmidt to create orthonormal basis from normal and noise
    float3 randomVec = float3(noiseVec.x, noiseVec.y, 0.0);

    // Create tangent perpendicular to normal
    float3 tangent = normalize(randomVec - viewNormal * dot(randomVec, viewNormal));

    // Handle degenerate case when randomVec is parallel to normal
    if (length(tangent) < 0.001) {
        tangent = normalize(float3(1, 0, 0) - viewNormal * viewNormal.x);
    }

    float3 bitangent = cross(viewNormal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, viewNormal);

    // Number of samples
    int numSamples = min(gNumSlices * gNumSteps, 16);

    for (int i = 0; i < numSamples; i++) {
        // Get sample direction from kernel and orient to hemisphere around normal
        float3 sampleDir = mul(SSAO_KERNEL[i], TBN);

        // Flip sample if it's below the surface (ensure hemisphere sampling)
        if (dot(sampleDir, viewNormal) < 0.0) {
            sampleDir = -sampleDir;
        }

        // Scale sample position by radius (varying scale for better distribution)
        float scale = float(i + 1) / float(numSamples);
        scale = lerp(0.1, 1.0, scale * scale);

        // Sample position in view space
        // float3 samplePos = viewPos + sampleDir * gRadius * scale;
        float3 samplePos = viewPos + float3(1,0,0) * gRadius * scale;

        // Project sample to screen space
        float4 sampleClip = mul(float4(samplePos, 1.0), gProj);
        float2 sampleUV = sampleClip.xy / sampleClip.w;
        // return sampleClip.z/sampleClip.w*10.0;
        sampleUV = sampleUV * float2(0.5, -0.5) + 0.5;

        // Bounds check
        if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) continue;

        // Sample depth at this position
        float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
        // return sampleDepth*10;
        // if (IsSkyDepth(sampleDepth)) continue;  // Skip sky

        // Reconstruct actual position at sampled depth
        float3 actualPos = ReconstructViewPos(sampleUV, sampleDepth);

        // Distance from view position to actual surface
        float3 diff = actualPos - viewPos;
        float dist = length(diff);
        return dist;
        // return actualPos.z/100.0;
        // return sampleClip.z/sampleClip.w;
        // return dot(normalize(diff),viewNormal);
        // Range check: smooth falloff at radius boundary
        float rangeCheck = 1.0 - smoothstep(gRadius * 0.5, gRadius, dist);

        // Occlusion check:
        // In LEFT-HANDED view space with camera looking at +Z:
        // Z values are POSITIVE (farther = larger Z)
        // samplePos.z = expected depth if nothing blocks
        // actualPos.z = actual surface depth at that screen position
        // If actualPos.z < samplePos.z, actual surface is CLOSER = occludes the sample
        float depthDiff = samplePos.z - actualPos.z;  // positive if actualPos is closer (occludes)

        // Only count as occlusion if:
        // 1. The actual surface is closer than our sample point (depthDiff > 0)
        // 2. The depth difference is within reasonable bounds (not a backface or far surface)
        if (depthDiff > 0.001 && depthDiff < gRadius) 
        {
            occlusion += rangeCheck;
        }
    }

    // Normalize and apply intensity
    occlusion = occlusion / float(numSamples);
    float ao = saturate(1.0 - occlusion);

    return ao;
}

// ============================================
// Main Compute Shader - Unified Entry Point
// ============================================

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixelCoord = dispatchThreadID.xy;
    float2 uv = (float2(pixelCoord) + 0.5) * gTexelSize;

    // Sample depth
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;

    // Skip sky pixels
    if (IsSkyDepth(depth)) {
        gSSAOOutput[pixelCoord] = 1.0;
        return;
    }

    // Reconstruct view-space position and normal
    float3 viewPos = ReconstructViewPos(uv, depth);
    float3 viewNormal = GetViewNormal(uv);

    // ============================================
    // Debug Visualization Modes
    // ============================================
    if (gAlgorithm >= 100) 
    {
        float debugValue = 0.0;

        // if (gAlgorithm == SSAO_DEBUG_RAW_DEPTH) {
        //     // Raw depth buffer value [0, 1]
        //     // Near plane should be dark, far plane should be white
        //     debugValue = depth;
        // }
        // else if (gAlgorithm == SSAO_DEBUG_LINEAR_DEPTH) {
        //     // View-space Z (linear depth)
        //     // Normalize to visible range (assume max 100 units)
        //     debugValue = saturate(abs(viewPos.z) / 100.0);
        // }
        // else if (gAlgorithm == SSAO_DEBUG_VIEW_POS_Z) {
        //     // View-space Z sign check
        //     // If LEFT-HANDED: Z should be POSITIVE (into screen)
        //     // If RIGHT-HANDED: Z should be NEGATIVE
        //     // Output: positive Z = green tint, negative Z = red tint
        //     // For grayscale: output normalized absolute Z
        //     debugValue = viewPos.z > 0.0 ? saturate(viewPos.z / 50.0) : 0.0;
        // }
        // else if (gAlgorithm == SSAO_DEBUG_VIEW_NORMAL_Z) {
        //     // View-space normal Z component
        //     // Surfaces facing camera should have negative Z (toward camera)
        //     // Output: facing camera = white, facing away = black
        //     debugValue = saturate(-viewNormal.z);
        // }
        // else if (gAlgorithm == SSAO_DEBUG_SAMPLE_DIFF) 
        {
            // Test sample reconstruction accuracy
            // Sample a nearby pixel and compare reconstructed positions
            float2 offsetUV = uv + float2(gTexelSize.y * 5.0, 0.0);
            // float2 offsetUV = uv;
            if (offsetUV.x < 1.0) {
                float sampleDepth = gDepth.SampleLevel(gPointSampler, offsetUV, 0).r;
                if (sampleDepth < 1.0) {
                    float3 samplePos = ReconstructViewPos(offsetUV, sampleDepth);
                    // debugValue = saturate(samplePos.z / 50.0);
                    // debugValue = sampleDepth*10;
                    float3 diff = samplePos - viewPos;
                    // // Show the XY distance (should be small for nearby pixels on same surface)
                    debugValue = saturate(length(diff.xy) / gRadius);
                }
            }
        }

        gSSAOOutput[pixelCoord] = debugValue;
        return;
    }

    // ============================================
    // Normal SSAO Algorithm Selection
    // ============================================

    // Sample noise for random rotation (tiles across screen)
    float2 noiseUV = uv * gNoiseScale;
    float2 noise = gNoise.SampleLevel(gPointSampler, noiseUV, 0).rg;
    float2 noiseVec = normalize(noise * 2.0 - 1.0);

    // Select algorithm based on gAlgorithm
    float ao = 1.0;
    if (gAlgorithm == SSAO_ALGORITHM_GTAO) {
        ao = ComputeGTAO(viewPos, viewNormal, uv, noiseVec);
    }
    else if (gAlgorithm == SSAO_ALGORITHM_HBAO) {
        ao = ComputeHBAO(viewPos, viewNormal, uv, noiseVec);
    }
    else if (gAlgorithm == SSAO_ALGORITHM_CRYTEK) {
        ao = ComputeCrytekSSAO(viewPos, viewNormal, uv, noiseVec);
    }

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

    // Skip sky (works for both standard-Z and reversed-Z)
    if (centerDepth >= 0.999 || centerDepth <= 0.001) {
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

    // Skip sky (works for both standard-Z and reversed-Z)
    if (centerDepth >= 0.999 || centerDepth <= 0.001) {
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

    // Skip sky (works for both standard-Z and reversed-Z)
    if (centerDepth >= 0.999 || centerDepth <= 0.001) {
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

    // Sample 4 full-res depth values
    float d0 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2(-0.25, -0.25) * gDownsampleTexelSize, 0).r;
    float d1 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2( 0.25, -0.25) * gDownsampleTexelSize, 0).r;
    float d2 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2(-0.25,  0.25) * gDownsampleTexelSize, 0).r;
    float d3 = gDepthSource.SampleLevel(gPointSampler, halfResUV + float2( 0.25,  0.25) * gDownsampleTexelSize, 0).r;

    // Use closest depth (preserves edges better for bilateral operations)
    // Standard-Z: min = closest, Reversed-Z: max = closest
    float closestDepth;
    if (gDownsampleUseReversedZ) {
        closestDepth = max(max(d0, d1), max(d2, d3));
    } else {
        closestDepth = min(min(d0, d1), min(d2, d3));
    }

    gSSAOOutput[halfResCoord] = closestDepth;
}
