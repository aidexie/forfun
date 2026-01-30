#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "DX12DescriptorSet.h"
#include "DX12MemoryAllocator.h"
#include "../RHIResources.h"
#include <unordered_map>

// ============================================
// DX12 Resource Implementations
// ============================================

namespace RHI {
namespace DX12 {

// Forward declarations
class CDX12DescriptorHeapManager;

// ============================================
// Resource State
// ============================================
// Tracks the current D3D12 resource state for automatic barrier insertion

struct ResourceState {
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// ============================================
// DX12 Buffer
// ============================================
class CDX12Buffer : public IBuffer {
public:
    // Legacy constructor (external resource, e.g., from CreateCommittedResource)
    CDX12Buffer(ID3D12Resource* resource, const BufferDesc& desc, ID3D12Device* device);

    // D3D12MA constructor (owns allocation)
    CDX12Buffer(SMemoryAllocation&& allocation, const BufferDesc& desc, ID3D12Device* device);

    ~CDX12Buffer() override;

    // IBuffer interface
    const BufferDesc& GetDesc() const override { return m_desc; }
    void* Map() override;
    void Unmap() override;
    void* GetNativeHandle() override { return m_resource.Get(); }

    // DX12-specific accessors
    ID3D12Resource* GetD3D12Resource() { return m_resource.Get(); }

    // Resource state tracking
    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES state) { m_currentState = state; }

    // Descriptor handles (created on demand)
    // CBV returns CPU handle (used with root descriptors, not descriptor tables)
    D3D12_CPU_DESCRIPTOR_HANDLE GetCBV();
    // SRV/UAV return full handle for efficient descriptor table binding
    SDescriptorHandle GetSRV();
    SDescriptorHandle GetUAV();

    bool HasCBV() const { return m_cbvHandle.IsValid(); }
    bool HasSRV() const { return m_srvHandle.IsValid(); }
    bool HasUAV() const { return m_uavHandle.IsValid(); }

    // GPU virtual address (for vertex/index/constant buffer binding)
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const {
        return m_resource->GetGPUVirtualAddress();
    }

private:
    void CreateCBV();
    void CreateSRV();
    void CreateUAV();

private:
    ComPtr<ID3D12Resource> m_resource;
    D3D12MA::Allocation* m_allocation = nullptr;  // D3D12MA allocation (owns memory if non-null)
    BufferDesc m_desc;
    ID3D12Device* m_device = nullptr;

    // Resource state
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;

    // For mappable buffers
    void* m_mappedData = nullptr;

    // Descriptor handles
    SDescriptorHandle m_cbvHandle;
    SDescriptorHandle m_srvHandle;
    SDescriptorHandle m_uavHandle;
};

// ============================================
// DX12 Texture
// ============================================
class CDX12Texture : public ITexture {
public:
    // Legacy constructor (external resource, e.g., swapchain backbuffer)
    CDX12Texture(ID3D12Resource* resource, const TextureDesc& desc, ID3D12Device* device);

    // D3D12MA constructor (owns allocation)
    CDX12Texture(SMemoryAllocation&& allocation, const TextureDesc& desc, ID3D12Device* device);

    ~CDX12Texture() override;

    // ITexture interface
    const TextureDesc& GetDesc() const override { return m_desc; }
    void* GetNativeHandle() override { return m_resource.Get(); }
    MappedTexture Map(uint32_t arraySlice = 0, uint32_t mipLevel = 0) override;
    void Unmap(uint32_t arraySlice = 0, uint32_t mipLevel = 0) override;

    // DX12-specific accessors
    ID3D12Resource* GetD3D12Resource() { return m_resource.Get(); }

    // Resource state tracking (per-subresource in the future, global for now)
    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES state) { m_currentState = state; }

    // Get default SRV (all mips, all slices) - returns full handle for efficient binding
    SDescriptorHandle GetOrCreateSRV();

    // Get SRV for specific mip/slice - returns full handle for efficient binding
    SDescriptorHandle GetOrCreateSRVSlice(uint32_t arraySlice, uint32_t mipLevel = 0);

    // Get default RTV (mip 0, slice 0)
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateRTV();

    // Get RTV for specific mip/slice
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateRTVSlice(uint32_t arraySlice, uint32_t mipLevel = 0);

    // Get default DSV (slice 0)
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateDSV();

    // Get DSV for specific slice
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateDSVSlice(uint32_t arraySlice);

    // Get default UAV (mip 0) - returns full handle for efficient binding
    SDescriptorHandle GetOrCreateUAV();

    // Get UAV for specific mip level - returns full handle for efficient binding
    SDescriptorHandle GetOrCreateUAVSlice(uint32_t mipLevel);

    // Check if views exist
    bool HasSRV() const { return m_defaultSRV.IsValid(); }
    bool HasRTV() const { return m_defaultRTV.IsValid(); }
    bool HasDSV() const { return m_defaultDSV.IsValid(); }
    bool HasUAV() const { return m_defaultUAV.IsValid(); }

private:
    // View cache key
    struct ViewKey {
        uint32_t mipLevel;
        uint32_t arraySlice;

        bool operator==(const ViewKey& other) const {
            return mipLevel == other.mipLevel && arraySlice == other.arraySlice;
        }
    };

    struct ViewKeyHash {
        size_t operator()(const ViewKey& key) const {
            return std::hash<uint64_t>{}((uint64_t)key.mipLevel << 32 | key.arraySlice);
        }
    };

    // View creation helpers
    SDescriptorHandle CreateSRV(uint32_t mipLevel, uint32_t numMips, uint32_t arraySlice, uint32_t numSlices);
    SDescriptorHandle CreateRTV(uint32_t mipLevel, uint32_t arraySlice);
    SDescriptorHandle CreateDSV(uint32_t arraySlice);
    SDescriptorHandle CreateUAV(uint32_t mipLevel);

private:
    ComPtr<ID3D12Resource> m_resource;
    D3D12MA::Allocation* m_allocation = nullptr;  // D3D12MA allocation (owns memory if non-null)
    TextureDesc m_desc;
    ID3D12Device* m_device = nullptr;

    // Resource state (simplified: one state for whole resource)
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;

    // Default views
    SDescriptorHandle m_defaultSRV;
    SDescriptorHandle m_defaultRTV;
    SDescriptorHandle m_defaultDSV;
    SDescriptorHandle m_defaultUAV;

    // View caches for slice/mip-specific views
    std::unordered_map<ViewKey, SDescriptorHandle, ViewKeyHash> m_srvCache;
    std::unordered_map<ViewKey, SDescriptorHandle, ViewKeyHash> m_rtvCache;
    std::unordered_map<uint32_t, SDescriptorHandle> m_dsvCache;  // keyed by arraySlice
    std::unordered_map<uint32_t, SDescriptorHandle> m_uavCache;  // keyed by mipLevel
};

// ============================================
// DX12 Sampler
// ============================================
class CDX12Sampler : public ISampler {
public:
    CDX12Sampler(const SDescriptorHandle& handle) : m_handle(handle) {}
    ~CDX12Sampler() override;

    void* GetNativeHandle() override { return (void*)m_handle.cpuHandle.ptr; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle() const { return m_handle.cpuHandle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle() const { return m_handle.gpuHandle; }
    const SDescriptorHandle& GetHandle() const { return m_handle; }

private:
    SDescriptorHandle m_handle;
};

// ============================================
// DX12 Shader
// ============================================
class CDX12Shader : public IShader {
public:
    CDX12Shader(EShaderType type, const void* bytecode, size_t bytecodeSize);
    ~CDX12Shader() override = default;

    void* GetNativeHandle() override { return m_bytecode.data(); }
    EShaderType GetType() const override { return m_type; }

    D3D12_SHADER_BYTECODE GetBytecode() const {
        return { m_bytecode.data(), m_bytecode.size() };
    }

    const std::vector<uint8_t>& GetBytecodeData() const { return m_bytecode; }

private:
    EShaderType m_type;
    std::vector<uint8_t> m_bytecode;
};

// ============================================
// DX12 Pipeline State
// ============================================
class CDX12PipelineState : public IPipelineState {
public:
    // Legacy constructor (no descriptor sets)
    CDX12PipelineState(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig, bool isCompute = false);

    // Descriptor set constructor
    CDX12PipelineState(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig,
                       const SSetRootParamInfo setBindings[4],
                       IDescriptorSetLayout* const expectedLayouts[4],
                       bool isCompute = false);

    ~CDX12PipelineState() override = default;

    void* GetNativeHandle() override { return m_pso.Get(); }

    ID3D12PipelineState* GetPSO() { return m_pso.Get(); }
    ID3D12RootSignature* GetRootSignature() { return m_rootSignature.Get(); }
    bool IsCompute() const { return m_isCompute; }

    // Descriptor set support
    bool UsesDescriptorSets() const { return m_usesDescriptorSets; }
    const SSetRootParamInfo& GetSetBindingInfo(uint32_t setIndex) const {
        return m_setBindings[setIndex];
    }
    IDescriptorSetLayout* GetExpectedLayout(uint32_t setIndex) const {
        return m_expectedLayouts[setIndex];
    }

private:
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    bool m_isCompute = false;

    // Descriptor set support
    bool m_usesDescriptorSets = false;
    SSetRootParamInfo m_setBindings[4];
    IDescriptorSetLayout* m_expectedLayouts[4] = {nullptr, nullptr, nullptr, nullptr};
};

// ============================================
// Format Conversion Utilities
// ============================================

// Convert RHI format to DXGI format
DXGI_FORMAT ToDXGIFormat(ETextureFormat format);

// Convert DXGI format to RHI format
ETextureFormat FromDXGIFormat(DXGI_FORMAT format);

// Convert RHI resource state to D3D12 resource state
D3D12_RESOURCE_STATES ToD3D12ResourceState(EResourceState state);

// Get D3D12 resource flags from texture usage
D3D12_RESOURCE_FLAGS GetResourceFlags(ETextureUsage usage);

// Get D3D12 heap type for buffer
D3D12_HEAP_TYPE GetHeapType(ECPUAccess cpuAccess, EBufferUsage usage);

// Get initial resource state based on heap type
D3D12_RESOURCE_STATES GetInitialResourceState(D3D12_HEAP_TYPE heapType, EBufferUsage bufferUsage);
D3D12_RESOURCE_STATES GetInitialResourceState(D3D12_HEAP_TYPE heapType, ETextureUsage textureUsage);

} // namespace DX12
} // namespace RHI
