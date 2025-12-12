#pragma once
#include <DirectXMath.h>
#include <array>
#include <random>
#include <vector>
#include <string>
#include "Core/Loader/KTXLoader.h"

class CScene;
class CRayTracer;

// ============================================
// Path Tracing Config
// ============================================
struct SPathTraceConfig
{
    int samplesPerVoxel = 64;
    int maxBounces = 3;
    bool useRussianRoulette = true;
    float rrStartBounce = 2;
    float rrMinProbability = 0.1f;

    // Validity detection: probe is invalid if hit ratio > threshold
    // (hit ratio = geometry hits / total samples)
    float validityHitThreshold = 0.75f;  // 75% hit = inside geometry

    // Debug: export cubemap at position
    bool debugExportCubemap = false;
    DirectX::XMFLOAT3 debugExportPosition = {10, 10, 10};
    float debugExportRadius = 1.0f;
    int debugCubemapResolution = 64;
    std::string debugExportPath = "";
};

// ============================================
// Bake Result - includes validity info
// ============================================
struct SBakeResult
{
    std::array<DirectX::XMFLOAT3, 9> sh;  // SH coefficients
    bool isValid = true;                    // false if probe is inside geometry
    float hitRatio = 0.0f;                  // geometry hit ratio (0-1)
};

// ============================================
// CPathTraceBaker - Path Tracing Baker
// ============================================
class CPathTraceBaker
{
public:
    CPathTraceBaker() = default;
    ~CPathTraceBaker() = default;

    // Lifecycle
    bool Initialize(CScene& scene, const SPathTraceConfig& config = SPathTraceConfig());
    void Shutdown();
    void RebuildBVH(CScene& scene);

    // Baking (legacy, does not return validity)
    void BakeVoxel(
        const DirectX::XMFLOAT3& position,
        CScene& scene,
        std::array<DirectX::XMFLOAT3, 9>& outSH
    );

    // Baking with validity detection
    SBakeResult BakeVoxelWithValidity(
        const DirectX::XMFLOAT3& position,
        CScene& scene
    );

    // Config
    const SPathTraceConfig& GetConfig() const { return m_config; }
    void SetConfig(const SPathTraceConfig& config) { m_config = config; }

    bool IsInitialized() const { return m_initialized; }
    bool HasSkybox() const { return m_skyboxData.valid; }

private:
    // Path Tracing Core
    DirectX::XMFLOAT3 traceRadiance(
        const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction,
        CScene& scene,
        int depth,
        bool& outHitGeometry  // output: did ray hit geometry?
    );

    DirectX::XMFLOAT3 evaluateDirectLight(
        const DirectX::XMFLOAT3& hitPos,
        const DirectX::XMFLOAT3& hitNormal,
        const DirectX::XMFLOAT3& albedo,
        CScene& scene
    );

    DirectX::XMFLOAT3 sampleSkybox(const DirectX::XMFLOAT3& direction);

    // Skybox Sampling
    bool loadSkyboxToCPU(CScene& scene);
    void directionToCubemapUV(const DirectX::XMFLOAT3& dir, int& face, float& u, float& v);
    DirectX::XMFLOAT3 sampleCubemapFace(int face, float u, float v);

    // Debug Cubemap Export
    bool shouldExportDebugCubemap(const DirectX::XMFLOAT3& position);
    void exportDebugCubemapFromSamples(
        const DirectX::XMFLOAT3& position,
        const std::vector<std::vector<DirectX::XMFLOAT3>>& cubemapAccum,
        const std::vector<std::vector<float>>& cubemapWeights
    );

    // Sampling Utils
    DirectX::XMFLOAT3 sampleHemisphereCosine(
        const DirectX::XMFLOAT3& normal,
        float u1, float u2
    );
    DirectX::XMFLOAT3 sampleSphereUniform(float u1, float u2);
    void buildTangentBasis(
        const DirectX::XMFLOAT3& normal,
        DirectX::XMFLOAT3& tangent,
        DirectX::XMFLOAT3& bitangent
    );

    // SH Projection
    void accumulateToSH(
        const DirectX::XMFLOAT3& direction,
        const DirectX::XMFLOAT3& radiance,
        float weight,
        std::array<DirectX::XMFLOAT3, 9>& outSH
    );
    void evaluateSHBasis(const DirectX::XMFLOAT3& dir, float basis[9]);

private:
    bool m_initialized = false;
    SPathTraceConfig m_config;

    CRayTracer* m_rayTracer = nullptr;
    CKTXLoader::SCubemapCPUData m_skyboxData;
    bool m_debugCubemapExported = false;

    std::mt19937 m_rng;
    std::uniform_real_distribution<float> m_dist{0.0f, 1.0f};

    float random() { return m_dist(m_rng); }
};
