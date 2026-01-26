#pragma once

#include "DX12Common.h"
#include "DX12DescriptorSet.h"
#include "DX12DescriptorHeap.h"
#include "../IDescriptorSet.h"
#include <vector>
#include <memory>
#include <unordered_map>

// ============================================
// DX12 Descriptor Set Allocator
// ============================================
// Manages allocation of descriptor sets for DX12.

namespace RHI {
namespace DX12 {

// Forward declarations
class CDX12DynamicBufferRing;

class CDX12DescriptorSetAllocator : public IDescriptorSetAllocator {
public:
    CDX12DescriptorSetAllocator() = default;
    ~CDX12DescriptorSetAllocator() override;

    // Initialize with device
    bool Initialize(ID3D12Device* device);

    // Shutdown and release all resources
    void Shutdown();

    // IDescriptorSetAllocator interface
    IDescriptorSetLayout* CreateLayout(const BindingLayoutDesc& desc) override;
    void DestroyLayout(IDescriptorSetLayout* layout) override;
    IDescriptorSet* AllocateSet(IDescriptorSetLayout* layout) override;
    void FreeSet(IDescriptorSet* set) override;

private:
    ID3D12Device* m_device = nullptr;

    // Owned layouts
    std::vector<std::unique_ptr<CDX12DescriptorSetLayout>> m_layouts;

    // Allocated sets
    std::vector<std::unique_ptr<CDX12DescriptorSet>> m_sets;
};

} // namespace DX12
} // namespace RHI
