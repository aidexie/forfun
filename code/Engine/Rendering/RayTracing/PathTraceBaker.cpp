#include "PathTraceBaker.h"
#include "RayTracer.h"
#include "Engine/Scene.h"
#include "Engine/SceneLightSettings.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Components/PointLight.h"
#include "Engine/Components/SpotLight.h"
#include "Core/FFLog.h"
#include "Core/Loader/FFAssetLoader.h"
#include "Core/PathManager.h"
#include <cmath>
#include <fstream>
#include <ktx.h>

using namespace DirectX;

// ============================================
// Constants
// ============================================
static const float PI = 3.14159265358979323846f;
static const float INV_PI = 1.0f / PI;

// ============================================
// Lifecycle
// ============================================

bool CPathTraceBaker::Initialize(CScene& scene, const SPathTraceConfig& config)
{
    if (m_initialized) {
        Shutdown();
    }

    m_config = config;
    m_debugCubemapExported = false;

    // Create Ray Tracer
    m_rayTracer = new CRayTracer();
    if (!m_rayTracer->Initialize(scene)) {
        CFFLog::Error("[PathTraceBaker] Failed to initialize RayTracer");
        delete m_rayTracer;
        m_rayTracer = nullptr;
        return false;
    }

    // Load skybox to CPU
    if (!loadSkyboxToCPU(scene)) {
        CFFLog::Warning("[PathTraceBaker] Failed to load skybox, using fallback gradient");
    }

    // Initialize random number generator
    std::random_device rd;
    m_rng.seed(rd());

    m_initialized = true;
    CFFLog::Info("[PathTraceBaker] Initialized: samples=%d, bounces=%d, RR=%s, skybox=%s",
                 m_config.samplesPerVoxel, m_config.maxBounces,
                 m_config.useRussianRoulette ? "on" : "off",
                 m_skyboxData.valid ? "loaded" : "fallback");

    return true;
}

void CPathTraceBaker::Shutdown()
{
    if (m_rayTracer) {
        m_rayTracer->Shutdown();
        delete m_rayTracer;
        m_rayTracer = nullptr;
    }

    // Clear skybox data
    m_skyboxData.valid = false;
    m_skyboxData.size = 0;
    for (int i = 0; i < 6; i++) {
        m_skyboxData.faces[i].clear();
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
// Skybox Loading
// ============================================

bool CPathTraceBaker::loadSkyboxToCPU(CScene& scene)
{
    const auto& lightSettings = scene.GetLightSettings();
    const std::string& skyboxAssetPath = lightSettings.skyboxAssetPath;

    if (skyboxAssetPath.empty()) {
        CFFLog::Warning("[PathTraceBaker] No skybox asset path configured");
        return false;
    }

    CFFAssetLoader::SkyboxAsset skyboxAsset;
    std::string fullAssetPath = FFPath::GetAbsolutePath(skyboxAssetPath);
    if (!CFFAssetLoader::LoadSkyboxAsset(fullAssetPath, skyboxAsset)) {
        CFFLog::Error("[PathTraceBaker] Failed to load skybox asset: %s", fullAssetPath.c_str());
        return false;
    }

    std::string ktx2Path = FFPath::GetAbsolutePath(skyboxAsset.envPath);
    if (!CKTXLoader::LoadCubemapToCPU(ktx2Path, m_skyboxData)) {
        CFFLog::Error("[PathTraceBaker] Failed to load skybox cubemap: %s", ktx2Path.c_str());
        return false;
    }

    CFFLog::Info("[PathTraceBaker] Loaded skybox cubemap: %s (%dx%d)",
                 ktx2Path.c_str(), m_skyboxData.size, m_skyboxData.size);
    return true;
}

// ============================================
// Baking
// ============================================

void CPathTraceBaker::BakeVoxel(
    const XMFLOAT3& position,
    CScene& scene,
    std::array<XMFLOAT3, 9>& outSH)
{
    // Legacy interface - just call new function and ignore validity
    SBakeResult result = BakeVoxelWithValidity(position, scene);
    outSH = result.sh;
}

SBakeResult CPathTraceBaker::BakeVoxelWithValidity(
    const XMFLOAT3& position,
    CScene& scene)
{
    SBakeResult result;
    for (auto& coeff : result.sh) {
        coeff = {0, 0, 0};
    }
    result.isValid = true;
    result.hitRatio = 0.0f;

    if (!m_initialized || !m_rayTracer) {
        return result;
    }

    // Check if we need to export debug cubemap
    bool exportCubemap = shouldExportDebugCubemap(position);

    // Debug cubemap data
    int cubemapRes = m_config.debugCubemapResolution;
    std::vector<std::vector<XMFLOAT3>> cubemapAccum;
    std::vector<std::vector<float>> cubemapWeights;

    if (exportCubemap) {
        cubemapAccum.resize(6);
        cubemapWeights.resize(6);
        for (int f = 0; f < 6; f++) {
            cubemapAccum[f].resize(cubemapRes * cubemapRes, {0, 0, 0});
            cubemapWeights[f].resize(cubemapRes * cubemapRes, 0.0f);
        }
    }

    int numSamples = m_config.samplesPerVoxel;
    int hitCount = 0;  // Count geometry hits for validity detection

    for (int s = 0; s < numSamples; s++)
    {
        float u1 = random();
        float u2 = random();
        XMFLOAT3 direction = sampleSphereUniform(u1, u2);

        bool hitGeometry = false;
        XMFLOAT3 radiance = traceRadiance(position, direction, scene, 0, hitGeometry);

        if (hitGeometry) {
            hitCount++;
        }

        float weight = 4.0f * PI / numSamples;
        accumulateToSH(direction, radiance, weight, result.sh);

        // Accumulate to debug cubemap if needed
        if (exportCubemap) {
            int face;
            float u, v;
            directionToCubemapUV(direction, face, u, v);

            int px = std::clamp((int)(u * cubemapRes), 0, cubemapRes - 1);
            int py = std::clamp((int)(v * cubemapRes), 0, cubemapRes - 1);
            int idx = py * cubemapRes + px;

            cubemapAccum[face][idx].x += radiance.x;
            cubemapAccum[face][idx].y += radiance.y;
            cubemapAccum[face][idx].z += radiance.z;
            cubemapWeights[face][idx] += 1.0f;
        }
    }

    // Calculate validity
    result.hitRatio = (float)hitCount / (float)numSamples;
    result.isValid = (result.hitRatio < m_config.validityHitThreshold);

    // Export debug cubemap
    if (exportCubemap) {
        exportDebugCubemapFromSamples(position, cubemapAccum, cubemapWeights);
    }

    return result;
}

// ============================================
// Path Tracing Core
// ============================================

XMFLOAT3 CPathTraceBaker::traceRadiance(
    const XMFLOAT3& origin,
    const XMFLOAT3& direction,
    CScene& scene,
    int depth,
    bool& outHitGeometry)
{
    outHitGeometry = false;

    if (depth > m_config.maxBounces) {
        return {0, 0, 0};
    }

    SRayHit hit = m_rayTracer->TraceRay(origin, direction);

    if (!hit.valid) {
        return sampleSkybox(direction);
    }

    // First bounce hit geometry - record for validity detection
    if (depth == 0) {
        outHitGeometry = true;
    }

    XMFLOAT3 directLight = evaluateDirectLight(hit.position, hit.normal, hit.albedo, scene);

    if (depth >= m_config.maxBounces) {
        return directLight;
    }

    float rrProbability = 1.0f;
    if (m_config.useRussianRoulette && depth >= (int)m_config.rrStartBounce) {
        rrProbability = std::max({hit.albedo.x, hit.albedo.y, hit.albedo.z});
        rrProbability = std::max(rrProbability, m_config.rrMinProbability);

        if (random() > rrProbability) {
            return directLight;
        }
    }

    float u1 = random();
    float u2 = random();
    XMFLOAT3 bounceDir = sampleHemisphereCosine(hit.normal, u1, u2);

    XMFLOAT3 bounceOrigin = {
        hit.position.x + hit.normal.x * 0.001f,
        hit.position.y + hit.normal.y * 0.001f,
        hit.position.z + hit.normal.z * 0.001f
    };

    bool dummyHit;  // We don't care about hits after first bounce
    XMFLOAT3 indirectRadiance = traceRadiance(bounceOrigin, bounceDir, scene, depth + 1, dummyHit);

    XMFLOAT3 indirectContrib = {
        indirectRadiance.x * hit.albedo.x,
        indirectRadiance.y * hit.albedo.y,
        indirectRadiance.z * hit.albedo.z
    };

    if (rrProbability < 1.0f) {
        indirectContrib.x /= rrProbability;
        indirectContrib.y /= rrProbability;
        indirectContrib.z /= rrProbability;
    }

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
            XMFLOAT3 dir = dirLight->GetDirection();
            XMFLOAT3 lightDir = {-dir.x, -dir.y, -dir.z};

            float NdotL = hitNormal.x * lightDir.x +
                          hitNormal.y * lightDir.y +
                          hitNormal.z * lightDir.z;

            if (NdotL > 0.0f) {
                XMFLOAT3 shadowOrigin = {
                    hitPos.x + hitNormal.x * 0.001f,
                    hitPos.y + hitNormal.y * 0.001f,
                    hitPos.z + hitNormal.z * 0.001f
                };

                bool inShadow = m_rayTracer->TraceShadowRay(shadowOrigin, lightDir, 1000.0f);

                if (!inShadow) {
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

            XMFLOAT3 lightDir = {toLight.x / dist, toLight.y / dist, toLight.z / dist};

            float NdotL = hitNormal.x * lightDir.x +
                          hitNormal.y * lightDir.y +
                          hitNormal.z * lightDir.z;

            if (NdotL > 0.0f) {
                XMFLOAT3 shadowOrigin = {
                    hitPos.x + hitNormal.x * 0.001f,
                    hitPos.y + hitNormal.y * 0.001f,
                    hitPos.z + hitNormal.z * 0.001f
                };

                bool inShadow = m_rayTracer->TraceShadowRay(shadowOrigin, lightDir, dist - 0.001f);

                if (!inShadow) {
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

            float cosAngle = -(lightDir.x * spotLight->direction.x +
                               lightDir.y * spotLight->direction.y +
                               lightDir.z * spotLight->direction.z);

            float outerCos = std::cos(spotLight->outerConeAngle * PI / 180.0f);
            float innerCos = std::cos(spotLight->innerConeAngle * PI / 180.0f);

            if (cosAngle > outerCos) {
                float NdotL = hitNormal.x * lightDir.x +
                              hitNormal.y * lightDir.y +
                              hitNormal.z * lightDir.z;

                if (NdotL > 0.0f) {
                    XMFLOAT3 shadowOrigin = {
                        hitPos.x + hitNormal.x * 0.001f,
                        hitPos.y + hitNormal.y * 0.001f,
                        hitPos.z + hitNormal.z * 0.001f
                    };

                    bool inShadow = m_rayTracer->TraceShadowRay(shadowOrigin, lightDir, dist - 0.001f);

                    if (!inShadow) {
                        float spotFactor = (cosAngle - outerCos) / (innerCos - outerCos);
                        spotFactor = std::clamp(spotFactor, 0.0f, 1.0f);

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

// ============================================
// Skybox Sampling
// ============================================

XMFLOAT3 CPathTraceBaker::sampleSkybox(const XMFLOAT3& direction)
{
    if (!m_skyboxData.valid) {
        float skyFactor = direction.y * 0.5f + 0.5f;
        XMFLOAT3 skyColor = {
            0.5f + 0.5f * skyFactor,
            0.7f + 0.3f * skyFactor,
            1.0f
        };
        float ambientIntensity = 0.3f;
        return {
            skyColor.x * ambientIntensity,
            skyColor.y * ambientIntensity,
            skyColor.z * ambientIntensity
        };
    }

    int face;
    float u, v;
    directionToCubemapUV(direction, face, u, v);

    XMFLOAT3 color = sampleCubemapFace(face, u, v);
    return color;
}

void CPathTraceBaker::directionToCubemapUV(const XMFLOAT3& dir, int& face, float& u, float& v)
{
    float absX = std::abs(dir.x);
    float absY = std::abs(dir.y);
    float absZ = std::abs(dir.z);

    float ma, sc, tc;

    if (absX >= absY && absX >= absZ) {
        ma = absX;
        if (dir.x > 0) {
            face = 0;
            sc = -dir.z;
            tc = -dir.y;
        } else {
            face = 1;
            sc = dir.z;
            tc = -dir.y;
        }
    } else if (absY >= absX && absY >= absZ) {
        ma = absY;
        if (dir.y > 0) {
            face = 2;
            sc = dir.x;
            tc = dir.z;
        } else {
            face = 3;
            sc = dir.x;
            tc = -dir.z;
        }
    } else {
        ma = absZ;
        if (dir.z > 0) {
            face = 4;
            sc = dir.x;
            tc = -dir.y;
        } else {
            face = 5;
            sc = -dir.x;
            tc = -dir.y;
        }
    }

    u = 0.5f * (sc / ma + 1.0f);
    v = 0.5f * (tc / ma + 1.0f);

    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
}

XMFLOAT3 CPathTraceBaker::sampleCubemapFace(int face, float u, float v)
{
    if (face < 0 || face >= 6 || m_skyboxData.faces[face].empty()) {
        return {0, 0, 0};
    }

    int size = m_skyboxData.size;

    float fx = u * (size - 1);
    float fy = v * (size - 1);

    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = std::min(x0 + 1, size - 1);
    int y1 = std::min(y0 + 1, size - 1);

    float dx = fx - x0;
    float dy = fy - y0;

    const auto& faceData = m_skyboxData.faces[face];
    const XMFLOAT4& p00 = faceData[y0 * size + x0];
    const XMFLOAT4& p10 = faceData[y0 * size + x1];
    const XMFLOAT4& p01 = faceData[y1 * size + x0];
    const XMFLOAT4& p11 = faceData[y1 * size + x1];

    float r = (1-dx)*(1-dy)*p00.x + dx*(1-dy)*p10.x + (1-dx)*dy*p01.x + dx*dy*p11.x;
    float g = (1-dx)*(1-dy)*p00.y + dx*(1-dy)*p10.y + (1-dx)*dy*p01.y + dx*dy*p11.y;
    float b = (1-dx)*(1-dy)*p00.z + dx*(1-dy)*p10.z + (1-dx)*dy*p01.z + dx*dy*p11.z;

    return {r, g, b};
}

// ============================================
// Debug Cubemap Export
// ============================================

bool CPathTraceBaker::shouldExportDebugCubemap(const XMFLOAT3& position)
{
    if (!m_config.debugExportCubemap || m_debugCubemapExported) {
        return false;
    }

    float dx = position.x - m_config.debugExportPosition.x;
    float dy = position.y - m_config.debugExportPosition.y;
    float dz = position.z - m_config.debugExportPosition.z;
    float distSq = dx*dx + dy*dy + dz*dz;

    return distSq <= m_config.debugExportRadius * m_config.debugExportRadius;
}

void CPathTraceBaker::exportDebugCubemapFromSamples(
    const XMFLOAT3& position,
    const std::vector<std::vector<XMFLOAT3>>& cubemapAccum,
    const std::vector<std::vector<float>>& cubemapWeights)
{
    m_debugCubemapExported = true;

    int resolution = m_config.debugCubemapResolution;
    CFFLog::Info("[PathTraceBaker] Exporting debug cubemap from samples at (%.2f, %.2f, %.2f), resolution=%d",
                 position.x, position.y, position.z, resolution);

    // Helper: float to half-float
    auto floatToHalf = [](float f) -> uint16_t {
        uint32_t x = *reinterpret_cast<uint32_t*>(&f);
        uint32_t sign = (x >> 16) & 0x8000;
        int exp = ((x >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = (x >> 13) & 0x3FF;

        if (exp <= 0) {
            return (uint16_t)sign;
        } else if (exp >= 31) {
            return (uint16_t)(sign | 0x7C00);
        }
        return (uint16_t)(sign | (exp << 10) | mant);
    };

    // 准备 KTX2 数据
    std::vector<std::vector<uint16_t>> faceData(6);
    for (int f = 0; f < 6; f++) {
        faceData[f].resize(resolution * resolution * 4);

        for (int i = 0; i < resolution * resolution; i++) {
            XMFLOAT3 avgRadiance = {0, 0, 0};
            float w = cubemapWeights[f][i];

            if (w > 0.0f) {
                // 计算平均 radiance
                avgRadiance.x = cubemapAccum[f][i].x / w;
                avgRadiance.y = cubemapAccum[f][i].y / w;
                avgRadiance.z = cubemapAccum[f][i].z / w;
            }

            int idx = i * 4;
            faceData[f][idx + 0] = floatToHalf(avgRadiance.x);
            faceData[f][idx + 1] = floatToHalf(avgRadiance.y);
            faceData[f][idx + 2] = floatToHalf(avgRadiance.z);
            faceData[f][idx + 3] = floatToHalf(1.0f);
        }
    }

    // 导出路径
    std::string exportPath = m_config.debugExportPath;
    if (exportPath.empty()) {
        exportPath = FFPath::GetDebugDir() + "/debug_cubemap.ktx2";
    }

    // 创建 KTX2 文件
    ktxTextureCreateInfo createInfo = {};
    createInfo.glInternalformat = 0;
    createInfo.vkFormat = 97;  // VK_FORMAT_R16G16B16A16_SFLOAT
    createInfo.baseWidth = resolution;
    createInfo.baseHeight = resolution;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1;
    createInfo.numLayers = 1;
    createInfo.numFaces = 6;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("[PathTraceBaker] Failed to create KTX2 texture: %d", result);
        return;
    }

    // 写入 face 数据
    for (int f = 0; f < 6; f++) {
        size_t offset;
        result = ktxTexture_GetImageOffset(ktxTexture(ktxTex), 0, 0, f, &offset);
        if (result != KTX_SUCCESS) {
            CFFLog::Error("[PathTraceBaker] Failed to get image offset for face %d", f);
            ktxTexture2_Destroy(ktxTex);
            return;
        }

        memcpy(ktxTex->pData + offset, faceData[f].data(), faceData[f].size() * sizeof(uint16_t));
    }

    // 写入文件
    result = ktxTexture_WriteToNamedFile(ktxTexture(ktxTex), exportPath.c_str());
    ktxTexture2_Destroy(ktxTex);

    if (result != KTX_SUCCESS) {
        CFFLog::Error("[PathTraceBaker] Failed to write KTX2 file: %s", exportPath.c_str());
    } else {
        CFFLog::Info("[PathTraceBaker] Exported debug cubemap to: %s", exportPath.c_str());
    }
}

// ============================================
// Sampling Utils
// ============================================

XMFLOAT3 CPathTraceBaker::sampleSphereUniform(float u1, float u2)
{
    float z = 1.0f - 2.0f * u1;
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
    float r = std::sqrt(u1);
    float theta = 2.0f * PI * u2;

    float x = r * std::cos(theta);
    float y = r * std::sin(theta);
    float z = std::sqrt(std::max(0.0f, 1.0f - u1));

    XMFLOAT3 tangent, bitangent;
    buildTangentBasis(normal, tangent, bitangent);

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
// SH Projection
// ============================================

void CPathTraceBaker::evaluateSHBasis(const XMFLOAT3& dir, float basis[9])
{
    basis[0] = 0.282095f;

    basis[1] = 0.488603f * dir.y;
    basis[2] = 0.488603f * dir.z;
    basis[3] = 0.488603f * dir.x;

    basis[4] = 1.092548f * dir.x * dir.y;
    basis[5] = 1.092548f * dir.y * dir.z;
    basis[6] = 0.315392f * (3.0f * dir.z * dir.z - 1.0f);
    basis[7] = 1.092548f * dir.x * dir.z;
    basis[8] = 0.546274f * (dir.x * dir.x - dir.y * dir.y);
}

void CPathTraceBaker::accumulateToSH(
    const XMFLOAT3& direction,
    const XMFLOAT3& radiance,
    float weight,
    std::array<XMFLOAT3, 9>& outSH)
{
    float basis[9];
    evaluateSHBasis(direction, basis);

    for (int i = 0; i < 9; i++) {
        float w = basis[i] * weight;
        outSH[i].x += radiance.x * w;
        outSH[i].y += radiance.y * w;
        outSH[i].z += radiance.z * w;
    }
}
