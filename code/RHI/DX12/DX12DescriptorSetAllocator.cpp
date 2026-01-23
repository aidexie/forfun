#include "DX12DescriptorSetAllocator.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

CDX12DescriptorSetAllocator::~CDX12DescriptorSetAllocator() {
    Shutdown();
}

bool CDX12DescriptorSetAllocator::Initialize(ID3D12Device* device) {
    m_device = device;
    return true;
}

void CDX12DescriptorSetAllocator::Shutdown() {
    // Clear all transient sets
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_transientSets[i].clear();
    }

    // Clear persistent sets
    m_persistentSets.clear();

    // Clear layouts
    m_layouts.clear();

    m_device = nullptr;
}

IDescriptorSetLayout* CDX12DescriptorSetAllocator::CreateLayout(const BindingLayoutDesc& desc) {
    auto layout = std::make_unique<CDX12DescriptorSetLayout>(desc);
    IDescriptorSetLayout* ptr = layout.get();
    m_layouts.push_back(std::move(layout));
    return ptr;
}

void CDX12DescriptorSetAllocator::DestroyLayout(IDescriptorSetLayout* layout) {
    if (!layout) return;

    // Find and remove the layout
    for (auto it = m_layouts.begin(); it != m_layouts.end(); ++it) {
        if (it->get() == layout) {
            m_layouts.erase(it);
            return;
        }
    }

    CFFLog::Warning("DestroyLayout: Layout not found in allocator");
}

IDescriptorSet* CDX12DescriptorSetAllocator::AllocatePersistentSet(IDescriptorSetLayout* layout) {
    if (!layout) {
        CFFLog::Error("AllocatePersistentSet: layout is null");
        return nullptr;
    }

    auto* dx12Layout = static_cast<CDX12DescriptorSetLayout*>(layout);
    auto set = std::make_unique<CDX12DescriptorSet>(dx12Layout, true);
    IDescriptorSet* ptr = set.get();
    m_persistentSets.push_back(std::move(set));
    return ptr;
}

IDescriptorSet* CDX12DescriptorSetAllocator::AllocateTransientSet(IDescriptorSetLayout* layout) {
    if (!layout) {
        CFFLog::Error("AllocateTransientSet: layout is null");
        return nullptr;
    }

    auto* dx12Layout = static_cast<CDX12DescriptorSetLayout*>(layout);
    auto set = std::make_unique<CDX12DescriptorSet>(dx12Layout, false);
    IDescriptorSet* ptr = set.get();
    m_transientSets[m_currentFrameIndex].push_back(std::move(set));
    return ptr;
}

void CDX12DescriptorSetAllocator::FreePersistentSet(IDescriptorSet* set) {
    if (!set) return;

    // Find and remove the set
    for (auto it = m_persistentSets.begin(); it != m_persistentSets.end(); ++it) {
        if (it->get() == set) {
            m_persistentSets.erase(it);
            return;
        }
    }

    CFFLog::Warning("FreePersistentSet: Set not found in allocator");
}

void CDX12DescriptorSetAllocator::BeginFrame(uint32_t frameIndex) {
    m_currentFrameIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;

    // Clear transient sets from this frame slot (they were used 3 frames ago)
    m_transientSets[m_currentFrameIndex].clear();
}

} // namespace DX12
} // namespace RHI
