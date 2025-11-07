#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// RAII wrapper for GPU mesh resources
// Automatically releases GPU resources when destroyed
class GpuMeshResource {
public:
    Microsoft::WRL::ComPtr<ID3D11Buffer> vbo;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ibo;
    UINT indexCount = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> albedoSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSRV;

    GpuMeshResource() = default;
    ~GpuMeshResource() = default;

    // Non-copyable
    GpuMeshResource(const GpuMeshResource&) = delete;
    GpuMeshResource& operator=(const GpuMeshResource&) = delete;

    // Movable
    GpuMeshResource(GpuMeshResource&&) = default;
    GpuMeshResource& operator=(GpuMeshResource&&) = default;
};
