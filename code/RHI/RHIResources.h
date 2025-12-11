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
// ============================================
class ITexture : public IResource {
public:
    virtual ~ITexture() = default;

    // Get texture dimensions
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual uint32_t GetDepth() const = 0;
    virtual uint32_t GetArraySize() const = 0;
    virtual uint32_t GetMipLevels() const = 0;
    virtual ETextureFormat GetFormat() const = 0;

    // Get views (may return nullptr if not applicable)
    virtual void* GetRTV() = 0;  // ID3D11RenderTargetView*, D3D12_CPU_DESCRIPTOR_HANDLE
    virtual void* GetDSV() = 0;  // ID3D11DepthStencilView*, D3D12_CPU_DESCRIPTOR_HANDLE
    virtual void* GetSRV() = 0;  // ID3D11ShaderResourceView*, D3D12_CPU_DESCRIPTOR_HANDLE
    virtual void* GetUAV() = 0;  // ID3D11UnorderedAccessView*, D3D12_CPU_DESCRIPTOR_HANDLE

    // Get per-slice DSV for texture arrays (for CSM shadow mapping)
    // Returns nullptr if arrayIndex is out of bounds or texture is not an array
    virtual void* GetDSVSlice(uint32_t arrayIndex) = 0;

    // Get per-slice RTV for texture arrays/cubemaps (for cubemap face rendering)
    // Returns nullptr if arrayIndex is out of bounds or texture is not an array
    virtual void* GetRTVSlice(uint32_t arrayIndex) = 0;

    // Get per-slice SRV for texture arrays/cubemaps (for debug visualization)
    // For cubemaps: arrayIndex = face (0-5: +X, -X, +Y, -Y, +Z, -Z), mipLevel = mip
    // Returns nullptr if indices are out of bounds
    // Note: SRV slices are created on-demand and cached
    virtual void* GetSRVSlice(uint32_t arrayIndex, uint32_t mipLevel = 0) = 0;

    // ============================================
    // CPU Access (for Staging textures)
    // ============================================

    // Map texture subresource for CPU read (only valid for Staging textures)
    // arraySlice: Array slice index (0-5 for cubemap faces)
    // mipLevel: Mip level index
    // Returns mapped data info, pData is nullptr on failure
    virtual MappedTexture Map(uint32_t arraySlice = 0, uint32_t mipLevel = 0) = 0;

    // Unmap texture subresource after reading
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
