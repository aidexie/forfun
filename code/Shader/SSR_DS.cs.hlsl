// ============================================
// SSR_DS.cs.hlsl - Screen-Space Reflections (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
//
// Reference: "Efficient GPU Screen-Space Ray Tracing"
//            Morgan McGuire & Michael Mara (2014)
//
// Modes (ordered simple to complex):
//   0 - SimpleLinear: Basic linear ray march (no Hi-Z, educational)
//   1 - HiZ Trace: Single ray with Hi-Z acceleration (default)
//   2 - Stochastic: Multiple rays with GGX importance sampling
//   3 - Temporal: Stochastic + temporal accumulation
//
// Optimized for reversed-Z (near=1.0, far=0.0)
// ============================================

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_SSR : register(b0, space1)
{
    float4x4 g_Proj;           // Projection matrix
    float4x4 g_InvProj;        // Inverse projection matrix
    float4x4 g_View;           // View matrix (world to view)
    float4x4 g_InvView;        // Inverse view matrix
    float4x4 g_PrevViewProj;   // Previous frame view-projection (temporal)
    float2 g_ScreenSize;       // Full resolution
    float2 g_TexelSize;        // 1.0 / screenSize
    float g_MaxDistance;       // Maximum ray distance
    float g_Thickness;         // Surface thickness for hit
    float g_Stride;            // Ray march stride
    float g_StrideZCutoff;     // View-Z stride scaling cutoff (reserved)
    int g_MaxSteps;            // Maximum ray march steps
    int g_BinarySearchSteps;   // Binary search refinement (reserved)
    float g_JitterOffset;      // Temporal jitter
    float g_FadeStart;         // Edge fade start (reserved)
    float g_FadeEnd;           // Edge fade end (reserved)
    float g_RoughnessFade;     // Roughness cutoff
    float g_NearZ;             // Camera near plane
    float g_FarZ;              // Camera far plane
    int g_HiZMipCount;         // Number of Hi-Z mip levels
    uint g_UseReversedZ;       // 0 = standard-Z, 1 = reversed-Z (always 1)
    int g_SSRMode;             // 0=SimpleLinear, 1=HiZ, 2=Stochastic, 3=Temporal
    int g_NumRays;             // Rays per pixel (stochastic/temporal)
    float g_BrdfBias;          // BRDF importance sampling bias (0=uniform, 1=full GGX)
    float g_TemporalBlend;     // History blend factor
    float g_MotionThreshold;   // Motion rejection threshold
    uint g_FrameIndex;         // Frame counter for temporal jitter
    uint g_UseAdaptiveRays;    // Enable adaptive ray count based on roughness
    float g_FireflyClampThreshold;  // Absolute luminance clamp (e.g., 10.0)
    float g_FireflyMultiplier;      // Multiplier for adaptive threshold (e.g., 4.0)
    float g_Pad;
};

// Texture SRVs (t0-t7, space1)
Texture2D<float> g_DepthBuffer : register(t0, space1);      // Full-res depth
Texture2D<float4> g_NormalBuffer : register(t1, space1);    // Normal.xyz + Roughness
Texture2D<float> g_HiZPyramid : register(t2, space1);       // Hi-Z depth pyramid
Texture2D<float4> g_SceneColor : register(t3, space1);      // HDR scene color
Texture2D<float4> g_BlueNoise : register(t4, space1);       // Blue noise for stochastic jitter
Texture2D<float4> g_SSRHistory : register(t5, space1);      // SSR history (temporal mode)

// UAVs (u0-u3, space1)
RWTexture2D<float4> g_SSROutput : register(u0, space1);     // Output: reflection color + confidence

// Samplers (s0-s3, space1)
SamplerState g_PointSampler : register(s0, space1);
SamplerState g_LinearSampler : register(s1, space1);

// ============================================
// Constants
// ============================================
static const float PI = 3.14159265359;

// ============================================
// Random & Sampling Functions
// ============================================

uint WangHash(uint seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

float4 GetBlueNoise(uint2 pixelCoord)
{
    uint2 noiseCoord = (pixelCoord + uint2(g_FrameIndex * 13, g_FrameIndex * 7)) % 64;
    return g_BlueNoise.Load(int3(noiseCoord, 0));
}

float3 SampleGGX(float2 Xi, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a2 - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    return H;
}

void BuildOrthonormalBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// ============================================
// PDF and Weighting Helpers
// ============================================

float D_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0001);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 ClampLuminance(float3 color, float maxLuminance)
{
    float lum = Luminance(color);
    if (lum > maxLuminance)
    {
        return color * (maxLuminance / lum);
    }
    return color;
}

int GetAdaptiveRayCount(float roughness, int maxRays)
{
    if (roughness < 0.05)
        return 1;

    float t = saturate((roughness - 0.05) / 0.25);
    return max(1, int(lerp(1.0, float(maxRays), t)));
}

float3 TangentToWorld(float3 H, float3 N)
{
    float3 T, B;
    BuildOrthonormalBasis(N, T, B);
    return normalize(T * H.x + B * H.y + N * H.z);
}

// ============================================
// Helper Functions
// ============================================

float ComputeRoughnessMask(float roughness)
{
    float mask = 1.0 - saturate(roughness / g_RoughnessFade);
    return mask * mask;
}

float3 ScreenToView(float2 uv, float depth)
{
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    ndc.y = -ndc.y;

    float4 viewPos = mul(ndc, g_InvProj);
    return viewPos.xyz / viewPos.w;
}

float3 ViewToScreen(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos, 1.0), g_Proj);
    clipPos.xyz /= clipPos.w;

    float2 uv = clipPos.xy * float2(0.5, -0.5) + 0.5;
    return float3(uv, clipPos.z);
}

float SampleHiZ(float2 uv, int mipLevel)
{
    return g_HiZPyramid.SampleLevel(g_PointSampler, uv, mipLevel);
}

bool IsValidUV(float2 uv)
{
    return all(uv >= 0.001) && all(uv <= 0.999);
}

// ============================================
// Hi-Z Accelerated Ray March
// ============================================
float4 HiZTrace(float3 rayOrigin, float3 rayDir, float jitter)
{
    float3 rayEnd = rayOrigin + rayDir * g_MaxDistance;
    float3 ssStart = ViewToScreen(rayOrigin);
    float3 ssEnd = ViewToScreen(rayEnd);

    if (!IsValidUV(ssEnd.xy) && !IsValidUV(ssStart.xy))
        return float4(0, 0, 0, 0);

    float3 ssRay = ssEnd - ssStart;
    float ssRayLengthPixels = length(ssRay.xy * g_ScreenSize);
    if (ssRayLengthPixels < 0.001)
        return float4(0, 0, 0, 0);

    float3 ssDir = ssRay / ssRayLengthPixels;
    float3 ssPos = ssStart + ssDir * g_Stride * (1.0 + jitter);
    float thicknessThreshold = g_Thickness * 0.01;

    int mipLevel = 0;
    int maxMip = max(0, g_HiZMipCount - 2);

    float3 hitResult = float3(0, 0, 0);
    bool hit = false;
    int stepsInFront = 0;

    [loop]
    for (int i = 0; i < g_MaxSteps && !hit; ++i)
    {
        if (!IsValidUV(ssPos.xy))
            break;

        float cellSizePixels = exp2((float)mipLevel);
        float cellSizeUV = cellSizePixels * g_TexelSize.x;
        float sceneDepth = SampleHiZ(ssPos.xy, mipLevel);
        bool behind = ssPos.z <= sceneDepth;

        if (behind)
        {
            stepsInFront = 0;

            if (mipLevel == 0)
            {
                float depthDiff = abs(ssPos.z - sceneDepth);
                if (depthDiff < thicknessThreshold)
                {
                    hitResult = ssPos;
                    hit = true;
                }
                else
                {
                    ssPos += ssDir * cellSizePixels;
                }
            }
            else
            {
                mipLevel--;
            }
        }
        else
        {
            float2 cellIndex = floor(ssPos.xy / cellSizeUV);
            float2 cellMin = cellIndex * cellSizeUV;
            float2 cellMax = cellMin + cellSizeUV;

            float2 tBoundary;
            tBoundary.x = (ssDir.x > 0) ? (cellMax.x - ssPos.x) : (ssPos.x - cellMin.x);
            tBoundary.y = (ssDir.y > 0) ? (cellMax.y - ssPos.y) : (ssPos.y - cellMin.y);

            float2 tSteps;
            tSteps.x = (abs(ssDir.x) > 1e-7) ? tBoundary.x / abs(ssDir.x) : 1e10;
            tSteps.y = (abs(ssDir.y) > 1e-7) ? tBoundary.y / abs(ssDir.y) : 1e10;

            float stepPixels = min(tSteps.x, tSteps.y) + 0.5;
            stepPixels = max(stepPixels, 1.0);

            ssPos += ssDir * stepPixels;
            stepsInFront++;

            if (stepsInFront > 2 && mipLevel < maxMip)
            {
                mipLevel++;
                stepsInFront = 0;
            }
        }
    }

    float confidence = hit ? 1.0 : 0.0;

    if (hit)
    {
        float2 edgeFade = 1.0 - pow(abs(hitResult.xy * 2.0 - 1.0), 8.0);
        confidence *= min(edgeFade.x, edgeFade.y);

        float3 hitView = ScreenToView(hitResult.xy, hitResult.z);
        float hitDist = length(hitView - rayOrigin);
        float distFade = 1.0 - saturate(hitDist / g_MaxDistance);
        confidence *= distFade;

        float3 hitNormal = g_NormalBuffer.SampleLevel(g_PointSampler, hitResult.xy, 0).xyz * 2.0 - 1.0;
        float backfaceFade = saturate(dot(-rayDir, hitNormal) + 0.1);
        confidence *= backfaceFade;
    }

    return float4(hitResult.xy, hitResult.z, confidence);
}

// ============================================
// Simple Linear Ray March (No Hi-Z)
// ============================================
float4 SimpleLinearTrace(float3 rayOrigin, float3 rayDir)
{
    float3 rayEnd = rayOrigin + rayDir * g_MaxDistance;
    float3 ssStart = ViewToScreen(rayOrigin);
    float3 ssEnd = ViewToScreen(rayEnd);

    if (!IsValidUV(ssEnd.xy) && !IsValidUV(ssStart.xy))
        return float4(0, 0, 0, 0);

    float2 rayDirUV = ssEnd.xy - ssStart.xy;
    float rayLength = length(rayDirUV * g_ScreenSize);
    if (rayLength < 0.001)
        return float4(0, 0, 0, 0);

    float2 rayDirPixels = normalize(rayDirUV * g_ScreenSize);
    float2 rayStepUV = rayDirPixels * g_TexelSize * g_Stride;
    float depthPerStep = (ssEnd.z - ssStart.z) / rayLength * g_Stride;

    float2 currentUV = ssStart.xy;
    float currentDepth = ssStart.z;
    float thicknessThreshold = g_Thickness * 0.01;

    bool hit = false;
    float3 hitResult = float3(0, 0, 0);

    [loop]
    for (int i = 0; i < g_MaxSteps && !hit; ++i)
    {
        currentUV += rayStepUV;
        currentDepth += depthPerStep;

        if (!IsValidUV(currentUV))
            break;

        float sceneDepth = g_HiZPyramid.SampleLevel(g_PointSampler, currentUV, 0);

        if (currentDepth <= sceneDepth)
        {
            if (abs(currentDepth - sceneDepth) < thicknessThreshold)
            {
                hitResult = float3(currentUV, currentDepth);
                hit = true;
            }
            break;
        }
    }

    float confidence = hit ? 1.0 : 0.0;

    if (hit)
    {
        float2 edgeFade = 1.0 - pow(abs(hitResult.xy * 2.0 - 1.0), 8.0);
        confidence *= min(edgeFade.x, edgeFade.y);

        float3 hitView = ScreenToView(hitResult.xy, hitResult.z);
        float hitDist = length(hitView - rayOrigin);
        float distFade = 1.0 - saturate(hitDist / g_MaxDistance);
        confidence *= distFade;
    }

    return float4(hitResult.xyz, confidence);
}

// ============================================
// Single-Ray SSR Helper
// ============================================
float4 TraceSingleRay(float3 viewPos, float3 reflectDir, float roughness, bool useHiZ, float jitter)
{
    float4 hitResult = useHiZ ? HiZTrace(viewPos, reflectDir, jitter) : SimpleLinearTrace(viewPos, reflectDir);

    float3 reflectionColor = float3(0, 0, 0);
    if (hitResult.w > 0.001)
    {
        reflectionColor = g_SceneColor.SampleLevel(g_LinearSampler, hitResult.xy, 0).rgb;
    }

    float confidence = hitResult.w * ComputeRoughnessMask(roughness);

    return float4(reflectionColor, confidence);
}

// ============================================
// Stochastic SSR
// ============================================
float4 StochasticSSR(float3 viewPos, float3 normalVS, float3 viewDir, float roughness, uint2 pixelCoord)
{
    float4 noise = GetBlueNoise(pixelCoord);

    int baseRays = min(g_NumRays, 8);
    int numRays = g_UseAdaptiveRays ? GetAdaptiveRayCount(roughness, baseRays) : baseRays;

    if (roughness < 0.01)
    {
        float3 reflectDir = reflect(viewDir, normalVS);
        if (reflectDir.z < 0.0)
            return float4(0, 0, 0, 0);

        float jitter = 1.0 + noise.z * 0.5;
        float4 hitResult = HiZTrace(viewPos, reflectDir, jitter);

        if (hitResult.w > 0.001)
        {
            float3 color = g_SceneColor.SampleLevel(g_LinearSampler, hitResult.xy, 0).rgb;
            color = ClampLuminance(color, g_FireflyClampThreshold);
            return float4(color, hitResult.w);
        }
        return float4(0, 0, 0, 0);
    }

    float3 totalColor = float3(0, 0, 0);
    float totalWeight = 0.0;
    int validSamples = 0;

    float effectiveRoughness = lerp(1.0, roughness, g_BrdfBias);
    float totalLuminance = 0.0;

    [loop]
    for (int ray = 0; ray < numRays; ++ray)
    {
        float2 Xi;
        if (ray == 0)
        {
            Xi = noise.xy;
        }
        else
        {
            const float2 R2 = float2(0.7548776662466927, 0.5698402909980532);
            Xi.x = frac(noise.x + ray * R2.x);
            Xi.y = frac(noise.y + ray * R2.y);
        }

        float3 H_tangent = SampleGGX(Xi, effectiveRoughness);
        float3 H = TangentToWorld(H_tangent, normalVS);
        float3 reflectDir = reflect(viewDir, H);

        float NdotL = dot(normalVS, reflectDir);
        if (NdotL < 0.001 || reflectDir.z < 0.0)
            continue;

        float NdotH = saturate(dot(normalVS, H));
        float VdotH = saturate(dot(-viewDir, H));

        float D = D_GGX(NdotH, effectiveRoughness);
        float pdfWeight = (D > 0.001 && NdotH > 0.001) ?
                          (4.0 * VdotH / (D * NdotH)) : 1.0;
        pdfWeight = clamp(pdfWeight, 0.0, 4.0);

        float jitter = 1.0 + noise.z * 0.5;
        float4 hitResult = HiZTrace(viewPos, reflectDir, jitter);

        if (hitResult.w > 0.001)
        {
            float3 reflectionColor = g_SceneColor.SampleLevel(g_LinearSampler, hitResult.xy, 0).rgb;

            float lum = Luminance(reflectionColor);
            totalLuminance += lum;
            validSamples++;

            float weight = hitResult.w * pdfWeight * NdotL;

            totalColor += reflectionColor * weight;
            totalWeight += weight;
        }
    }

    if (validSamples == 0 || totalWeight < 0.001)
        return float4(0, 0, 0, 0);

    float3 avgColor = totalColor / totalWeight;

    float avgLuminance = totalLuminance / float(validSamples);
    float adaptiveThreshold = max(g_FireflyClampThreshold, avgLuminance * g_FireflyMultiplier);
    avgColor = ClampLuminance(avgColor, adaptiveThreshold);

    float sampleRatio = float(validSamples) / float(numRays);
    float avgConfidence = saturate(totalWeight / float(validSamples)) * sampleRatio;

    return float4(avgColor, avgConfidence);
}

// ============================================
// Temporal SSR
// ============================================
float4 TemporalSSR(float3 viewPos, float3 worldPos, float3 normalVS, float3 viewDir, float roughness, uint2 pixelCoord, float2 uv)
{
    float4 currentSSR = StochasticSSR(viewPos, normalVS, viewDir, roughness, pixelCoord);

    float4 worldPos4 = float4(worldPos, 1.0);
    float4 prevClip = mul(worldPos4, g_PrevViewProj);
    float2 prevUV = prevClip.xy / prevClip.w * float2(0.5, -0.5) + 0.5;

    if (!IsValidUV(prevUV))
    {
        return currentSSR;
    }

    float4 historySSR = g_SSRHistory.SampleLevel(g_LinearSampler, prevUV, 0);

    float2 motion = abs(uv - prevUV);
    float motionMagnitude = length(motion * g_ScreenSize);
    float motionRejection = 1.0 - saturate(motionMagnitude / (g_MotionThreshold * g_ScreenSize.x));

    float3 m1 = currentSSR.rgb;
    float3 m2 = currentSSR.rgb * currentSSR.rgb;

    float3 neighborMin = currentSSR.rgb;
    float3 neighborMax = currentSSR.rgb;

    float3 clampedHistory = clamp(historySSR.rgb, neighborMin * 0.8, neighborMax * 1.2 + 0.01);

    float blendFactor = g_TemporalBlend * motionRejection * historySSR.a;
    float3 blendedColor = lerp(currentSSR.rgb, clampedHistory, blendFactor);
    float blendedConfidence = lerp(currentSSR.a, historySSR.a, blendFactor * 0.5);

    return float4(blendedColor, blendedConfidence);
}

// ============================================
// Main SSR Compute Shader
// ============================================
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)g_ScreenSize.x || DTid.y >= (uint)g_ScreenSize.y)
    {
        return;
    }

    float2 uv = (DTid.xy + 0.5) * g_TexelSize;

    float4 normalRoughness = g_NormalBuffer.SampleLevel(g_PointSampler, uv, 0);
    float roughness = normalRoughness.w;

    if (roughness > g_RoughnessFade)
    {
        g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float depth = g_DepthBuffer.SampleLevel(g_PointSampler, uv, 0);

    if (depth < 0.0001)
    {
        g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float3 normalWS = normalRoughness.xyz;
    float3 viewPos = ScreenToView(uv, depth);
    float3 normalVS = normalize(mul(float4(normalWS, 0.0), g_View).xyz);
    float3 viewDir = normalize(viewPos);

    float4 result;

    if (g_SSRMode == 0 || g_SSRMode == 1)
    {
        float3 reflectDir = reflect(viewDir, normalVS);

        if (reflectDir.z < 0.0)
        {
            g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
            return;
        }

        float jitter = 1.0 + g_JitterOffset;
        bool useHiZ = (g_SSRMode == 1);

        result = TraceSingleRay(viewPos, reflectDir, roughness, useHiZ, jitter);
    }
    else if (g_SSRMode == 2)
    {
        result = StochasticSSR(viewPos, normalVS, viewDir, roughness, DTid.xy);
        result.a *= ComputeRoughnessMask(roughness);
    }
    else
    {
        float3 worldPos = mul(float4(viewPos, 1.0), g_InvView).xyz;
        result = TemporalSSR(viewPos, worldPos, normalVS, viewDir, roughness, DTid.xy, uv);
        result.a *= ComputeRoughnessMask(roughness);
    }

    g_SSROutput[DTid.xy] = result;
}
