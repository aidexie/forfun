#include "PathTraceBaker.h"
#include "RayTracer.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Components/PointLight.h"
#include "Engine/Components/SpotLight.h"
#include "Core/FFLog.h"
#include <cmath>

using namespace DirectX;

// ============================================
// 常量
// ============================================
static const float PI = 3.14159265358979323846f;
static const float INV_PI = 1.0f / PI;

// ============================================
// 生命周期
// ============================================

bool CPathTraceBaker::Initialize(CScene& scene, const SPathTraceConfig& config)
{
    if (m_initialized) {
        Shutdown();
    }

    m_config = config;

    // 创建 Ray Tracer
    m_rayTracer = new CRayTracer();
    if (!m_rayTracer->Initialize(scene)) {
        CFFLog::Error("[PathTraceBaker] Failed to initialize RayTracer");
        delete m_rayTracer;
        m_rayTracer = nullptr;
        return false;
    }

    // 初始化随机数生成器
    std::random_device rd;
    m_rng.seed(rd());

    m_initialized = true;
    CFFLog::Info("[PathTraceBaker] Initialized: samples=%d, bounces=%d, RR=%s",
                 m_config.samplesPerVoxel, m_config.maxBounces,
                 m_config.useRussianRoulette ? "on" : "off");

    return true;
}

void CPathTraceBaker::Shutdown()
{
    if (m_rayTracer) {
        m_rayTracer->Shutdown();
        delete m_rayTracer;
        m_rayTracer = nullptr;
    }

    m_initialized = false;
}

void CPathTraceBaker::RebuildBVH(CScene& scene)
{
    if (m_rayTracer) {
        m_rayTracer->Rebuild(scene);
    }
}

// ============================================
// 烘焙
// ============================================

void CPathTraceBaker::BakeVoxel(
    const XMFLOAT3& position,
    CScene& scene,
    std::array<XMFLOAT3, 9>& outSH)
{
    // 初始化 SH 系数为零
    for (auto& coeff : outSH) {
        coeff = {0, 0, 0};
    }

    if (!m_initialized || !m_rayTracer) {
        return;
    }

    int numSamples = m_config.samplesPerVoxel;

    // Monte Carlo 采样
    for (int s = 0; s < numSamples; s++)
    {
        // 生成均匀球面采样方向
        float u1 = random();
        float u2 = random();
        XMFLOAT3 direction = sampleSphereUniform(u1, u2);

        // 追踪该方向的入射辐射度
        XMFLOAT3 radiance = traceRadiance(position, direction, scene, 0);

        // 累加到 SH
        // 均匀球面采样的权重 = 4π / numSamples
        float weight = 4.0f * PI / numSamples;
        accumulateToSH(direction, radiance, weight, outSH);
    }
}

// ============================================
// Path Tracing 核心
// ============================================

XMFLOAT3 CPathTraceBaker::traceRadiance(
    const XMFLOAT3& origin,
    const XMFLOAT3& direction,
    CScene& scene,
    int depth)
{
    // 终止条件
    if (depth > m_config.maxBounces) {
        return {0, 0, 0};
    }

    // 追踪射线
    SRayHit hit = m_rayTracer->TraceRay(origin, direction);

    if (!hit.valid) {
        // 未命中：返回天空盒颜色
        return sampleSkybox(direction, scene);
    }

    // 命中：计算该点的光照
    XMFLOAT3 directLight = evaluateDirectLight(hit.position, hit.normal, hit.albedo, scene);

    // 如果已达最大深度，只返回直接光
    if (depth >= m_config.maxBounces) {
        return directLight;
    }

    // Russian Roulette 终止
    float rrProbability = 1.0f;
    if (m_config.useRussianRoulette && depth >= (int)m_config.rrStartBounce) {
        // 概率 = albedo 的最大分量
        rrProbability = std::max({hit.albedo.x, hit.albedo.y, hit.albedo.z});
        rrProbability = std::max(rrProbability, m_config.rrMinProbability);

        if (random() > rrProbability) {
            return directLight;  // 终止，只返回直接光
        }
    }

    // 继续反弹：采样下一个方向
    float u1 = random();
    float u2 = random();
    XMFLOAT3 bounceDir = sampleHemisphereCosine(hit.normal, u1, u2);

    // 偏移起点，避免自相交
    XMFLOAT3 bounceOrigin = {
        hit.position.x + hit.normal.x * 0.001f,
        hit.position.y + hit.normal.y * 0.001f,
        hit.position.z + hit.normal.z * 0.001f
    };

    // 递归追踪
    XMFLOAT3 indirectRadiance = traceRadiance(bounceOrigin, bounceDir, scene, depth + 1);

    // BRDF: Lambertian diffuse = albedo / π
    // Cosine-weighted 采样的 PDF = cos(θ) / π
    // 因此 BRDF / PDF = albedo
    XMFLOAT3 indirectContrib = {
        indirectRadiance.x * hit.albedo.x,
        indirectRadiance.y * hit.albedo.y,
        indirectRadiance.z * hit.albedo.z
    };

    // Russian Roulette 补偿
    if (rrProbability < 1.0f) {
        indirectContrib.x /= rrProbability;
        indirectContrib.y /= rrProbability;
        indirectContrib.z /= rrProbability;
    }

    // 返回直接光 + 间接光
    return {
        directLight.x + indirectContrib.x,
        directLight.y + indirectContrib.y,
        directLight.z + indirectContrib.z
    };
}

XMFLOAT3 CPathTraceBaker::evaluateDirectLight(
    const XMFLOAT3& hitPos,
    const XMFLOAT3& hitNormal,
    const XMFLOAT3& albedo,
    CScene& scene)
{
    XMFLOAT3 totalLight = {0, 0, 0};
    auto& world = scene.GetWorld();

    // 遍历所有光源
    for (size_t i = 0; i < world.Count(); i++)
    {
        auto* obj = world.Get(i);
        if (!obj) continue;

        auto* transform = obj->GetComponent<STransform>();
        if (!transform) continue;

        // Directional Light
        auto* dirLight = obj->GetComponent<SDirectionalLight>();
        if (dirLight)
        {
            // 使用 GetDirection() 获取方向，然后取反得到指向光源的方向
            XMFLOAT3 dir = dirLight->GetDirection();
            XMFLOAT3 lightDir = {-dir.x, -dir.y, -dir.z};

            // N·L
            float NdotL = hitNormal.x * lightDir.x +
                          hitNormal.y * lightDir.y +
                          hitNormal.z * lightDir.z;

            if (NdotL > 0.0f) {
                // Shadow ray
                XMFLOAT3 shadowOrigin = {
                    hitPos.x + hitNormal.x * 0.001f,
                    hitPos.y + hitNormal.y * 0.001f,
                    hitPos.z + hitNormal.z * 0.001f
                };

                bool inShadow = m_rayTracer->TraceShadowRay(shadowOrigin, lightDir, 1000.0f);

                if (!inShadow) {
                    // Lambertian: L * albedo * N·L / π
                    float intensity = dirLight->intensity;
                    totalLight.x += dirLight->color.x * intensity * albedo.x * NdotL * INV_PI;
                    totalLight.y += dirLight->color.y * intensity * albedo.y * NdotL * INV_PI;
                    totalLight.z += dirLight->color.z * intensity * albedo.z * NdotL * INV_PI;
                }
            }
            continue;
        }

        // Point Light
        auto* pointLight = obj->GetComponent<SPointLight>();
        if (pointLight)
        {
            XMFLOAT3 lightPos = transform->position;
            XMFLOAT3 toLight = {
                lightPos.x - hitPos.x,
                lightPos.y - hitPos.y,
                lightPos.z - hitPos.z
            };

            float dist = std::sqrt(toLight.x * toLight.x + toLight.y * toLight.y + toLight.z * toLight.z);
            if (dist < 0.001f) continue;

            // 归一化
            XMFLOAT3 lightDir = {toLight.x / dist, toLight.y / dist, toLight.z / dist};

            float NdotL = hitNormal.x * lightDir.x +
                          hitNormal.y * lightDir.y +
                          hitNormal.z * lightDir.z;

            if (NdotL > 0.0f) {
                // Shadow ray
                XMFLOAT3 shadowOrigin = {
                    hitPos.x + hitNormal.x * 0.001f,
                    hitPos.y + hitNormal.y * 0.001f,
                    hitPos.z + hitNormal.z * 0.001f
                };

                bool inShadow = m_rayTracer->TraceShadowRay(shadowOrigin, lightDir, dist - 0.001f);

                if (!inShadow) {
                    // 衰减: 1 / (distance^2)
                    float attenuation = 1.0f / (dist * dist);
                    float intensity = pointLight->intensity * attenuation;

                    totalLight.x += pointLight->color.x * intensity * albedo.x * NdotL * INV_PI;
                    totalLight.y += pointLight->color.y * intensity * albedo.y * NdotL * INV_PI;
                    totalLight.z += pointLight->color.z * intensity * albedo.z * NdotL * INV_PI;
                }
            }
            continue;
        }

        // Spot Light
        auto* spotLight = obj->GetComponent<SSpotLight>();
        if (spotLight)
        {
            XMFLOAT3 lightPos = transform->position;
            XMFLOAT3 toLight = {
                lightPos.x - hitPos.x,
                lightPos.y - hitPos.y,
                lightPos.z - hitPos.z
            };

            float dist = std::sqrt(toLight.x * toLight.x + toLight.y * toLight.y + toLight.z * toLight.z);
            if (dist < 0.001f) continue;

            XMFLOAT3 lightDir = {toLight.x / dist, toLight.y / dist, toLight.z / dist};

            // 检查是否在锥体内
            float cosAngle = -(lightDir.x * spotLight->direction.x +
                               lightDir.y * spotLight->direction.y +
                               lightDir.z * spotLight->direction.z);

            // 角度是度数，需要转换为弧度
            float outerCos = std::cos(spotLight->outerConeAngle * PI / 180.0f);
            float innerCos = std::cos(spotLight->innerConeAngle * PI / 180.0f);

            if (cosAngle > outerCos) {
                float NdotL = hitNormal.x * lightDir.x +
                              hitNormal.y * lightDir.y +
                              hitNormal.z * lightDir.z;

                if (NdotL > 0.0f) {
                    // Shadow ray
                    XMFLOAT3 shadowOrigin = {
                        hitPos.x + hitNormal.x * 0.001f,
                        hitPos.y + hitNormal.y * 0.001f,
                        hitPos.z + hitNormal.z * 0.001f
                    };

                    bool inShadow = m_rayTracer->TraceShadowRay(shadowOrigin, lightDir, dist - 0.001f);

                    if (!inShadow) {
                        // 锥体衰减
                        float spotFactor = (cosAngle - outerCos) / (innerCos - outerCos);
                        spotFactor = std::clamp(spotFactor, 0.0f, 1.0f);

                        // 距离衰减
                        float attenuation = 1.0f / (dist * dist);
                        float intensity = spotLight->intensity * attenuation * spotFactor;

                        totalLight.x += spotLight->color.x * intensity * albedo.x * NdotL * INV_PI;
                        totalLight.y += spotLight->color.y * intensity * albedo.y * NdotL * INV_PI;
                        totalLight.z += spotLight->color.z * intensity * albedo.z * NdotL * INV_PI;
                    }
                }
            }
        }
    }

    return totalLight;
}

XMFLOAT3 CPathTraceBaker::sampleSkybox(const XMFLOAT3& direction, CScene& scene)
{
    // 简化版：返回基于方向的渐变色（模拟天空）
    // TODO: 实际采样 HDR skybox
    float skyFactor = direction.y * 0.5f + 0.5f;  // -1~1 -> 0~1

    // 天空蓝 -> 地平线白
    XMFLOAT3 skyColor = {
        0.5f + 0.5f * skyFactor,
        0.7f + 0.3f * skyFactor,
        1.0f
    };

    // 乘以环境光强度
    float ambientIntensity = 0.3f;  // 默认环境光强度

    return {
        skyColor.x * ambientIntensity,
        skyColor.y * ambientIntensity,
        skyColor.z * ambientIntensity
    };
}

// ============================================
// 采样工具
// ============================================

XMFLOAT3 CPathTraceBaker::sampleSphereUniform(float u1, float u2)
{
    // 均匀球面采样
    float z = 1.0f - 2.0f * u1;  // -1 ~ 1
    float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    float phi = 2.0f * PI * u2;

    return {
        r * std::cos(phi),
        r * std::sin(phi),
        z
    };
}

XMFLOAT3 CPathTraceBaker::sampleHemisphereCosine(
    const XMFLOAT3& normal,
    float u1, float u2)
{
    // Cosine-weighted 半球采样（在切线空间）
    float r = std::sqrt(u1);
    float theta = 2.0f * PI * u2;

    float x = r * std::cos(theta);
    float y = r * std::sin(theta);
    float z = std::sqrt(std::max(0.0f, 1.0f - u1));

    // 构建切线空间基
    XMFLOAT3 tangent, bitangent;
    buildTangentBasis(normal, tangent, bitangent);

    // 变换到世界空间
    return {
        x * tangent.x + y * bitangent.x + z * normal.x,
        x * tangent.y + y * bitangent.y + z * normal.y,
        x * tangent.z + y * bitangent.z + z * normal.z
    };
}

void CPathTraceBaker::buildTangentBasis(
    const XMFLOAT3& normal,
    XMFLOAT3& tangent,
    XMFLOAT3& bitangent)
{
    // Frisvad's method for building orthonormal basis
    if (normal.z < -0.9999f) {
        tangent = {0.0f, -1.0f, 0.0f};
        bitangent = {-1.0f, 0.0f, 0.0f};
    } else {
        float a = 1.0f / (1.0f + normal.z);
        float b = -normal.x * normal.y * a;
        tangent = {1.0f - normal.x * normal.x * a, b, -normal.x};
        bitangent = {b, 1.0f - normal.y * normal.y * a, -normal.y};
    }
}

// ============================================
// SH 投影
// ============================================

void CPathTraceBaker::evaluateSHBasis(const XMFLOAT3& dir, float basis[9])
{
    // L0
    basis[0] = 0.282095f;  // Y_0^0 = 0.5 * sqrt(1/π)

    // L1
    basis[1] = 0.488603f * dir.y;   // Y_1^-1
    basis[2] = 0.488603f * dir.z;   // Y_1^0
    basis[3] = 0.488603f * dir.x;   // Y_1^1

    // L2
    basis[4] = 1.092548f * dir.x * dir.y;                       // Y_2^-2
    basis[5] = 1.092548f * dir.y * dir.z;                       // Y_2^-1
    basis[6] = 0.315392f * (3.0f * dir.z * dir.z - 1.0f);       // Y_2^0
    basis[7] = 1.092548f * dir.x * dir.z;                       // Y_2^1
    basis[8] = 0.546274f * (dir.x * dir.x - dir.y * dir.y);     // Y_2^2
}

void CPathTraceBaker::accumulateToSH(
    const XMFLOAT3& direction,
    const XMFLOAT3& radiance,
    float weight,
    std::array<XMFLOAT3, 9>& outSH)
{
    // 计算 SH 基函数
    float basis[9];
    evaluateSHBasis(direction, basis);

    // 累加：SH[i] += radiance * basis[i] * weight
    for (int i = 0; i < 9; i++) {
        float w = basis[i] * weight;
        outSH[i].x += radiance.x * w;
        outSH[i].y += radiance.y * w;
        outSH[i].z += radiance.z * w;
    }
}
