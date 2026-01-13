// SSR.cs.hlsl
// Screen-Space Reflections with Hi-Z accelerated ray marching
//
// Reference: "Efficient GPU Screen-Space Ray Tracing"
//            Morgan McGuire & Michael Mara (2014)
//
// Uses Hi-Z pyramid for efficient ray-depth intersection testing.
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
    float2 g_Pad;
};

// ============================================
// Resources
// ============================================
Texture2D<float> g_DepthBuffer : register(t0);      // Full-res depth
Texture2D<float4> g_NormalBuffer : register(t1);    // Normal.xyz + Roughness
Texture2D<float> g_HiZPyramid : register(t2);       // Hi-Z depth pyramid
Texture2D<float4> g_SceneColor : register(t3);      // HDR scene color

RWTexture2D<float4> g_SSROutput : register(u0);     // Output: reflection color + confidence

SamplerState g_PointSampler : register(s0);
SamplerState g_LinearSampler : register(s1);

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

    // Calculate reflection direction in view-space
    float3 viewDir = normalize(viewPos);
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

    // Output: reflection color + confidence
    g_SSROutput[DTid.xy] = float4(reflectionColor, confidence);
}
