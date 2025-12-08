// ============================================
// LightProbe.hlsl - Light Probe Shader Utilities
// ============================================
// 提供 Light Probe 的 SH 解码和采样功能
//
// 使用方式：
// #include "LightProbe.hlsl"
// bool hasProbe;
// float3 diffuseIBL = SampleLightProbes(worldPos, normal, hasProbe);
// ============================================

#ifndef LIGHT_PROBE_HLSL
#define LIGHT_PROBE_HLSL

// ============================================
// Constants
// ============================================
#define SH_COEFF_COUNT 9

// ============================================
// Data Structures
// ============================================

// 单个 Light Probe 数据（与 C++ 对应）
struct LightProbeData
{
    float3 position;    // Probe 位置
    float radius;       // 影响半径（暂未使用）
    float3 shCoeffs[9]; // L2 球谐系数（9 个，每个是 RGB）
};

// ============================================
// Resources
// ============================================

// t15: Light Probe Buffer
StructuredBuffer<LightProbeData> g_lightProbes : register(t15);

// b5: Light Probe Parameters
cbuffer CB_LightProbeParams : register(b5)
{
    int g_lightProbeCount;          // 当前场景中的 Probe 数量
    float g_lightProbeBlendFalloff; // 预留参数（暂未使用）
    float2 _lightProbePad;
};

// ============================================
// SH Decoding (L2)
// ============================================
// 从 SH 系数重建辐照度
// 输入：n（归一化方向），shCoeffs（9 个 float3）
// 输出：RGB 辐照度

float3 EvaluateSH(float3 n, float3 shCoeffs[9])
{
    float3 result = 0;

    // L0 (1 个系数)
    result += shCoeffs[0] * 0.282095;

    // L1 (3 个系数)
    result += shCoeffs[1] * 0.488603 * n.y;
    result += shCoeffs[2] * 0.488603 * n.z;
    result += shCoeffs[3] * 0.488603 * n.x;

    // L2 (5 个系数)
    result += shCoeffs[4] * 1.092548 * n.x * n.y;
    result += shCoeffs[5] * 1.092548 * n.y * n.z;
    result += shCoeffs[6] * 0.315392 * (3.0 * n.z * n.z - 1.0);
    result += shCoeffs[7] * 1.092548 * n.x * n.z;
    result += shCoeffs[8] * 0.546274 * (n.x * n.x - n.y * n.y);

    // Clamp 负值（SH ringing artifact）
    return max(result, 0.0);
}

// ============================================
// Light Probe Sampling
// ============================================
// 当前实现：最简单的"最近一个 Probe"
//
// TODO: 后续改进为四面体插值
// - CPU 端构建 Delaunay 四面体网格
// - CPU 端查找四面体并计算重心坐标
// - CPU 端混合 4 个 Probe 的 SH
// - 传混合后的 SH 给 Shader
// ============================================

float3 SampleLightProbes(float3 worldPos, float3 normal, out bool hasProbe)
{
    hasProbe = false;

    if (g_lightProbeCount == 0)
    {
        return float3(0, 0, 0);
    }

    // ============================================
    // 查找最近的 Probe
    // ============================================
    int nearestIndex = -1;
    float nearestDist = 1e10;

    for (int i = 0; i < g_lightProbeCount; i++)
    {
        float3 delta = worldPos - g_lightProbes[i].position;
        float dist = length(delta);

        if (dist < nearestDist)
        {
            nearestDist = dist;
            nearestIndex = i;
        }
    }

    // ============================================
    // 采样最近的 Probe
    // ============================================
    if (nearestIndex >= 0)
    {
        hasProbe = true;
        return EvaluateSH(normal, g_lightProbes[nearestIndex].shCoeffs);
    }

    return float3(0, 0, 0);
}

#endif // LIGHT_PROBE_HLSL
