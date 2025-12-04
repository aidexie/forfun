// ============================================
// LightProbe.hlsl - Light Probe Shader Utilities
// ============================================
// 提供 Light Probe 的 SH 解码和混合功能
//
// 使用方式：
// #include "LightProbe.hlsl"
// float3 diffuseIBL = SampleLightProbes(worldPos, normal);
// ============================================

#ifndef LIGHT_PROBE_HLSL
#define LIGHT_PROBE_HLSL

// ============================================
// Constants
// ============================================
#define MAX_BLEND_PROBES 4
#define SH_COEFF_COUNT 9

// ============================================
// Data Structures
// ============================================

// 单个 Light Probe 数据（与 C++ 对应）
struct LightProbeData
{
    float3 position;    // Probe 位置
    float radius;       // 影响半径
    float3 shCoeffs[9]; // L2 球谐系数（9 bands，每个是 RGB）
};

// ============================================
// Resources
// ============================================

// t15: Light Probe Buffer
StructuredBuffer<LightProbeData> g_lightProbes : register(t15);

// b5: Light Probe Parameters
cbuffer CB_LightProbeParams : register(b5)
{
    int g_lightProbeCount;      // 当前场景中的 Probe 数量
    float g_lightProbeBlendFalloff; // 权重衰减指数（默认 2.0）
    float2 _lightProbePad;
};

// ============================================
// Spherical Harmonics L2 Basis Functions
// ============================================
// 使用直角坐标形式（更适合 shader）
// 参考：Stupid Spherical Harmonics Tricks (Sloan, GDC 2008)

// L0 (1 个系数)
float SH_Y00()
{
    return 0.282095;  // 1 / (2 * sqrt(π))
}

// L1 (3 个系数)
float SH_Y1m1(float3 n)
{
    return 0.488603 * n.y;  // sqrt(3 / (4π)) * y
}

float SH_Y10(float3 n)
{
    return 0.488603 * n.z;  // sqrt(3 / (4π)) * z
}

float SH_Y11(float3 n)
{
    return 0.488603 * n.x;  // sqrt(3 / (4π)) * x
}

// L2 (5 个系数)
float SH_Y2m2(float3 n)
{
    return 1.092548 * n.x * n.y;  // sqrt(15 / (4π)) * x * y
}

float SH_Y2m1(float3 n)
{
    return 1.092548 * n.y * n.z;  // sqrt(15 / (4π)) * y * z
}

float SH_Y20(float3 n)
{
    return 0.315392 * (3.0 * n.z * n.z - 1.0);  // sqrt(5 / (16π)) * (3z² - 1)
}

float SH_Y21(float3 n)
{
    return 1.092548 * n.x * n.z;  // sqrt(15 / (4π)) * x * z
}

float SH_Y22(float3 n)
{
    return 0.546274 * (n.x * n.x - n.y * n.y);  // sqrt(15 / (16π)) * (x² - y²)
}

// ============================================
// SH Decoding (Reconstruction)
// ============================================
// 从 SH 系数重建辐照度
// 输入：normal（归一化方向），shCoeffs（9 个 float3）
// 输出：RGB 辐照度

float3 EvaluateSH(float3 normal, float3 shCoeffs[9])
{
    float3 result = 0;

    // L0 (band 0)
    result += shCoeffs[0] * SH_Y00();

    // L1 (bands 1-3)
    result += shCoeffs[1] * SH_Y1m1(normal);
    result += shCoeffs[2] * SH_Y10(normal);
    result += shCoeffs[3] * SH_Y11(normal);

    // L2 (bands 4-8)
    result += shCoeffs[4] * SH_Y2m2(normal);
    result += shCoeffs[5] * SH_Y2m1(normal);
    result += shCoeffs[6] * SH_Y20(normal);
    result += shCoeffs[7] * SH_Y21(normal);
    result += shCoeffs[8] * SH_Y22(normal);

    // Clamp 负值（SH ringing artifact）
    return max(result, 0.0);
}

// ============================================
// Light Probe Blending
// ============================================
// 距离权重混合最近的 4 个 Probe
// 输入：worldPos（世界坐标），normal（归一化法线）
// 输出：混合后的 RGB 辐照度

float3 SampleLightProbes(float3 worldPos, float3 normal)
{
    // 没有 probe → 返回 0（Shader 外部会 fallback 到全局 IBL）
    if (g_lightProbeCount == 0) {
        return float3(0, 0, 0);
    }

    // ============================================
    // 1. 收集附近的 probe（在 radius 范围内）
    // ============================================
    struct ProbeCandidate {
        int index;
        float distance;
        float weight;
    };

    ProbeCandidate candidates[MAX_BLEND_PROBES];
    int candidateCount = 0;

    for (int i = 0; i < g_lightProbeCount && candidateCount < MAX_BLEND_PROBES; i++)
    {
        LightProbeData probe = g_lightProbes[i];
        float3 delta = worldPos - probe.position;
        float dist = length(delta);

        // 在 radius 范围内
        if (dist < probe.radius)
        {
            // 插入排序（按距离从近到远）
            int insertPos = candidateCount;
            for (int j = 0; j < candidateCount; j++) {
                if (dist < candidates[j].distance) {
                    insertPos = j;
                    break;
                }
            }

            // 移动后面的元素
            for (int j = candidateCount; j > insertPos; j--) {
                if (j < MAX_BLEND_PROBES) {
                    candidates[j] = candidates[j - 1];
                }
            }

            // 插入新元素
            if (insertPos < MAX_BLEND_PROBES) {
                candidates[insertPos].index = i;
                candidates[insertPos].distance = dist;
                candidates[insertPos].weight = 0;  // 稍后计算
            }

            candidateCount = min(candidateCount + 1, MAX_BLEND_PROBES);
        }
    }

    // ============================================
    // 2. Fallback：没有 probe 覆盖 → 返回 0
    // ============================================
    if (candidateCount == 0) {
        return float3(0, 0, 0);
    }

    // ============================================
    // 3. 计算距离权重
    // ============================================
    float totalWeight = 0.0;
    for (int i = 0; i < candidateCount; i++)
    {
        float dist = candidates[i].distance;
        float weight = 1.0 / pow(dist + 0.1, g_lightProbeBlendFalloff);  // 逆平方衰减
        candidates[i].weight = weight;
        totalWeight += weight;
    }

    // ============================================
    // 4. 混合 SH 系数并解码
    // ============================================
    float3 result = 0;

    for (int i = 0; i < candidateCount; i++)
    {
        LightProbeData probe = g_lightProbes[candidates[i].index];
        float weight = candidates[i].weight / totalWeight;  // 归一化权重

        // 解码 SH 并累加
        float3 irradiance = EvaluateSH(normal, probe.shCoeffs);
        result += irradiance * weight;
    }

    return result;
}

#endif // LIGHT_PROBE_HLSL
