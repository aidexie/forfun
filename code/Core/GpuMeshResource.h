#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>

// RAII wrapper for GPU mesh resources
// Automatically releases GPU resources when destroyed
class GpuMeshResource {
public:
    Microsoft::WRL::ComPtr<ID3D11Buffer> vbo;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ibo;
    UINT indexCount = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> albedoSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metallicRoughnessSRV;  // G=Roughness, B=Metallic (glTF 2.0 standard)

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
