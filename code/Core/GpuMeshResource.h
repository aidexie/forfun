#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>

// RAII wrapper for GPU mesh resources
// Automatically releases GPU resources when destroyed
// NOTE: This class ONLY contains geometry data (vertices, indices, bounds).
//       Textures and materials are managed separately by TextureManager and MaterialManager.
class GpuMeshResource {
public:
    Microsoft::WRL::ComPtr<ID3D11Buffer> vbo;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ibo;
    UINT indexCount = 0;

    // Local space AABB (computed once at load time, shared by all instances)
    DirectX::XMFLOAT3 localBoundsMin{-0.5f, -0.5f, -0.5f};
    DirectX::XMFLOAT3 localBoundsMax{ 0.5f,  0.5f,  0.5f};
    bool hasBounds = false;

    GpuMeshResource() = default;
    ~GpuMeshResource() = default;

    // Non-copyable
    GpuMeshResource(const GpuMeshResource&) = delete;
    GpuMeshResource& operator=(const GpuMeshResource&) = delete;

    // Movable
    GpuMeshResource(GpuMeshResource&&) = default;
    GpuMeshResource& operator=(GpuMeshResource&&) = default;
};
