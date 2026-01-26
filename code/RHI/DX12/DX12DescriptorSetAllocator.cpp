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
    // Clear all sets
    m_sets.clear();

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

IDescriptorSet* CDX12DescriptorSetAllocator::AllocateSet(IDescriptorSetLayout* layout) {
    if (!layout) {
        CFFLog::Error("AllocateSet: layout is null");
        return nullptr;
    }

    auto* dx12Layout = static_cast<CDX12DescriptorSetLayout*>(layout);
    auto set = std::make_unique<CDX12DescriptorSet>(dx12Layout, true);
    IDescriptorSet* ptr = set.get();
    m_sets.push_back(std::move(set));
    return ptr;
}

void CDX12DescriptorSetAllocator::FreeSet(IDescriptorSet* set) {
    if (!set) return;

    // Find and remove the set
    for (auto it = m_sets.begin(); it != m_sets.end(); ++it) {
        if (it->get() == set) {
            m_sets.erase(it);
            return;
        }
    }

    CFFLog::Warning("FreeSet: Set not found in allocator");
}

} // namespace DX12
} // namespace RHI
