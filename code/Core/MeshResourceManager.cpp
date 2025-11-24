#include "MeshResourceManager.h"
#include "DX11Context.h"
#include "Mesh.h"
#include "Loader/ObjLoader.h"
#include "Loader/GltfLoader.h"
#include "Loader/TextureLoader.h"
#include <d3d11.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

CMeshResourceManager& CMeshResourceManager::Instance() {
    static CMeshResourceManager instance;
    return instance;
}

std::vector<std::shared_ptr<GpuMeshResource>> CMeshResourceManager::GetOrLoad(
    const std::string& path
) {
    if (path.empty()) {
        return {};
    }

    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) {
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

        for (auto& gltfMesh : meshes) {
            auto resource = UploadGltfMesh(gltfMesh);
            if (resource) {
                resources.push_back(resource);
            }
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
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) {
        return nullptr;
    }

    auto resource = std::make_shared<GpuMeshResource>();

    // Create VBO
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(cpu.vertices.size() * sizeof(SVertexPNT));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initVB{ cpu.vertices.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&bd, &initVB, resource->vbo.GetAddressOf()))) {
        return nullptr;
    }

    // Create IBO
    D3D11_BUFFER_DESC ib{};
    ib.ByteWidth = (UINT)(cpu.indices.size() * sizeof(uint32_t));
    ib.Usage = D3D11_USAGE_DEFAULT;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initIB{ cpu.indices.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&ib, &initIB, resource->ibo.GetAddressOf()))) {
        return nullptr;
    }

    resource->indexCount = (UINT)cpu.indices.size();

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

    // Leave textures as nullptr (Renderer will use defaults during rendering)

    return resource;
}

std::shared_ptr<GpuMeshResource> CMeshResourceManager::UploadGltfMesh(
    const SGltfMeshCPU& gltfMesh
) {
    auto resource = UploadMesh(gltfMesh.mesh);
    if (!resource) return nullptr;

    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return nullptr;

    // Load albedo texture (sRGB)
    if (!gltfMesh.textures.baseColorPath.empty()) {
        ComPtr<ID3D11ShaderResourceView> srv;
        std::wstring wpath(gltfMesh.textures.baseColorPath.begin(),
                          gltfMesh.textures.baseColorPath.end());
        if (LoadTextureWIC(device, wpath, srv, /*srgb=*/true)) {
            resource->albedoSRV = srv;
        }
    }

    // Load normal texture (linear)
    if (!gltfMesh.textures.normalPath.empty()) {
        ComPtr<ID3D11ShaderResourceView> srv;
        std::wstring wpath(gltfMesh.textures.normalPath.begin(),
                          gltfMesh.textures.normalPath.end());
        if (LoadTextureWIC(device, wpath, srv, /*srgb=*/false)) {
            resource->normalSRV = srv;
        }
    }

    // Load metallic-roughness texture (linear, G=Roughness, B=Metallic)
    if (!gltfMesh.textures.metallicRoughnessPath.empty()) {
        ComPtr<ID3D11ShaderResourceView> srv;
        std::wstring wpath(gltfMesh.textures.metallicRoughnessPath.begin(),
                          gltfMesh.textures.metallicRoughnessPath.end());
        if (LoadTextureWIC(device, wpath, srv, /*srgb=*/false)) {
            resource->metallicRoughnessSRV = srv;
        }
    }

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
