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
// - Persistent sets: Long-lived, freed explicitly
// - Transient sets: Per-frame, auto-freed at frame boundary

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
    IDescriptorSet* AllocatePersistentSet(IDescriptorSetLayout* layout) override;
    IDescriptorSet* AllocateTransientSet(IDescriptorSetLayout* layout) override;
    void FreePersistentSet(IDescriptorSet* set) override;
    void BeginFrame(uint32_t frameIndex) override;

private:
    ID3D12Device* m_device = nullptr;

    // Owned layouts
    std::vector<std::unique_ptr<CDX12DescriptorSetLayout>> m_layouts;

    // Persistent sets (explicitly freed)
    std::vector<std::unique_ptr<CDX12DescriptorSet>> m_persistentSets;

    // Transient sets per frame (auto-freed at frame boundary)
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
    std::vector<std::unique_ptr<CDX12DescriptorSet>> m_transientSets[MAX_FRAMES_IN_FLIGHT];
    uint32_t m_currentFrameIndex = 0;
};

} // namespace DX12
} // namespace RHI
