#pragma once
#include <DirectXMath.h>
#include <array>
#include <random>

class CScene;
class CRayTracer;

// ============================================
// Path Tracing 配置
// ============================================
struct SPathTraceConfig
{
    int samplesPerVoxel = 64*32*32*6;       // 每个 voxel 的采样数
    int maxBounces = 3;             // 最大反弹次数（0 = 只有直接光）
    bool useRussianRoulette = true; // 是否使用 Russian Roulette 终止
    float rrStartBounce = 2;        // 从第几次 bounce 开始 RR
    float rrMinProbability = 0.1f;  // RR 最小概率
};

// ============================================
// CPathTraceBaker - Path Tracing 烘焙器
// ============================================
// 使用 Path Tracing 计算每个 voxel 的球谐系数
// 替代原有的 Cubemap 渲染方式
// ============================================
class CPathTraceBaker
{
public:
    CPathTraceBaker() = default;
    ~CPathTraceBaker() = default;

    // ============================================
    // 生命周期
    // ============================================

    // 初始化（构建 BVH）
    bool Initialize(CScene& scene, const SPathTraceConfig& config = SPathTraceConfig());
    void Shutdown();

    // 场景变化后重建 BVH
    void RebuildBVH(CScene& scene);

    // ============================================
    // 烘焙
    // ============================================

    // 烘焙单个 voxel 的 SH 系数
    // position: 世界空间位置
    // scene: 场景（用于获取光源信息）
    // outSH: 输出的 L2 球谐系数（9 个 RGB）
    void BakeVoxel(
        const DirectX::XMFLOAT3& position,
        CScene& scene,
        std::array<DirectX::XMFLOAT3, 9>& outSH
    );

    // ============================================
    // 配置
    // ============================================
    const SPathTraceConfig& GetConfig() const { return m_config; }
    void SetConfig(const SPathTraceConfig& config) { m_config = config; }

    bool IsInitialized() const { return m_initialized; }

private:
    // ============================================
    // Path Tracing 核心
    // ============================================

    // 追踪一条路径，返回入射辐射度
    // origin: 起点
    // direction: 方向（归一化）
    // depth: 当前递归深度
    DirectX::XMFLOAT3 traceRadiance(
        const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction,
        CScene& scene,
        int depth
    );

    // 计算命中点的直接光照
    DirectX::XMFLOAT3 evaluateDirectLight(
        const DirectX::XMFLOAT3& hitPos,
        const DirectX::XMFLOAT3& hitNormal,
        const DirectX::XMFLOAT3& albedo,
        CScene& scene
    );

    // 采样天空盒/环境光
    DirectX::XMFLOAT3 sampleSkybox(
        const DirectX::XMFLOAT3& direction,
        CScene& scene
    );

    // ============================================
    // 采样工具
    // ============================================

    // 生成 Cosine-weighted 半球采样方向
    DirectX::XMFLOAT3 sampleHemisphereCosine(
        const DirectX::XMFLOAT3& normal,
        float u1, float u2
    );

    // 生成均匀球面采样方向
    DirectX::XMFLOAT3 sampleSphereUniform(float u1, float u2);

    // 构建切线空间基
    void buildTangentBasis(
        const DirectX::XMFLOAT3& normal,
        DirectX::XMFLOAT3& tangent,
        DirectX::XMFLOAT3& bitangent
    );

    // ============================================
    // SH 投影
    // ============================================

    // 将方向和辐射度累加到 SH 系数
    void accumulateToSH(
        const DirectX::XMFLOAT3& direction,
        const DirectX::XMFLOAT3& radiance,
        float weight,
        std::array<DirectX::XMFLOAT3, 9>& outSH
    );

    // 计算 SH 基函数
    void evaluateSHBasis(const DirectX::XMFLOAT3& dir, float basis[9]);

private:
    bool m_initialized = false;
    SPathTraceConfig m_config;

    // Ray Tracer
    CRayTracer* m_rayTracer = nullptr;

    // 随机数生成器（每个线程一个，这里简化为单线程）
    std::mt19937 m_rng;
    std::uniform_real_distribution<float> m_dist{0.0f, 1.0f};

    float random() { return m_dist(m_rng); }
};
