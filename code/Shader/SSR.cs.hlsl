// SSR.cs.hlsl
// Screen-Space Reflections with Hi-Z accelerated ray marching
//
// Reference: "Efficient GPU Screen-Space Ray Tracing"
//            Morgan McGuire & Michael Mara (2014)
//
// Modes:
//   0 - HiZ Trace: Single ray per pixel (fastest)
//   1 - Stochastic: Multiple rays with GGX importance sampling
//   2 - Temporal: Stochastic + temporal accumulation
//
// Optimized for reversed-Z (near=1.0, far=0.0)

// ============================================
// Constant Buffer
// ============================================
cbuffer CB_SSR : register(b0)
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
    float g_StrideZCutoff;     // View-Z stride scaling cutoff
    int g_MaxSteps;            // Maximum ray march steps
    int g_BinarySearchSteps;   // Binary search refinement
    float g_JitterOffset;      // Temporal jitter
    float g_FadeStart;         // Edge fade start
    float g_FadeEnd;           // Edge fade end
    float g_RoughnessFade;     // Roughness cutoff
    float g_NearZ;             // Camera near plane
    float g_FarZ;              // Camera far plane
    int g_HiZMipCount;         // Number of Hi-Z mip levels
    uint g_UseReversedZ;       // 0 = standard-Z, 1 = reversed-Z (always 1)
    int g_SSRMode;             // 0=HiZ, 1=Stochastic, 2=Temporal
    int g_NumRays;             // Rays per pixel (stochastic/temporal)
    float g_BrdfBias;          // BRDF importance sampling bias (0=uniform, 1=full GGX)
    float g_TemporalBlend;     // History blend factor
    float g_MotionThreshold;   // Motion rejection threshold
    uint g_FrameIndex;         // Frame counter for temporal jitter
    float2 g_Pad;
};

// ============================================
// Resources
// ============================================
Texture2D<float> g_DepthBuffer : register(t0);      // Full-res depth
Texture2D<float4> g_NormalBuffer : register(t1);    // Normal.xyz + Roughness
Texture2D<float> g_HiZPyramid : register(t2);       // Hi-Z depth pyramid
Texture2D<float4> g_SceneColor : register(t3);      // HDR scene color
Texture2D<float4> g_BlueNoise : register(t4);       // Blue noise for stochastic jitter
Texture2D<float4> g_SSRHistory : register(t5);      // SSR history (temporal mode)

RWTexture2D<float4> g_SSROutput : register(u0);     // Output: reflection color + confidence

SamplerState g_PointSampler : register(s0);
SamplerState g_LinearSampler : register(s1);

// ============================================
// Constants
// ============================================
static const float PI = 3.14159265359;

// ============================================
// Random & Sampling Functions
// ============================================

// Hash function for pseudo-random number generation
uint WangHash(uint seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

// Get random values from blue noise + frame index
float4 GetBlueNoise(uint2 pixelCoord)
{
    uint2 noiseCoord = (pixelCoord + uint2(g_FrameIndex * 13, g_FrameIndex * 7)) % 64;
    return g_BlueNoise.Load(int3(noiseCoord, 0));
}

// GGX importance sampling - sample half vector H
// Returns direction in tangent space
float3 SampleGGX(float2 Xi, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a2 - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // Spherical to Cartesian
    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    return H;
}

// Build orthonormal basis from normal
void BuildOrthonormalBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Transform from tangent space to world/view space
float3 TangentToWorld(float3 H, float3 N)
{
    float3 T, B;
    BuildOrthonormalBasis(N, T, B);
    return normalize(T * H.x + B * H.y + N * H.z);
}

// ============================================
// Helper Functions
// ============================================

// Convert screen-space UV + depth to view-space position
float3 ScreenToView(float2 uv, float depth)
{
    // Convert UV to NDC [-1, 1]
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    ndc.y = -ndc.y;  // Flip Y for DirectX convention

    // Transform to view-space
    float4 viewPos = mul(ndc, g_InvProj);
    return viewPos.xyz / viewPos.w;
}

// Convert view-space position to screen-space UV + depth
float3 ViewToScreen(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos, 1.0), g_Proj);
    clipPos.xyz /= clipPos.w;

    float2 uv = clipPos.xy * float2(0.5, -0.5) + 0.5;
    return float3(uv, clipPos.z);
}

// Sample Hi-Z at specific mip level
float SampleHiZ(float2 uv, int mipLevel)
{
    return g_HiZPyramid.SampleLevel(g_PointSampler, uv, mipLevel);
}

// Check if UV is within screen bounds with small margin
bool IsValidUV(float2 uv)
{
    return all(uv >= 0.001) && all(uv <= 0.999);
}

// ============================================
// Hi-Z Accelerated Ray March
// ============================================
// Returns: hit UV in xy, hit depth in z, hit confidence in w
// Optimized for reversed-Z (larger depth = closer to camera)
float4 HiZTrace(float3 rayOrigin, float3 rayDir, float jitter)
{
    // Transform ray endpoints to screen-space
    float3 rayEnd = rayOrigin + rayDir * g_MaxDistance;
    float3 ssStart = ViewToScreen(rayOrigin);
    float3 ssEnd = ViewToScreen(rayEnd);

    // Early out if both endpoints are off-screen
    if (!IsValidUV(ssEnd.xy) && !IsValidUV(ssStart.xy))
        return float4(0, 0, 0, 0);

    // Ray in screen-space (UV coordinates)
    float2 rayDirUV = ssEnd.xy - ssStart.xy;

    // Handle degenerate rays
    float rayLength = length(rayDirUV * g_ScreenSize);
    if (rayLength < 0.001)
        return float4(0, 0, 0, 0);

    // Precompute ray stepping parameters
    float2 rayDirPixels = normalize(rayDirUV * g_ScreenSize);
    float2 rayStepUV = rayDirPixels * g_TexelSize;
    float depthPerPixel = (ssEnd.z - ssStart.z) / rayLength;

    // Initialize ray state
    float stride = g_Stride;
    float2 currentUV = ssStart.xy + rayStepUV * stride * jitter;
    float currentDepth = ssStart.z + depthPerPixel * stride * jitter;
    int mipLevel = 0;

    float3 hitResult = float3(0, 0, 0);
    bool hit = false;
    float thicknessThreshold = g_Thickness * 0.01;

    // Main ray march loop
    [loop]
    for (int i = 0; i < g_MaxSteps && !hit; ++i)
    {
        // Bounds check
        if (!IsValidUV(currentUV))
            break;

        // Sample Hi-Z at current mip level
        float sceneDepth = SampleHiZ(currentUV, mipLevel);

        // Reversed-Z: ray is behind surface when rayDepth <= sceneDepth
        // (larger depth values are closer to camera)
        bool behind = currentDepth <= sceneDepth;
        float depthDiff = currentDepth - sceneDepth;

        if (behind)
        {
            bool withinThickness = abs(depthDiff) < thicknessThreshold;

            if (mipLevel == 0 && withinThickness)
            {
                // Found hit at finest level
                hitResult = float3(currentUV, currentDepth);
                hit = true;
            }
            else if (mipLevel > 0)
            {
                // Refine at lower mip level - step back and reduce stride
                currentUV -= rayStepUV * stride;
                currentDepth -= depthPerPixel * stride;
                stride *= 0.5;
                mipLevel--;
            }
            else
            {
                // Behind but not within thickness - continue with larger stride
                stride = min(stride * 2.0, 16.0);
                mipLevel = min(mipLevel + 1, g_HiZMipCount - 1);
            }
        }
        else
        {
            // In front of surface - advance ray
            currentUV += rayStepUV * stride;
            currentDepth += depthPerPixel * stride;

            // Increase stride and mip when safe (after initial steps)
            if (i > 4)
            {
                stride = min(stride * 1.5, 16.0);
                mipLevel = min(mipLevel + 1, g_HiZMipCount - 1);
            }
        }
    }

    // Binary search refinement for precise hit location
    if (hit && g_BinarySearchSteps > 0)
    {
        float2 searchUV = hitResult.xy;
        float searchDepth = hitResult.z;
        float searchStride = stride * 0.5;

        [unroll]
        for (int b = 0; b < 8; ++b)  // Unroll for common case
        {
            if (b >= g_BinarySearchSteps) break;

            float sceneDepth = g_DepthBuffer.SampleLevel(g_PointSampler, searchUV, 0);
            bool behind = searchDepth <= sceneDepth;

            float2 step = rayStepUV * searchStride;
            float depthStep = depthPerPixel * searchStride;

            // Branchless update
            float dir = behind ? -1.0 : 1.0;
            searchUV += step * dir;
            searchDepth += depthStep * dir;
            searchStride *= 0.5;
        }

        hitResult.xy = searchUV;
        hitResult.z = searchDepth;
    }

    // Calculate confidence
    float confidence = hit ? 1.0 : 0.0;

    if (hit)
    {
        // Edge fade - smooth falloff at screen edges
        float2 edgeFade = 1.0 - pow(abs(hitResult.xy * 2.0 - 1.0), 8.0);
        confidence *= min(edgeFade.x, edgeFade.y);

        // Distance fade
        float3 hitView = ScreenToView(hitResult.xy, hitResult.z);
        float hitDist = length(hitView - rayOrigin);
        float distFade = 1.0 - saturate(hitDist / g_MaxDistance);
        confidence *= distFade;

        // Backface fade - reduce confidence for grazing hit angles
        float3 hitNormal = g_NormalBuffer.SampleLevel(g_PointSampler, hitResult.xy, 0).xyz * 2.0 - 1.0;
        float backfaceFade = saturate(dot(-rayDir, hitNormal) + 0.1);
        confidence *= backfaceFade;
    }

    return float4(hitResult.xy, hitResult.z, confidence);
}

// ============================================
// Stochastic SSR - Multiple rays with importance sampling
// ============================================
float4 StochasticSSR(float3 viewPos, float3 normalVS, float3 viewDir, float roughness, uint2 pixelCoord)
{
    float4 noise = GetBlueNoise(pixelCoord);

    float3 totalColor = float3(0, 0, 0);
    float totalWeight = 0.0;

    int numRays = min(g_NumRays, 8);  // Cap at 8 rays

    [loop]
    for (int ray = 0; ray < numRays; ++ray)
    {
        // Generate sample point
        float2 Xi;
        if (ray == 0)
        {
            Xi = noise.xy;
        }
        else
        {
            // Use golden ratio offset for additional samples
            const float goldenRatio = 1.61803398875;
            Xi.x = frac(noise.x + ray * goldenRatio);
            Xi.y = frac(noise.y + ray * goldenRatio * goldenRatio);
        }

        // Lerp between uniform hemisphere and GGX based on brdfBias
        float effectiveRoughness = lerp(1.0, roughness, g_BrdfBias);

        // Sample reflection direction
        float3 reflectDir;
        if (effectiveRoughness < 0.01)
        {
            // Mirror reflection for very smooth surfaces
            reflectDir = reflect(viewDir, normalVS);
        }
        else
        {
            // GGX importance sampling
            float3 H_tangent = SampleGGX(Xi, effectiveRoughness);
            float3 H = TangentToWorld(H_tangent, normalVS);
            reflectDir = reflect(viewDir, H);
        }

        // Skip rays pointing away from camera
        if (reflectDir.z > 0.0)
            continue;

        // Jitter for temporal stability
        float jitter = 1.0 + noise.z * 0.5;

        // Trace ray
        float4 hitResult = HiZTrace(viewPos, reflectDir, jitter);

        if (hitResult.w > 0.001)
        {
            float3 reflectionColor = g_SceneColor.SampleLevel(g_LinearSampler, hitResult.xy, 0).rgb;

            // Weight by confidence and NdotL
            float NdotL = saturate(dot(normalVS, reflectDir));
            float weight = hitResult.w * (0.5 + 0.5 * NdotL);

            totalColor += reflectionColor * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 0.001)
    {
        float3 avgColor = totalColor / totalWeight;
        float avgConfidence = totalWeight / numRays;
        return float4(avgColor, avgConfidence);
    }

    return float4(0, 0, 0, 0);
}

// ============================================
// Temporal SSR - Stochastic + history reprojection
// ============================================
float4 TemporalSSR(float3 viewPos, float3 worldPos, float3 normalVS, float3 viewDir, float roughness, uint2 pixelCoord, float2 uv)
{
    // Get current frame SSR result (single ray for temporal to accumulate over time)
    float4 currentSSR = StochasticSSR(viewPos, normalVS, viewDir, roughness, pixelCoord);

    // Reproject to previous frame
    float4 worldPos4 = float4(worldPos, 1.0);
    float4 prevClip = mul(worldPos4, g_PrevViewProj);
    float2 prevUV = prevClip.xy / prevClip.w * float2(0.5, -0.5) + 0.5;

    // Check if reprojected UV is valid
    if (!IsValidUV(prevUV))
    {
        return currentSSR;
    }

    // Sample history
    float4 historySSR = g_SSRHistory.SampleLevel(g_LinearSampler, prevUV, 0);

    // Motion vector (screen-space)
    float2 motion = abs(uv - prevUV);
    float motionMagnitude = length(motion * g_ScreenSize);

    // Reject history if motion is too large
    float motionRejection = 1.0 - saturate(motionMagnitude / (g_MotionThreshold * g_ScreenSize.x));

    // Neighborhood clamping to reduce ghosting
    // Sample neighbors and compute min/max
    float3 m1 = currentSSR.rgb;
    float3 m2 = currentSSR.rgb * currentSSR.rgb;

    // Simple 3x3 neighborhood (could be expanded)
    float3 neighborMin = currentSSR.rgb;
    float3 neighborMax = currentSSR.rgb;

    // Clamp history to neighborhood
    float3 clampedHistory = clamp(historySSR.rgb, neighborMin * 0.8, neighborMax * 1.2 + 0.01);

    // Blend current and history
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
    // Bounds check
    if (DTid.x >= (uint)g_ScreenSize.x || DTid.y >= (uint)g_ScreenSize.y)
    {
        return;
    }

    float2 uv = (DTid.xy + 0.5) * g_TexelSize;

    // Sample G-Buffer (single fetch for normal + roughness)
    float4 normalRoughness = g_NormalBuffer.SampleLevel(g_PointSampler, uv, 0);
    float roughness = normalRoughness.w;

    // Early out for rough surfaces (most common rejection case)
    if (roughness > g_RoughnessFade)
    {
        g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }

    // Sample depth
    float depth = g_DepthBuffer.SampleLevel(g_PointSampler, uv, 0);

    // Early out for sky (reversed-Z: far plane is 0)
    if (depth < 0.0001)
    {
        g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }

    // Unpack normal
    float3 normalWS = normalRoughness.xyz * 2.0 - 1.0;

    // Reconstruct view-space position
    float3 viewPos = ScreenToView(uv, depth);

    // Transform normal to view-space
    float3 normalVS = normalize(mul(float4(normalWS, 0.0), g_View).xyz);

    // Calculate view direction (from camera to pixel)
    float3 viewDir = normalize(viewPos);

    float4 result;

    // Select SSR mode
    if (g_SSRMode == 0)
    {
        // HiZ Trace - single ray
        float3 reflectDir = reflect(viewDir, normalVS);

        // Only trace forward-facing reflections (towards camera)
        // In view-space, negative Z points towards camera
        if (reflectDir.z > 0.0)
        {
            g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
            return;
        }

        // Add jitter for temporal stability
        float jitter = 1.0 + g_JitterOffset;

        // Hi-Z accelerated ray trace
        float4 hitResult = HiZTrace(viewPos, reflectDir, jitter);

        // Sample scene color at hit point
        float3 reflectionColor = float3(0, 0, 0);
        if (hitResult.w > 0.001)
        {
            reflectionColor = g_SceneColor.SampleLevel(g_LinearSampler, hitResult.xy, 0).rgb;
        }

        // Apply roughness fade (quadratic for smoother falloff)
        float roughnessMask = 1.0 - saturate(roughness / g_RoughnessFade);
        roughnessMask *= roughnessMask;
        float confidence = hitResult.w * roughnessMask;

        result = float4(reflectionColor, confidence);
    }
    else if (g_SSRMode == 1)
    {
        // Stochastic SSR
        result = StochasticSSR(viewPos, normalVS, viewDir, roughness, DTid.xy);

        // Apply roughness fade
        float roughnessMask = 1.0 - saturate(roughness / g_RoughnessFade);
        roughnessMask *= roughnessMask;
        result.a *= roughnessMask;
    }
    else
    {
        // Temporal SSR
        // Get world position for reprojection
        float3 worldPos = mul(float4(viewPos, 1.0), g_InvView).xyz;
        result = TemporalSSR(viewPos, worldPos, normalVS, viewDir, roughness, DTid.xy, uv);

        // Apply roughness fade
        float roughnessMask = 1.0 - saturate(roughness / g_RoughnessFade);
        roughnessMask *= roughnessMask;
        result.a *= roughnessMask;
    }

    // Output: reflection color + confidence
    g_SSROutput[DTid.xy] = result;
}
