// ============================================
// TAA_DS.cs.hlsl - Temporal Anti-Aliasing (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
//
// Algorithm levels (g_Algorithm):
// 1: Basic - Simple blend (heavy ghosting)
// 2: NeighborhoodClamp - Min/max AABB clamping
// 3: VarianceClip - Variance clipping + YCoCg color space
// 4: CatmullRom - + Catmull-Rom history sampling
// 5: MotionRejection - + Motion/depth rejection
// 6: Production - Full quality (sharpening handled separately)
// ============================================

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_TAA : register(b0, space1)
{
    float4x4 g_InvViewProj;
    float4x4 g_PrevViewProj;
    float2 g_ScreenSize;
    float2 g_TexelSize;
    float2 g_JitterOffset;
    float2 g_PrevJitterOffset;
    float g_HistoryBlend;
    float g_VarianceClipGamma;
    float g_VelocityRejectionScale;
    float g_DepthRejectionScale;
    uint g_Algorithm;
    uint g_FrameIndex;
    uint g_Flags;  // Bit 0: first frame (no history)
    float _pad;
};

// Texture SRVs (t0-t7, space1)
Texture2D<float4> g_CurrentColor : register(t0, space1);
Texture2D<float2> g_VelocityBuffer : register(t1, space1);
Texture2D<float> g_DepthBuffer : register(t2, space1);
Texture2D<float4> g_HistoryColor : register(t3, space1);

// UAVs (u0-u3, space1)
RWTexture2D<float4> g_Output : register(u0, space1);

// Samplers (s0-s3, space1)
SamplerState g_PointSampler : register(s0, space1);
SamplerState g_LinearSampler : register(s1, space1);

// ============================================
// Helper Functions
// ============================================

// Color space conversion (YCoCg provides better perceptual clamping)
float3 RGBToYCoCg(float3 rgb)
{
    float Y  = dot(rgb, float3(0.25, 0.5, 0.25));
    float Co = dot(rgb, float3(0.5, 0.0, -0.5));
    float Cg = dot(rgb, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// HDR tonemap for stable blending (Level 5+)
float3 Tonemap(float3 color)
{
    return color / (1.0 + Luminance(color));
}

float3 TonemapInverse(float3 color)
{
    return color / max(1.0 - Luminance(color), 0.001);
}

// Neighborhood Min/Max (Level 2)
void ComputeNeighborhoodMinMax(uint2 pixel, out float3 minC, out float3 maxC)
{
    minC = float3(1e10, 1e10, 1e10);
    maxC = float3(-1e10, -1e10, -1e10);

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float3 c = g_CurrentColor[pixel + int2(x, y)].rgb;
            minC = min(minC, c);
            maxC = max(maxC, c);
        }
    }
}

// Variance Statistics (Level 3+)
void ComputeVarianceStatistics(uint2 pixel, out float3 mean, out float3 stddev, out float3 minC, out float3 maxC)
{
    float3 m1 = float3(0, 0, 0);
    float3 m2 = float3(0, 0, 0);
    minC = float3(1e10, 1e10, 1e10);
    maxC = float3(-1e10, -1e10, -1e10);

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float3 c = g_CurrentColor[pixel + int2(x, y)].rgb;
            float3 ycocg = RGBToYCoCg(c);
            m1 += ycocg;
            m2 += ycocg * ycocg;
            minC = min(minC, ycocg);
            maxC = max(maxC, ycocg);
        }
    }

    m1 /= 9.0;
    m2 /= 9.0;
    mean = m1;
    stddev = sqrt(max(m2 - m1 * m1, 0.0));
}

// Clip to AABB (Level 3+) - clips towards center instead of hard clamping
float3 ClipToAABB(float3 color, float3 minimum, float3 maximum)
{
    float3 center = 0.5 * (maximum + minimum);
    float3 extents = 0.5 * (maximum - minimum);
    float3 offset = color - center;
    float3 ts = abs(extents) / max(abs(offset), 0.0001);
    float t = saturate(min(min(ts.x, ts.y), ts.z));
    return center + offset * t;
}

// Catmull-Rom History Sampling (Level 4+) - higher quality than bilinear
float4 SampleHistoryCatmullRom(float2 uv)
{
    float2 position = uv * g_ScreenSize;
    float2 centerPosition = floor(position - 0.5) + 0.5;
    float2 f = position - centerPosition;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * (f3 - f2);

    float2 w12 = w1 + w2;
    float2 tc12 = (centerPosition + w2 / w12) * g_TexelSize;
    float2 tc0 = (centerPosition - 1.0) * g_TexelSize;
    float2 tc3 = (centerPosition + 2.0) * g_TexelSize;

    float4 result =
        (w12.x * w0.y)  * g_HistoryColor.SampleLevel(g_LinearSampler, float2(tc12.x, tc0.y), 0) +
        (w0.x  * w12.y) * g_HistoryColor.SampleLevel(g_LinearSampler, float2(tc0.x, tc12.y), 0) +
        (w12.x * w12.y) * g_HistoryColor.SampleLevel(g_LinearSampler, float2(tc12.x, tc12.y), 0) +
        (w3.x  * w12.y) * g_HistoryColor.SampleLevel(g_LinearSampler, float2(tc3.x, tc12.y), 0) +
        (w12.x * w3.y)  * g_HistoryColor.SampleLevel(g_LinearSampler, float2(tc12.x, tc3.y), 0);

    float totalWeight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) +
                        (w3.x * w12.y) + (w12.x * w3.y);
    return result / totalWeight;
}

// Motion/Depth Rejection Weight (Level 5+)
float ComputeRejectionWeight(uint2 pixel, float2 velocity)
{
    float weight = 1.0;

    // Velocity rejection
    float velocityLength = length(velocity * g_ScreenSize);
    weight *= saturate(1.0 - velocityLength * g_VelocityRejectionScale);

    // Depth rejection via gradient
    float currentDepth = g_DepthBuffer[pixel];
    float depthGradient = 0;
    depthGradient += abs(g_DepthBuffer[pixel + int2(1, 0)] - currentDepth);
    depthGradient += abs(g_DepthBuffer[pixel + int2(-1, 0)] - currentDepth);
    depthGradient += abs(g_DepthBuffer[pixel + int2(0, 1)] - currentDepth);
    depthGradient += abs(g_DepthBuffer[pixel + int2(0, -1)] - currentDepth);
    weight *= saturate(1.0 - depthGradient * g_DepthRejectionScale);

    return weight;
}

// ============================================
// Main TAA Compute Shader
// ============================================
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)g_ScreenSize.x || DTid.y >= (uint)g_ScreenSize.y)
        return;

    float2 uv = (DTid.xy + 0.5) * g_TexelSize;
    float3 current = g_CurrentColor[DTid.xy].rgb;

    // Get motion vector and compute reprojected UV
    float2 velocity = g_VelocityBuffer[DTid.xy].xy;
    float2 historyUV = uv - velocity;

    // Check if reprojected UV is valid
    bool historyValid = all(historyUV >= 0.0) && all(historyUV <= 1.0) && ((g_Flags & 1) == 0);

    if (!historyValid)
    {
        g_Output[DTid.xy] = float4(current, 1.0);
        return;
    }

    // Sample history (algorithm-dependent)
    float3 history = (g_Algorithm >= 4)
        ? SampleHistoryCatmullRom(historyUV).rgb
        : g_HistoryColor.SampleLevel(g_LinearSampler, historyUV, 0).rgb;

    // Apply clamping/clipping (algorithm-dependent)
    if (g_Algorithm >= 2)
    {
        float3 minC, maxC;
        ComputeNeighborhoodMinMax(DTid.xy, minC, maxC);
        history = clamp(history, minC, maxC);
    }

    // Compute blend weight
    float blend = g_HistoryBlend;
    if (g_Algorithm >= 5)
    {
        // Only reject based on motion, not spatial depth edges or luminance
        float velocityLength = length(velocity * g_ScreenSize);
        blend *= saturate(1.0 - velocityLength * g_VelocityRejectionScale);
    }

    // Blend current and history
    float3 result;
    if (g_Algorithm >= 5)
    {
        // Blend in tonemapped space for HDR stability
        float3 currentTM = Tonemap(current);
        float3 historyTM = Tonemap(history);
        result = TonemapInverse(lerp(currentTM, historyTM, blend));
    }
    else
    {
        result = lerp(current, history, blend);
    }

    g_Output[DTid.xy] = float4(result, 1.0);
}
