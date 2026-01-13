// SSR.cs.hlsl
// Screen-Space Reflections with Hi-Z accelerated ray marching
//
// Reference: "Efficient GPU Screen-Space Ray Tracing"
//            Morgan McGuire & Michael Mara (2014)
//
// Uses Hi-Z pyramid for efficient ray-depth intersection testing.
// Reversed-Z aware (near=1.0, far=0.0)

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
    uint g_UseReversedZ;       // 0 = standard-Z, 1 = reversed-Z
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

// Linear depth from reversed-Z buffer value
float LinearizeDepth(float depth)
{
    if (g_UseReversedZ)
    {
        // Reversed-Z: near=1.0, far=0.0
        // z = near * far / (far - depth * (far - near))
        return g_NearZ * g_FarZ / (g_FarZ - depth * (g_FarZ - g_NearZ));
    }
    else
    {
        // Standard-Z: near=0.0, far=1.0
        return g_NearZ * g_FarZ / (g_FarZ - depth * (g_FarZ - g_NearZ));
    }
}

// Check if UV is within screen bounds
bool IsValidUV(float2 uv)
{
    return all(uv >= 0.0) && all(uv <= 1.0);
}

// ============================================
// Hi-Z Accelerated Ray March
// ============================================
// Returns: hit UV in xy, hit depth in z, hit confidence in w
float4 HiZTrace(float3 rayOrigin, float3 rayDir, float jitter)
{
    // Transform ray to screen-space
    float3 rayEnd = rayOrigin + rayDir * g_MaxDistance;

    float3 ssStart = ViewToScreen(rayOrigin);
    float3 ssEnd = ViewToScreen(rayEnd);

    // Early out if ray points away from screen
    if (!IsValidUV(ssEnd.xy) && !IsValidUV(ssStart.xy))
        return float4(0, 0, 0, 0);

    // Ray in screen-space (UV coordinates)
    float2 rayStartUV = ssStart.xy;
    float2 rayEndUV = ssEnd.xy;
    float2 rayDirUV = rayEndUV - rayStartUV;

    // Handle degenerate rays
    float rayLength = length(rayDirUV * g_ScreenSize);
    if (rayLength < 0.001)
        return float4(0, 0, 0, 0);

    // Normalize ray direction in pixel space
    float2 rayDirPixels = normalize(rayDirUV * g_ScreenSize);
    float2 rayStepUV = rayDirPixels * g_TexelSize;

    // Calculate depth gradient for ray
    float depthStart = ssStart.z;
    float depthEnd = ssEnd.z;
    float depthDelta = depthEnd - depthStart;
    float depthPerPixel = depthDelta / rayLength;

    // Start marching
    float2 currentUV = rayStartUV + rayStepUV * g_Stride * jitter;
    float currentDepth = depthStart + depthPerPixel * g_Stride * jitter;
    float stride = g_Stride;
    int mipLevel = 0;

    float3 hitResult = float3(0, 0, 0);
    bool hit = false;

    for (int i = 0; i < g_MaxSteps && !hit; ++i)
    {
        if (!IsValidUV(currentUV))
            break;

        // Sample Hi-Z at current mip level
        float sceneDepth = SampleHiZ(currentUV, mipLevel);

        // Thickness test (reversed-Z: ray depth > scene depth means behind surface)
        float depthDiff;
        if (g_UseReversedZ)
        {
            // Reversed-Z: larger depth = closer
            // Hit if ray is behind or at surface (ray depth <= scene depth)
            // and within thickness (ray depth >= scene depth - thickness_in_depth_space)
            depthDiff = currentDepth - sceneDepth;
        }
        else
        {
            depthDiff = sceneDepth - currentDepth;
        }

        // Check intersection
        bool behind = (g_UseReversedZ) ? (currentDepth <= sceneDepth) : (currentDepth >= sceneDepth);
        bool withinThickness = abs(depthDiff) < g_Thickness * 0.01;

        if (behind)
        {
            if (mipLevel == 0 && withinThickness)
            {
                // Found hit at finest level
                hitResult = float3(currentUV, currentDepth);
                hit = true;
            }
            else if (mipLevel > 0)
            {
                // Refine at lower mip level
                currentUV -= rayStepUV * stride;
                currentDepth -= depthPerPixel * stride;
                stride *= 0.5;
                mipLevel = max(0, mipLevel - 1);
            }
            else
            {
                // Behind but not within thickness - continue
                stride *= 2.0;
                mipLevel = min(mipLevel + 1, g_HiZMipCount - 1);
            }
        }
        else
        {
            // In front of surface - advance ray
            currentUV += rayStepUV * stride;
            currentDepth += depthPerPixel * stride;

            // Increase stride and mip when safe
            if (i > 4)
            {
                stride = min(stride * 1.5, 16.0);
                mipLevel = min(mipLevel + 1, g_HiZMipCount - 1);
            }
        }
    }

    // Binary search refinement
    if (hit)
    {
        float2 searchUV = hitResult.xy;
        float searchDepth = hitResult.z;
        float searchStride = stride * 0.5;

        for (int b = 0; b < g_BinarySearchSteps; ++b)
        {
            float sceneDepth = g_DepthBuffer.SampleLevel(g_PointSampler, searchUV, 0);

            bool behind = (g_UseReversedZ) ? (searchDepth <= sceneDepth) : (searchDepth >= sceneDepth);

            if (behind)
            {
                searchUV -= rayStepUV * searchStride;
                searchDepth -= depthPerPixel * searchStride;
            }
            else
            {
                searchUV += rayStepUV * searchStride;
                searchDepth += depthPerPixel * searchStride;
            }

            searchStride *= 0.5;
        }

        hitResult.xy = searchUV;
        hitResult.z = searchDepth;
    }

    // Calculate confidence
    float confidence = hit ? 1.0 : 0.0;

    if (hit)
    {
        // Edge fade
        float2 edgeFade = 1.0 - pow(abs(hitResult.xy * 2.0 - 1.0), 8.0);
        confidence *= min(edgeFade.x, edgeFade.y);

        // Distance fade
        float3 hitView = ScreenToView(hitResult.xy, hitResult.z);
        float hitDist = length(hitView - rayOrigin);
        float distFade = 1.0 - saturate(hitDist / g_MaxDistance);
        confidence *= distFade;

        // Fade based on ray direction (grazing angles less reliable)
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

    // Sample G-Buffer
    float depth = g_DepthBuffer.SampleLevel(g_PointSampler, uv, 0);
    float4 normalRoughness = g_NormalBuffer.SampleLevel(g_PointSampler, uv, 0);

    float3 normalWS = normalRoughness.xyz * 2.0 - 1.0;
    float roughness = normalRoughness.w;

    // Early out for sky (depth at far plane)
    bool isSky = (g_UseReversedZ) ? (depth < 0.0001) : (depth > 0.9999);
    if (isSky)
    {
        g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }

    // Early out for rough surfaces
    if (roughness > g_RoughnessFade)
    {
        g_SSROutput[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }

    // Reconstruct view-space position
    float3 viewPos = ScreenToView(uv, depth);

    // Transform normal to view-space
    float3 normalVS = normalize(mul(float4(normalWS, 0.0), g_View).xyz);

    // Calculate reflection direction in view-space
    float3 viewDir = normalize(viewPos);
    float3 reflectDir = reflect(viewDir, normalVS);

    // Only trace forward-facing reflections
    if (reflectDir.z > 0.0)  // Pointing away from camera
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

    // Apply roughness fade
    float roughnessMask = 1.0 - saturate(roughness / g_RoughnessFade);
    float confidence = hitResult.w * roughnessMask;

    // Output: reflection color + confidence
    g_SSROutput[DTid.xy] = float4(reflectionColor, confidence);
}
