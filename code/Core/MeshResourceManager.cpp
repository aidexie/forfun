#include "MeshResourceManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "Mesh.h"
#include "Loader/ObjLoader.h"
#include "Loader/GltfLoader.h"
#include "../Engine/Rendering/RayTracing/SceneGeometryExport.h"
#include <algorithm>

CMeshResourceManager& CMeshResourceManager::Instance() {
    static CMeshResourceManager instance;
    return instance;
}

void CMeshResourceManager::CacheMeshForRayTracing(const SMeshCPU_PNT& cpu, const std::string& path, uint32_t subMeshIndex) {
    SRayTracingMeshData rtData;
    rtData.sourcePath = path;
    rtData.vertexCount = static_cast<uint32_t>(cpu.vertices.size());
    rtData.indexCount = static_cast<uint32_t>(cpu.indices.size());

    // Extract positions
    rtData.positions.reserve(cpu.vertices.size());
    for (const auto& v : cpu.vertices) {
        rtData.positions.push_back(DirectX::XMFLOAT3(v.px, v.py, v.pz));
    }

    // Copy indices
    rtData.indices = cpu.indices;

    // Compute bounds
    if (!cpu.vertices.empty()) {
        rtData.boundsMin = DirectX::XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        rtData.boundsMax = DirectX::XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        for (const auto& v : cpu.vertices) {
            rtData.boundsMin.x = std::min(rtData.boundsMin.x, v.px);
            rtData.boundsMin.y = std::min(rtData.boundsMin.y, v.py);
            rtData.boundsMin.z = std::min(rtData.boundsMin.z, v.pz);

            rtData.boundsMax.x = std::max(rtData.boundsMax.x, v.px);
            rtData.boundsMax.y = std::max(rtData.boundsMax.y, v.py);
            rtData.boundsMax.z = std::max(rtData.boundsMax.z, v.pz);
        }
    }

    // Store in cache
    CRayTracingMeshCache::Instance().StoreMeshData(path, subMeshIndex, rtData);
}

std::vector<std::shared_ptr<GpuMeshResource>> CMeshResourceManager::GetOrLoad(
    const std::string& path,
    bool cacheForRayTracing
) {
    if (path.empty()) {
        return {};
    }

    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) {
        return {};
    }

    // Check cache first
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        std::vector<std::shared_ptr<GpuMeshResource>> result;
        bool allValid = true;

        for (auto& weakPtr : it->second) {
            if (auto shared = weakPtr.lock()) {
                result.push_back(shared);
            } else {
                allValid = false;
                break;
            }
        }

        if (allValid && !result.empty()) {
            return result; // Cache hit
        }

        // Cache expired, remove entry
        m_cache.erase(it);
    }

    // Cache miss - load from disk
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<std::shared_ptr<GpuMeshResource>> resources;

    // Load OBJ
    if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".obj") {
        SMeshCPU_PNT cpu;
        if (!LoadOBJ_PNT(path, cpu, /*flipZ*/true, /*flipWinding*/true)) {
            return {};
        }
        RecenterAndScale(cpu, 2.0f);

        // Cache for ray tracing if requested
        if (cacheForRayTracing) {
            CacheMeshForRayTracing(cpu, path, 0);
        }

        auto resource = UploadMesh(cpu);
        if (resource) {
            resources.push_back(resource);
        }
    }
    // Load glTF
    else if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".gltf") {
        std::vector<SGltfMeshCPU> meshes;
        if (!LoadGLTF_PNT(path.c_str(), meshes, /*flipZ_to_LH*/true, /*flipWinding*/true)) {
            return {};
        }

        uint32_t subMeshIndex = 0;
        for (auto& gltfMesh : meshes) {
            // Cache for ray tracing if requested
            if (cacheForRayTracing) {
                CacheMeshForRayTracing(gltfMesh.mesh, path, subMeshIndex);
            }

            auto resource = UploadGltfMesh(gltfMesh);
            if (resource) {
                resources.push_back(resource);
            }
            subMeshIndex++;
        }
    }
    // Load GLB (same as glTF)
    else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".glb") {
        std::vector<SGltfMeshCPU> meshes;
        if (!LoadGLTF_PNT(path.c_str(), meshes, /*flipZ_to_LH*/true, /*flipWinding*/true)) {
            return {};
        }

        uint32_t subMeshIndex = 0;
        for (auto& gltfMesh : meshes) {
            // Cache for ray tracing if requested
            if (cacheForRayTracing) {
                CacheMeshForRayTracing(gltfMesh.mesh, path, subMeshIndex);
            }

            auto resource = UploadGltfMesh(gltfMesh);
            if (resource) {
                resources.push_back(resource);
            }
            subMeshIndex++;
        }
    }

    if (resources.empty()) {
        return {};
    }

    // Store in cache as weak_ptr
    std::vector<std::weak_ptr<GpuMeshResource>> weakPtrs;
    for (auto& res : resources) {
        weakPtrs.push_back(res);
    }
    m_cache[path] = std::move(weakPtrs);

    return resources;
}

std::shared_ptr<GpuMeshResource> CMeshResourceManager::UploadMesh(
    const SMeshCPU_PNT& cpu
) {
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) {
        return nullptr;
    }

    auto resource = std::make_shared<GpuMeshResource>();

    // Create VBO using RHI
    RHI::BufferDesc vboDesc;
    vboDesc.size = static_cast<uint32_t>(cpu.vertices.size() * sizeof(SVertexPNT));
    vboDesc.usage = RHI::EBufferUsage::Vertex;
    vboDesc.cpuAccess = RHI::ECPUAccess::None;
    resource->vbo.reset(rhiCtx->CreateBuffer(vboDesc, cpu.vertices.data()));
    if (!resource->vbo) {
        return nullptr;
    }

    // Create IBO using RHI
    RHI::BufferDesc iboDesc;
    iboDesc.size = static_cast<uint32_t>(cpu.indices.size() * sizeof(uint32_t));
    iboDesc.usage = RHI::EBufferUsage::Index;
    iboDesc.cpuAccess = RHI::ECPUAccess::None;
    resource->ibo.reset(rhiCtx->CreateBuffer(iboDesc, cpu.indices.data()));
    if (!resource->ibo) {
        return nullptr;
    }

    resource->indexCount = static_cast<uint32_t>(cpu.indices.size());

    // Compute AABB from vertices
    if (!cpu.vertices.empty()) {
        using namespace DirectX;
        XMFLOAT3 minBounds(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 maxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        for (const auto& v : cpu.vertices) {
            minBounds.x = std::min(minBounds.x, v.px);
            minBounds.y = std::min(minBounds.y, v.py);
            minBounds.z = std::min(minBounds.z, v.pz);

            maxBounds.x = std::max(maxBounds.x, v.px);
            maxBounds.y = std::max(maxBounds.y, v.py);
            maxBounds.z = std::max(maxBounds.z, v.pz);
        }

        resource->localBoundsMin = minBounds;
        resource->localBoundsMax = maxBounds;
        resource->hasBounds = true;
    }

    return resource;
}

std::shared_ptr<GpuMeshResource> CMeshResourceManager::UploadGltfMesh(
    const SGltfMeshCPU& gltfMesh
) {
    // glTF loader now only loads geometry data
    // Textures and materials are managed separately by MaterialAsset system
    auto resource = UploadMesh(gltfMesh.mesh);
    return resource;
}

void CMeshResourceManager::CollectGarbage() {
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        bool anyValid = false;
        for (auto& weakPtr : it->second) {
            if (!weakPtr.expired()) {
                anyValid = true;
                break;
            }
        }

        if (!anyValid) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void CMeshResourceManager::ClearCache() {
    m_cache.clear();
}
