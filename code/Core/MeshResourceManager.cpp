#include "MeshResourceManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "Mesh.h"
#include "Loader/ObjLoader.h"
#include "Loader/GltfLoader.h"
#include "../Engine/Rendering/RayTracing/SceneGeometryExport.h"
#include "../Engine/Rendering/Lightmap/LightmapUV2.h"
#include "FFLog.h"
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

    // Extract positions, normals, and UV2
    rtData.positions.reserve(cpu.vertices.size());
    rtData.normals.reserve(cpu.vertices.size());
    rtData.uv2.reserve(cpu.vertices.size());
    for (const auto& v : cpu.vertices) {
        rtData.positions.push_back(DirectX::XMFLOAT3(v.px, v.py, v.pz));
        rtData.normals.push_back(DirectX::XMFLOAT3(v.nx, v.ny, v.nz));
        rtData.uv2.push_back(DirectX::XMFLOAT2(v.u2, v.v2));
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

// ============================================
// ApplyUV2ToMesh - Generate UV2 using xatlas and rebuild mesh in-place
// ============================================
static void ApplyUV2ToMesh(SMeshCPU_PNT& cpu, int texelsPerUnit = 16) {
    if (cpu.vertices.empty() || cpu.indices.empty()) {
        return;
    }

    // Generate UV2 using xatlas
    SUV2GenerationResult result = GenerateUV2ForMesh(cpu, texelsPerUnit);

    if (!result.success) {
        CFFLog::Warning("[MeshResourceManager] UV2 generation failed, mesh will have no lightmap UV");
        return;
    }

    // Rebuild mesh with new vertex/index data from xatlas
    // xatlas may split vertices at UV seams, so we need to rebuild entirely
    std::vector<SVertexPNT> newVertices;
    newVertices.reserve(result.positions.size());

    for (size_t i = 0; i < result.positions.size(); i++) {
        SVertexPNT v = {};

        // Position
        v.px = result.positions[i].x;
        v.py = result.positions[i].y;
        v.pz = result.positions[i].z;

        // Normal
        v.nx = result.normals[i].x;
        v.ny = result.normals[i].y;
        v.nz = result.normals[i].z;

        // UV1 (original texture UV)
        if (i < result.uv1.size()) {
            v.u = result.uv1[i].x;
            v.v = result.uv1[i].y;
        }

        // Tangent
        if (i < result.tangents.size()) {
            v.tx = result.tangents[i].x;
            v.ty = result.tangents[i].y;
            v.tz = result.tangents[i].z;
            v.tw = result.tangents[i].w;
        }

        // Vertex color
        if (i < result.colors.size()) {
            v.r = result.colors[i].x;
            v.g = result.colors[i].y;
            v.b = result.colors[i].z;
            v.a = result.colors[i].w;
        } else {
            v.r = v.g = v.b = v.a = 1.0f;
        }

        // UV2 (lightmap UV) - the key output!
        v.u2 = result.uv2[i].x;
        v.v2 = result.uv2[i].y;

        newVertices.push_back(v);
    }

    // Replace mesh data
    cpu.vertices = std::move(newVertices);
    cpu.indices = result.indices;

    CFFLog::Info("[MeshResourceManager] UV2 applied: %d vertices, %d indices",
                 static_cast<int>(cpu.vertices.size()),
                 static_cast<int>(cpu.indices.size()));
}

std::vector<std::shared_ptr<GpuMeshResource>> CMeshResourceManager::GetOrLoad(
    const std::string& path,
    bool cacheForRayTracing,
    bool generateLightmapUV2
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

        // Generate UV2 if requested (must be before ray tracing cache)
        if (generateLightmapUV2) {
            ApplyUV2ToMesh(cpu);
        }

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
            // // Generate UV2 if requested (must be before ray tracing cache)
            // if (generateLightmapUV2) {
            //     ApplyUV2ToMesh(gltfMesh.mesh);
            // }

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
            // // Generate UV2 if requested (must be before ray tracing cache)
            // if (generateLightmapUV2) {
            //     ApplyUV2ToMesh(gltfMesh.mesh);
            // }

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
