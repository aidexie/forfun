#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "../IDescriptorSet.h"
#include <vector>
#include <cstring>

// ============================================
// DX12 Descriptor Set Implementation
// ============================================

namespace RHI {
namespace DX12 {

// Forward declarations
class CDX12DescriptorStagingRing;
class CDX12DynamicBufferRing;

// ============================================
// Per-Set Root Parameter Mapping
// ============================================
// Computed when building root signature from layouts.
// Stored in CDX12PipelineState for use during binding.

struct SSetRootParamInfo {
    uint32_t pushConstantRootParam = UINT32_MAX;   // Root param index for push constants
    uint32_t volatileCBVRootParam = UINT32_MAX;    // Root param index for volatile CBV
    uint32_t constantBufferRootParam = UINT32_MAX; // Root param index for static CBV
    uint32_t srvTableRootParam = UINT32_MAX;       // Root param index for SRV table
    uint32_t uavTableRootParam = UINT32_MAX;       // Root param index for UAV table
    uint32_t samplerTableRootParam = UINT32_MAX;   // Root param index for Sampler table

    uint32_t srvCount = 0;
    uint32_t uavCount = 0;
    uint32_t samplerCount = 0;
    uint32_t pushConstantDwordCount = 0;
    uint32_t volatileCBVSize = 0;

    bool isUsed = false;
};

// ============================================
// CDX12DescriptorSetLayout
// ============================================
// Immutable layout describing binding schema.
// Created by allocator, shared between PSO creation and set allocation.

class CDX12DescriptorSetLayout : public IDescriptorSetLayout {
public:
    CDX12DescriptorSetLayout(const BindingLayoutDesc& desc);
    ~CDX12DescriptorSetLayout() override = default;

    // IDescriptorSetLayout interface
    uint32_t GetBindingCount() const override { return static_cast<uint32_t>(m_bindings.size()); }
    const BindingLayoutItem& GetBinding(uint32_t index) const override { return m_bindings[index]; }
    const char* GetDebugName() const override { return m_debugName.c_str(); }

    // Query helpers for root signature construction
    uint32_t GetSRVCount() const override { return m_srvCount; }
    uint32_t GetUAVCount() const override { return m_uavCount; }
    uint32_t GetSamplerCount() const override { return m_samplerCount; }
    bool HasVolatileCBV() const override { return m_hasVolatileCBV; }
    bool HasConstantBuffer() const override { return m_hasConstantBuffer; }
    bool HasPushConstants() const override { return m_hasPushConstants; }
    uint32_t GetVolatileCBVSize() const override { return m_volatileCBVSize; }
    uint32_t GetPushConstantSize() const override { return m_pushConstantSize; }

    // Get CBV slot (for root CBV binding)
    uint32_t GetVolatileCBVSlot() const { return m_volatileCBVSlot; }
    uint32_t GetConstantBufferSlot() const { return m_constantBufferSlot; }
    uint32_t GetPushConstantSlot() const { return m_pushConstantSlot; }

    // Populate descriptor ranges for root signature construction
    // Returns number of ranges added
    uint32_t PopulateSRVRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const;
    uint32_t PopulateUAVRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const;
    uint32_t PopulateSamplerRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const;

private:
    std::vector<BindingLayoutItem> m_bindings;
    std::string m_debugName;

    // Computed counts
    uint32_t m_srvCount = 0;
    uint32_t m_uavCount = 0;
    uint32_t m_samplerCount = 0;
    bool m_hasVolatileCBV = false;
    bool m_hasConstantBuffer = false;
    bool m_hasPushConstants = false;
    uint32_t m_volatileCBVSize = 0;
    uint32_t m_volatileCBVSlot = 0;
    uint32_t m_constantBufferSlot = 0;
    uint32_t m_pushConstantSize = 0;
    uint32_t m_pushConstantSlot = 0;
};

// ============================================
// CDX12DescriptorSet
// ============================================
// Mutable set holding actual resource bindings.
// Stores CPU descriptor handles for SRVs/UAVs/Samplers.
// Stores volatile data for CBV/push constants (copied to ring at bind time).

class CDX12DescriptorSet : public IDescriptorSet {
public:
    CDX12DescriptorSet(CDX12DescriptorSetLayout* layout, bool isPersistent);
    ~CDX12DescriptorSet() override;

    // IDescriptorSet interface
    void Bind(const BindingSetItem& item) override;
    void Bind(const BindingSetItem* items, uint32_t count) override;
    IDescriptorSetLayout* GetLayout() const override { return m_layout; }
    bool IsComplete() const override;

    // DX12-specific accessors for command list binding
    bool HasSRVs() const { return m_layout->GetSRVCount() > 0; }
    bool HasUAVs() const { return m_layout->GetUAVCount() > 0; }
    bool HasSamplers() const { return m_layout->GetSamplerCount() > 0; }
    bool HasVolatileCBV() const { return m_layout->HasVolatileCBV(); }
    bool HasConstantBuffer() const { return m_layout->HasConstantBuffer(); }
    bool HasPushConstants() const { return m_layout->HasPushConstants(); }

    // Copy SRVs to staging ring and return GPU handle for binding
    D3D12_GPU_DESCRIPTOR_HANDLE CopySRVsToStaging(CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device);

    // Copy UAVs to staging ring and return GPU handle for binding
    D3D12_GPU_DESCRIPTOR_HANDLE CopyUAVsToStaging(CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device);

    // Copy Samplers to staging ring and return GPU handle for binding
    D3D12_GPU_DESCRIPTOR_HANDLE CopySamplersToStaging(CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device);

    // Allocate volatile CBV from ring and return GPU virtual address
    D3D12_GPU_VIRTUAL_ADDRESS AllocateVolatileCBV(CDX12DynamicBufferRing& bufferRing);

    // Get static constant buffer GPU virtual address
    D3D12_GPU_VIRTUAL_ADDRESS GetConstantBufferGPUAddress() const { return m_constantBufferGPUAddress; }

    // Get push constant data
    const void* GetPushConstantData() const { return m_pushConstantData.data(); }
    uint32_t GetPushConstantDwordCount() const { return static_cast<uint32_t>(m_pushConstantData.size()) / 4; }

    // For persistent set management
    bool IsPersistent() const { return m_isPersistent; }

private:
    CDX12DescriptorSetLayout* m_layout = nullptr;
    bool m_isPersistent = false;

    // SRV/UAV/Sampler CPU handles (indexed by slot)
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_srvHandles;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_uavHandles;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_samplerHandles;

    // Track which slots are bound
    std::vector<bool> m_srvBound;
    std::vector<bool> m_uavBound;
    std::vector<bool> m_samplerBound;

    // Volatile CBV data (copied to ring buffer at bind time)
    std::vector<uint8_t> m_volatileCBVData;
    bool m_volatileCBVBound = false;

    // Static constant buffer
    D3D12_GPU_VIRTUAL_ADDRESS m_constantBufferGPUAddress = 0;
    bool m_constantBufferBound = false;

    // Push constant data
    std::vector<uint8_t> m_pushConstantData;
    bool m_pushConstantBound = false;
};

} // namespace DX12
} // namespace RHI
