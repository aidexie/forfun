#pragma once
#include "RHICommon.h"

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
// Texture Interface
// ============================================
class ITexture : public IResource {
public:
    virtual ~ITexture() = default;

    // Get texture dimensions
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual uint32_t GetDepth() const = 0;
    virtual ETextureFormat GetFormat() const = 0;

    // Get views (may return nullptr if not applicable)
    virtual void* GetRTV() = 0;  // ID3D11RenderTargetView*, D3D12_CPU_DESCRIPTOR_HANDLE
    virtual void* GetDSV() = 0;  // ID3D11DepthStencilView*, D3D12_CPU_DESCRIPTOR_HANDLE
    virtual void* GetSRV() = 0;  // ID3D11ShaderResourceView*, D3D12_CPU_DESCRIPTOR_HANDLE
    virtual void* GetUAV() = 0;  // ID3D11UnorderedAccessView*, D3D12_CPU_DESCRIPTOR_HANDLE
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
