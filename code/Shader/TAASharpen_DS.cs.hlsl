// ============================================
// TAASharpen_DS.cs.hlsl - Post-TAA Sharpening (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
// Unsharp Mask sharpening filter
// ============================================

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_TAASharpen : register(b0, space1)
{
    float2 g_ScreenSize;
    float2 g_TexelSize;
    float g_SharpenStrength;
    float3 _pad;
};

// Texture SRVs (t0-t7, space1)
Texture2D<float4> g_Input : register(t0, space1);

// UAVs (u0-u3, space1)
RWTexture2D<float4> g_Output : register(u0, space1);

// Samplers (s0-s3, space1) - not used but declared for layout compatibility
SamplerState g_PointSampler : register(s0, space1);
SamplerState g_LinearSampler : register(s1, space1);

// ============================================
// Main Sharpen Compute Shader
// ============================================
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)g_ScreenSize.x || DTid.y >= (uint)g_ScreenSize.y)
        return;

    float3 center = g_Input[DTid.xy].rgb;
    float3 top    = g_Input[DTid.xy + int2(0, -1)].rgb;
    float3 bottom = g_Input[DTid.xy + int2(0,  1)].rgb;
    float3 left   = g_Input[DTid.xy + int2(-1, 0)].rgb;
    float3 right  = g_Input[DTid.xy + int2( 1, 0)].rgb;

    float3 blur = (top + bottom + left + right) * 0.25;
    float3 sharp = center + (center - blur) * g_SharpenStrength;

    g_Output[DTid.xy] = float4(max(sharp, 0.0), 1.0);
}
