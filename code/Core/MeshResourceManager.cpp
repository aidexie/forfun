#include "MeshResourceManager.h"
#include "DX11Context.h"
#include "Mesh.h"
#include "ObjLoader.h"
#include "GltfLoader.h"
#include "TextureLoader.h"
#include <d3d11.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

MeshResourceManager& MeshResourceManager::Instance() {
    static MeshResourceManager instance;
    return instance;
}

std::vector<std::shared_ptr<GpuMeshResource>> MeshResourceManager::GetOrLoad(
    const std::string& path
) {
    if (path.empty()) {
        return {};
    }

    ID3D11Device* device = DX11Context::Instance().GetDevice();
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
        MeshCPU_PNT cpu;
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
        std::vector<GltfMeshCPU> meshes;
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

std::shared_ptr<GpuMeshResource> MeshResourceManager::UploadMesh(
    const MeshCPU_PNT& cpu
) {
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    if (!device) {
        return nullptr;
    }

    auto resource = std::make_shared<GpuMeshResource>();

    // Create VBO
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(cpu.vertices.size() * sizeof(VertexPNT));
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

    // Leave textures as nullptr (Renderer will use defaults during rendering)

    return resource;
}

std::shared_ptr<GpuMeshResource> MeshResourceManager::UploadGltfMesh(
    const GltfMeshCPU& gltfMesh
) {
    auto resource = UploadMesh(gltfMesh.mesh);
    if (!resource) return nullptr;

    ID3D11Device* device = DX11Context::Instance().GetDevice();
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

    return resource;
}

void MeshResourceManager::CollectGarbage() {
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

void MeshResourceManager::ClearCache() {
    m_cache.clear();
}
