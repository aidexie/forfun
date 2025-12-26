// ============================================
// 2D Lightmap Finalize Compute Shader
// ============================================
// Normalizes accumulated radiance and writes to output texture.
//
// Input: Accumulation buffer (xyz = radiance sum, w = sample count)
// Output: HDR lightmap texture (R16G16B16A16_FLOAT)

// ============================================
// Constant Buffer
// ============================================

cbuffer CB_FinalizeParams : register(b0) {
    uint g_TotalTexels;
    uint g_SamplesPerTexel;
    uint g_MaxBounces;
    float g_SkyIntensity;

    uint g_AtlasWidth;
    uint g_AtlasHeight;
    uint g_BatchOffset;
    uint g_BatchSize;

    uint g_FrameIndex;
    uint g_NumLights;
    uint2 g_Padding;
};

// ============================================
// Resource Bindings
// ============================================

// Input: Accumulation buffer
StructuredBuffer<float4> g_Accumulation : register(t0);

// Output: Lightmap texture
RWTexture2D<float4> g_OutputTexture : register(u0);

// ============================================
// Main Compute Shader
// ============================================

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint x = dispatchThreadID.x;
    uint y = dispatchThreadID.y;

    // Bounds check
    if (x >= g_AtlasWidth || y >= g_AtlasHeight) {
        return;
    }

    // Calculate buffer index
    uint idx = y * g_AtlasWidth + x;

    // Load accumulated data
    float4 accumulated = g_Accumulation[idx];
    float3 radianceSum = accumulated.xyz;
    float sampleCount = accumulated.w;

    // Normalize by sample count
    float3 finalRadiance = float3(0, 0, 0);
    if (sampleCount > 0.0f) {
        finalRadiance = radianceSum / sampleCount;
    }

    // Clamp to reasonable HDR range
    finalRadiance = clamp(finalRadiance, 0.0f, 100.0f);

    // Write to output texture
    // Alpha = 1 for valid texels, 0 for invalid
    float alpha = (sampleCount > 0.0f) ? 1.0f : 0.0f;
    g_OutputTexture[uint2(x, y)] = float4(finalRadiance, alpha);
}
