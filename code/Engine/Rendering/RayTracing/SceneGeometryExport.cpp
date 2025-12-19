#include "SceneGeometryExport.h"
#include "../../Scene.h"
#include "../../Components/Transform.h"
#include "../../Components/MeshRenderer.h"
#include "../../Components/DirectionalLight.h"
#include "../../Components/PointLight.h"
#include "../../Components/SpotLight.h"
#include "../../../Core/GpuMeshResource.h"
#include "../../../Core/Mesh.h"
#include "../../../Core/FFLog.h"
#include "../../../Core/MaterialAsset.h"
#include "../../../Core/MaterialManager.h"
#include <algorithm>
#include <unordered_map>

// ============================================
// CRayTracingMeshCache Implementation
// ============================================

CRayTracingMeshCache& CRayTracingMeshCache::Instance() {
    static CRayTracingMeshCache instance;
    return instance;
}

void CRayTracingMeshCache::StoreMeshData(const std::string& path, uint32_t subMeshIndex, const SRayTracingMeshData& data) {
    std::string key = MakeKey(path, subMeshIndex);
    m_cache[key] = data;
}

const SRayTracingMeshData* CRayTracingMeshCache::GetMeshData(const std::string& path, uint32_t subMeshIndex) const {
    std::string key = MakeKey(path, subMeshIndex);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return &it->second;
    }
    return nullptr;
}

bool CRayTracingMeshCache::HasMeshData(const std::string& path, uint32_t subMeshIndex) const {
    std::string key = MakeKey(path, subMeshIndex);
    return m_cache.find(key) != m_cache.end();
}

void CRayTracingMeshCache::Clear() {
    m_cache.clear();
}

// ============================================
// CSceneGeometryExporter Implementation
// ============================================

bool CSceneGeometryExporter::ExtractPositionsFromVertices(
    const void* vertexData,
    uint32_t vertexCount,
    uint32_t vertexStride,
    std::vector<DirectX::XMFLOAT3>& outPositions)
{
    if (!vertexData || vertexCount == 0) {
        return false;
    }

    outPositions.resize(vertexCount);

    const uint8_t* ptr = static_cast<const uint8_t*>(vertexData);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        // Assume position is at offset 0 (px, py, pz as floats)
        const float* pos = reinterpret_cast<const float*>(ptr);
        outPositions[i] = DirectX::XMFLOAT3(pos[0], pos[1], pos[2]);
        ptr += vertexStride;
    }

    return true;
}

bool CSceneGeometryExporter::ExportMesh(
    const GpuMeshResource& meshResource,
    const std::string& path,
    SRayTracingMeshData& outData)
{
    // Check if we have cached RT data
    // Note: The mesh cache should be populated during mesh loading
    // For now, we can't extract data from GPU-only buffers

    outData.sourcePath = path;
    outData.boundsMin = meshResource.localBoundsMin;
    outData.boundsMax = meshResource.localBoundsMax;

    // If positions/indices are not available, return false
    // The caller should ensure mesh data is cached via CRayTracingMeshCache
    CFFLog::Warning("[SceneGeometryExport] ExportMesh: GPU-only mesh data not supported yet. "
                   "Mesh data should be cached during loading via CRayTracingMeshCache.");
    return false;
}

std::unique_ptr<SRayTracingSceneData> CSceneGeometryExporter::ExportScene(CScene& scene) {
    auto result = std::make_unique<SRayTracingSceneData>();

    CWorld& world = scene.GetWorld();
    auto& meshCache = CRayTracingMeshCache::Instance();

    // Track unique meshes (path:subMeshIndex -> meshIndex)
    std::unordered_map<std::string, uint32_t> meshIndexMap;

    // Initialize scene bounds
    result->sceneBoundsMin = DirectX::XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    result->sceneBoundsMax = DirectX::XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // Iterate all game objects
    for (size_t i = 0; i < world.Count(); ++i) {
        CGameObject* go = world.Get(i);
        if (!go) continue;

        // Get transform
        STransform* transform = go->GetComponent<STransform>();
        if (!transform) continue;

        // Get mesh renderer
        SMeshRenderer* meshRenderer = go->GetComponent<SMeshRenderer>();
        if (meshRenderer && !meshRenderer->path.empty()) {
            // Ensure mesh is loaded
            meshRenderer->EnsureUploaded();

            // Process each sub-mesh
            for (uint32_t subMeshIdx = 0; subMeshIdx < meshRenderer->meshes.size(); ++subMeshIdx) {
                auto& gpuMesh = meshRenderer->meshes[subMeshIdx];
                if (!gpuMesh) continue;

                std::string meshKey = meshRenderer->path + ":" + std::to_string(subMeshIdx);

                // Check if this mesh is already added
                auto meshIt = meshIndexMap.find(meshKey);
                uint32_t meshIndex;

                if (meshIt == meshIndexMap.end()) {
                    // New mesh - check if we have cached RT data
                    const SRayTracingMeshData* cachedData = meshCache.GetMeshData(meshRenderer->path, subMeshIdx);

                    if (cachedData && !cachedData->positions.empty()) {
                        // Use cached data
                        meshIndex = static_cast<uint32_t>(result->meshes.size());
                        result->meshes.push_back(*cachedData);
                        meshIndexMap[meshKey] = meshIndex;

                        result->totalVertices += cachedData->vertexCount;
                        result->totalTriangles += cachedData->indexCount / 3;
                    } else {
                        // No cached data - skip this mesh
                        CFFLog::Warning("[SceneGeometryExport] No RT mesh data for: %s (subMesh %u). "
                                       "Mesh will be excluded from ray tracing.",
                                       meshRenderer->path.c_str(), subMeshIdx);
                        continue;
                    }
                } else {
                    meshIndex = meshIt->second;
                }

                // Create instance
                SRayTracingInstance instance;
                DirectX::XMStoreFloat4x4(&instance.worldTransform, transform->WorldMatrix());
                instance.meshIndex = meshIndex;
                instance.instanceID = static_cast<uint32_t>(result->instances.size());
                instance.instanceMask = 0xFF;

                // Get material from MeshRenderer's materialPath
                CMaterialAsset* materialAsset = nullptr;
                if (!meshRenderer->materialPath.empty()) {
                    materialAsset = CMaterialManager::Instance().Load(meshRenderer->materialPath);
                }
                if (!materialAsset) {
                    materialAsset = CMaterialManager::Instance().GetDefault();
                }

                // Find or add material
                SRayTracingMaterial rtMat;
                rtMat.albedo = materialAsset->albedo;
                rtMat.metallic = materialAsset->metallic;
                rtMat.roughness = materialAsset->roughness;

                instance.materialIndex = static_cast<uint32_t>(result->materials.size());
                result->materials.push_back(rtMat);

                result->instances.push_back(instance);

                // Update scene bounds (transform mesh bounds to world space)
                const auto& meshData = result->meshes[meshIndex];
                DirectX::XMMATRIX worldMat = transform->WorldMatrix();

                // Transform the 8 corners of the AABB
                DirectX::XMFLOAT3 corners[8] = {
                    {meshData.boundsMin.x, meshData.boundsMin.y, meshData.boundsMin.z},
                    {meshData.boundsMax.x, meshData.boundsMin.y, meshData.boundsMin.z},
                    {meshData.boundsMin.x, meshData.boundsMax.y, meshData.boundsMin.z},
                    {meshData.boundsMax.x, meshData.boundsMax.y, meshData.boundsMin.z},
                    {meshData.boundsMin.x, meshData.boundsMin.y, meshData.boundsMax.z},
                    {meshData.boundsMax.x, meshData.boundsMin.y, meshData.boundsMax.z},
                    {meshData.boundsMin.x, meshData.boundsMax.y, meshData.boundsMax.z},
                    {meshData.boundsMax.x, meshData.boundsMax.y, meshData.boundsMax.z}
                };

                for (int c = 0; c < 8; ++c) {
                    DirectX::XMVECTOR corner = DirectX::XMLoadFloat3(&corners[c]);
                    corner = DirectX::XMVector3Transform(corner, worldMat);
                    DirectX::XMFLOAT3 worldCorner;
                    DirectX::XMStoreFloat3(&worldCorner, corner);

                    result->sceneBoundsMin.x = std::min(result->sceneBoundsMin.x, worldCorner.x);
                    result->sceneBoundsMin.y = std::min(result->sceneBoundsMin.y, worldCorner.y);
                    result->sceneBoundsMin.z = std::min(result->sceneBoundsMin.z, worldCorner.z);
                    result->sceneBoundsMax.x = std::max(result->sceneBoundsMax.x, worldCorner.x);
                    result->sceneBoundsMax.y = std::max(result->sceneBoundsMax.y, worldCorner.y);
                    result->sceneBoundsMax.z = std::max(result->sceneBoundsMax.z, worldCorner.z);
                }
            }
        }

        // Export lights
        SDirectionalLight* dirLight = go->GetComponent<SDirectionalLight>();
        if (dirLight) {
            SRayTracingLight light;
            light.type = SRayTracingLight::EType::Directional;
            light.direction = dirLight->GetDirection();
            light.color = dirLight->color;
            light.intensity = dirLight->intensity;
            result->lights.push_back(light);
        }

        SPointLight* pointLight = go->GetComponent<SPointLight>();
        if (pointLight) {
            SRayTracingLight light;
            light.type = SRayTracingLight::EType::Point;
            light.position = transform->position;
            light.color = pointLight->color;
            light.intensity = pointLight->intensity;
            light.range = pointLight->range;
            result->lights.push_back(light);
        }

        SSpotLight* spotLight = go->GetComponent<SSpotLight>();
        if (spotLight) {
            SRayTracingLight light;
            light.type = SRayTracingLight::EType::Spot;
            light.position = transform->position;
            // Use spot light's local direction transformed to world space
            DirectX::XMMATRIX rotMat = transform->GetRotationMatrix();
            DirectX::XMVECTOR localDir = DirectX::XMLoadFloat3(&spotLight->direction);
            DirectX::XMVECTOR worldDir = DirectX::XMVector3TransformNormal(localDir, rotMat);
            DirectX::XMStoreFloat3(&light.direction, worldDir);
            light.color = spotLight->color;
            light.intensity = spotLight->intensity;
            light.range = spotLight->range;
            light.spotAngle = spotLight->outerConeAngle;
            result->lights.push_back(light);
        }
    }

    // Default scene bounds if empty
    if (result->instances.empty()) {
        result->sceneBoundsMin = DirectX::XMFLOAT3(-10, -10, -10);
        result->sceneBoundsMax = DirectX::XMFLOAT3(10, 10, 10);
    }

    CFFLog::Info("[SceneGeometryExport] Exported scene: %zu meshes, %zu instances, %zu materials, %zu lights",
                 result->meshes.size(), result->instances.size(),
                 result->materials.size(), result->lights.size());
    CFFLog::Info("[SceneGeometryExport] Total: %u vertices, %u triangles",
                 result->totalVertices, result->totalTriangles);

    return result;
}
