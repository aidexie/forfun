#pragma once
#include "RHI/RHIResources.h"
#include <DirectXMath.h>
#include <memory>
#include <cstdint>

// RAII wrapper for GPU mesh resources
// Automatically releases GPU resources when destroyed
// NOTE: This class ONLY contains geometry data (vertices, indices, bounds).
//       Textures and materials are managed separately by TextureManager and MaterialManager.
class GpuMeshResource {
public:
    std::unique_ptr<RHI::IBuffer> vbo;
    std::unique_ptr<RHI::IBuffer> ibo;
    uint32_t indexCount = 0;

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
