#pragma once
#include "RHICommon.h"
#include "RHIDescriptors.h"

// ============================================
// RHI Resource Interfaces
// ============================================

namespace RHI {

// ============================================
// Base Resource Interface
// ============================================
class IResource {
public:
    virtual ~IResource() = default;

    // Get native API handle (ID3D11Resource*, ID3D12Resource*, etc.)
    virtual void* GetNativeHandle() = 0;
};

// ============================================
// Buffer Interface
// ============================================
class IBuffer : public IResource {
public:
    virtual ~IBuffer() = default;

    // Map buffer for CPU write (only valid if cpuAccess == Write)
    // Returns pointer to mapped memory
    virtual void* Map() = 0;

    // Unmap buffer after writing
    virtual void Unmap() = 0;

    // Get buffer size
    virtual uint32_t GetSize() const = 0;
};

// ============================================
// Texture Mapped Data
// ============================================
struct MappedTexture {
    void* pData = nullptr;      // Pointer to mapped data
    uint32_t rowPitch = 0;      // Row pitch in bytes
    uint32_t depthPitch = 0;    // Depth pitch in bytes (for 3D textures)
};

// ============================================
// Texture Interface
//
// Design principle: ITexture represents only the GPU resource.
// Views (RTV, DSV, SRV, UAV) are implementation details hidden inside
// the backend implementation. Upper layers express "intent" through
// ICommandList methods, and the backend creates/caches views as needed.
// ============================================
class ITexture : public IResource {
public:
    virtual ~ITexture() = default;

    // Get texture descriptor (all metadata in one struct)
    virtual const TextureDesc& GetDesc() const = 0;

    // Convenience accessors (all read from GetDesc())
    uint32_t GetWidth() const { return GetDesc().width; }
    uint32_t GetHeight() const { return GetDesc().height; }
    uint32_t GetDepth() const { return GetDesc().depth; }
    uint32_t GetArraySize() const { return GetDesc().arraySize; }
    uint32_t GetMipLevels() const { return GetDesc().mipLevels; }
    ETextureFormat GetFormat() const { return GetDesc().format; }
    ETextureDimension GetDimension() const { return GetDesc().dimension; }

    // ============================================
    // CPU Access (for Staging textures only)
    // ============================================

    // Map texture subresource for CPU read/write
    // Only valid for textures created with ETextureUsage::Staging
    virtual MappedTexture Map(uint32_t arraySlice = 0, uint32_t mipLevel = 0) = 0;

    // Unmap texture subresource
    virtual void Unmap(uint32_t arraySlice = 0, uint32_t mipLevel = 0) = 0;
};

// ============================================
// Sampler Interface
// ============================================
class ISampler {
public:
    virtual ~ISampler() = default;
    virtual void* GetNativeHandle() = 0;  // ID3D11SamplerState*, D3D12_CPU_DESCRIPTOR_HANDLE
};

// ============================================
// Shader Interface
// ============================================
class IShader {
public:
    virtual ~IShader() = default;
    virtual void* GetNativeHandle() = 0;  // ID3D11*Shader*, ID3DBlob*
    virtual EShaderType GetType() const = 0;
};

// ============================================
// Pipeline State Interface
// ============================================
class IPipelineState {
public:
    virtual ~IPipelineState() = default;
    virtual void* GetNativeHandle() = 0;  // ID3D11 (multiple states), ID3D12PipelineState*
};

} // namespace RHI
