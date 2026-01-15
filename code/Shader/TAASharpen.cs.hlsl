// TAASharpen.cs.hlsl - Post-TAA Sharpening (Unsharp Mask)

cbuffer CB_TAASharpen : register(b0)
{
    float2 g_ScreenSize;
    float2 g_TexelSize;
    float g_SharpenStrength;
    float3 _pad;
};

Texture2D<float4> g_Input : register(t0);
RWTexture2D<float4> g_Output : register(u0);

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
