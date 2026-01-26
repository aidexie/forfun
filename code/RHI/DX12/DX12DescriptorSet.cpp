#include "DX12DescriptorSet.h"
#include "DX12DescriptorHeap.h"
#include "DX12DynamicBuffer.h"
#include "DX12Resources.h"
#include "../RHIResources.h"
#include <cassert>

namespace RHI {

// ============================================
// BindingLayoutItem Static Factory Methods
// ============================================

BindingLayoutItem BindingLayoutItem::Texture_SRV(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::Texture_SRV;
    item.slot = slot;
    item.count = 1;
    return item;
}

BindingLayoutItem BindingLayoutItem::Texture_SRVArray(uint32_t slot, uint32_t count) {
    BindingLayoutItem item;
    item.type = EDescriptorType::Texture_SRV;
    item.slot = slot;
    item.count = count;
    return item;
}

BindingLayoutItem BindingLayoutItem::Buffer_SRV(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::Buffer_SRV;
    item.slot = slot;
    item.count = 1;
    return item;
}

BindingLayoutItem BindingLayoutItem::Texture_UAV(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::Texture_UAV;
    item.slot = slot;
    item.count = 1;
    return item;
}

BindingLayoutItem BindingLayoutItem::Buffer_UAV(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::Buffer_UAV;
    item.slot = slot;
    item.count = 1;
    return item;
}

BindingLayoutItem BindingLayoutItem::ConstantBuffer(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::ConstantBuffer;
    item.slot = slot;
    item.count = 1;
    return item;
}

BindingLayoutItem BindingLayoutItem::VolatileCBV(uint32_t slot, uint32_t size) {
    BindingLayoutItem item;
    item.type = EDescriptorType::VolatileCBV;
    item.slot = slot;
    item.count = 1;
    item.size = size;
    return item;
}

BindingLayoutItem BindingLayoutItem::PushConstants(uint32_t slot, uint32_t size) {
    BindingLayoutItem item;
    item.type = EDescriptorType::PushConstants;
    item.slot = slot;
    item.count = 1;
    item.size = size;
    return item;
}

BindingLayoutItem BindingLayoutItem::Sampler(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::Sampler;
    item.slot = slot;
    item.count = 1;
    return item;
}

BindingLayoutItem BindingLayoutItem::AccelerationStructure(uint32_t slot) {
    BindingLayoutItem item;
    item.type = EDescriptorType::AccelerationStructure;
    item.slot = slot;
    item.count = 1;
    return item;
}

// ============================================
// BindingSetItem Static Factory Methods
// ============================================

BindingSetItem BindingSetItem::Texture_SRV(uint32_t slot, ITexture* tex) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::Texture_SRV;
    item.texture = tex;
    return item;
}

BindingSetItem BindingSetItem::Texture_SRVSlice(uint32_t slot, ITexture* tex, uint32_t arraySlice) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::Texture_SRV;
    item.texture = tex;
    item.arraySlice = arraySlice;
    return item;
}

BindingSetItem BindingSetItem::Buffer_SRV(uint32_t slot, IBuffer* buf) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::Buffer_SRV;
    item.buffer = buf;
    return item;
}

BindingSetItem BindingSetItem::Texture_UAV(uint32_t slot, ITexture* tex, uint32_t mip) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::Texture_UAV;
    item.texture = tex;
    item.mipLevel = mip;
    return item;
}

BindingSetItem BindingSetItem::Buffer_UAV(uint32_t slot, IBuffer* buf) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::Buffer_UAV;
    item.buffer = buf;
    return item;
}

BindingSetItem BindingSetItem::ConstantBuffer(uint32_t slot, IBuffer* buf) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::ConstantBuffer;
    item.buffer = buf;
    return item;
}

BindingSetItem BindingSetItem::VolatileCBV(uint32_t slot, const void* data, uint32_t size) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::VolatileCBV;
    item.volatileData = data;
    item.volatileDataSize = size;
    return item;
}

BindingSetItem BindingSetItem::PushConstants(uint32_t slot, const void* data, uint32_t size) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::PushConstants;
    item.volatileData = data;
    item.volatileDataSize = size;
    return item;
}

BindingSetItem BindingSetItem::Sampler(uint32_t slot, ISampler* samp) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::Sampler;
    item.sampler = samp;
    return item;
}

BindingSetItem BindingSetItem::AccelerationStructure(uint32_t slot, IAccelerationStructure* as) {
    BindingSetItem item;
    item.slot = slot;
    item.type = EDescriptorType::AccelerationStructure;
    item.accelStruct = as;
    return item;
}

namespace DX12 {

// ============================================
// CDX12DescriptorSetLayout Implementation
// ============================================

CDX12DescriptorSetLayout::CDX12DescriptorSetLayout(const BindingLayoutDesc& desc) {
    m_bindings = desc.GetItems();
    m_debugName = desc.GetDebugName() ? desc.GetDebugName() : "";

    // Compute counts and find special bindings
    for (const auto& binding : m_bindings) {
        switch (binding.type) {
            case EDescriptorType::Texture_SRV:
            case EDescriptorType::Buffer_SRV:
            case EDescriptorType::AccelerationStructure:
                m_srvCount += binding.count;
                break;
            case EDescriptorType::Texture_UAV:
            case EDescriptorType::Buffer_UAV:
                m_uavCount += binding.count;
                break;
            case EDescriptorType::Sampler:
                m_samplerCount += binding.count;
                break;
            case EDescriptorType::VolatileCBV:
                m_volatileCBVs.push_back({binding.slot, binding.size});
                break;
            case EDescriptorType::ConstantBuffer:
                m_hasConstantBuffer = true;
                m_constantBufferSlot = binding.slot;
                break;
            case EDescriptorType::PushConstants:
                m_hasPushConstants = true;
                m_pushConstantSize = binding.size;
                m_pushConstantSlot = binding.slot;
                break;
        }
    }
}

uint32_t CDX12DescriptorSetLayout::PopulateSRVRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const {
    uint32_t rangeCount = 0;

    for (const auto& binding : m_bindings) {
        if (binding.type == EDescriptorType::Texture_SRV ||
            binding.type == EDescriptorType::Buffer_SRV ||
            binding.type == EDescriptorType::AccelerationStructure) {

            D3D12_DESCRIPTOR_RANGE1& range = ranges[rangeCount++];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = binding.count;
            range.BaseShaderRegister = binding.slot;
            range.RegisterSpace = registerSpace;
            range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }
    }

    return rangeCount;
}

uint32_t CDX12DescriptorSetLayout::PopulateUAVRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const {
    uint32_t rangeCount = 0;

    for (const auto& binding : m_bindings) {
        if (binding.type == EDescriptorType::Texture_UAV ||
            binding.type == EDescriptorType::Buffer_UAV) {

            D3D12_DESCRIPTOR_RANGE1& range = ranges[rangeCount++];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            range.NumDescriptors = binding.count;
            range.BaseShaderRegister = binding.slot;
            range.RegisterSpace = registerSpace;
            range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }
    }

    return rangeCount;
}

uint32_t CDX12DescriptorSetLayout::PopulateSamplerRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const {
    uint32_t rangeCount = 0;

    for (const auto& binding : m_bindings) {
        if (binding.type == EDescriptorType::Sampler) {
            D3D12_DESCRIPTOR_RANGE1& range = ranges[rangeCount++];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            range.NumDescriptors = binding.count;
            range.BaseShaderRegister = binding.slot;
            range.RegisterSpace = registerSpace;
            range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }
    }

    return rangeCount;
}

// ============================================
// CDX12DescriptorSet Implementation
// ============================================

CDX12DescriptorSet::CDX12DescriptorSet(CDX12DescriptorSetLayout* layout, bool isPersistent)
    : m_layout(layout)
    , m_isPersistent(isPersistent) {

    // Initialize handle arrays
    uint32_t srvCount = layout->GetSRVCount();
    uint32_t uavCount = layout->GetUAVCount();
    uint32_t samplerCount = layout->GetSamplerCount();

    if (srvCount > 0) {
        m_srvHandles.resize(srvCount);
        m_srvBound.resize(srvCount, false);
        for (auto& h : m_srvHandles) { h.ptr = 0; }
    }

    if (uavCount > 0) {
        m_uavHandles.resize(uavCount);
        m_uavBound.resize(uavCount, false);
        for (auto& h : m_uavHandles) { h.ptr = 0; }
    }

    if (samplerCount > 0) {
        m_samplerHandles.resize(samplerCount);
        m_samplerBound.resize(samplerCount, false);
        for (auto& h : m_samplerHandles) { h.ptr = 0; }
    }

    // Initialize volatile CBV storage for each CBV in layout
    if (layout->HasVolatileCBV()) {
        const auto& cbvInfos = layout->GetVolatileCBVs();
        m_volatileCBVs.resize(cbvInfos.size());
        for (size_t i = 0; i < cbvInfos.size(); ++i) {
            m_volatileCBVs[i].slot = cbvInfos[i].slot;
            m_volatileCBVs[i].data.resize(cbvInfos[i].size, 0);
            m_volatileCBVs[i].bound = false;
        }
    }

    // Initialize push constant storage
    if (layout->HasPushConstants()) {
        m_pushConstantData.resize(layout->GetPushConstantSize(), 0);
    }
}

CDX12DescriptorSet::~CDX12DescriptorSet() {
    // Descriptor handles are not owned - they come from textures/buffers/samplers
}

void CDX12DescriptorSet::Bind(const BindingSetItem& item) {
    switch (item.type) {
        case EDescriptorType::Texture_SRV: {
            if (item.texture) {
                auto* dx12Tex = static_cast<CDX12Texture*>(item.texture);
                SDescriptorHandle handle;
                if (item.arraySlice > 0) {
                    handle = dx12Tex->GetOrCreateSRVSlice(item.arraySlice, 0);
                } else {
                    handle = dx12Tex->GetOrCreateSRV();
                }
                if (item.slot < m_srvHandles.size()) {
                    m_srvHandles[item.slot] = handle.cpuHandle;
                    m_srvBound[item.slot] = true;
                }
            }
            break;
        }
        case EDescriptorType::Buffer_SRV: {
            if (item.buffer) {
                auto* dx12Buf = static_cast<CDX12Buffer*>(item.buffer);
                SDescriptorHandle handle = dx12Buf->GetSRV();
                if (item.slot < m_srvHandles.size()) {
                    m_srvHandles[item.slot] = handle.cpuHandle;
                    m_srvBound[item.slot] = true;
                }
            }
            break;
        }
        case EDescriptorType::Texture_UAV: {
            if (item.texture) {
                auto* dx12Tex = static_cast<CDX12Texture*>(item.texture);
                SDescriptorHandle handle;
                if (item.mipLevel > 0) {
                    handle = dx12Tex->GetOrCreateUAVSlice(item.mipLevel);
                } else {
                    handle = dx12Tex->GetOrCreateUAV();
                }
                if (item.slot < m_uavHandles.size()) {
                    m_uavHandles[item.slot] = handle.cpuHandle;
                    m_uavBound[item.slot] = true;
                }
            }
            break;
        }
        case EDescriptorType::Buffer_UAV: {
            if (item.buffer) {
                auto* dx12Buf = static_cast<CDX12Buffer*>(item.buffer);
                SDescriptorHandle handle = dx12Buf->GetUAV();
                if (item.slot < m_uavHandles.size()) {
                    m_uavHandles[item.slot] = handle.cpuHandle;
                    m_uavBound[item.slot] = true;
                }
            }
            break;
        }
        case EDescriptorType::Sampler: {
            if (item.sampler) {
                auto* dx12Sampler = static_cast<CDX12Sampler*>(item.sampler);
                if (item.slot < m_samplerHandles.size()) {
                    m_samplerHandles[item.slot] = dx12Sampler->GetCPUHandle();
                    m_samplerBound[item.slot] = true;
                }
            }
            break;
        }
        case EDescriptorType::ConstantBuffer: {
            if (item.buffer) {
                auto* dx12Buf = static_cast<CDX12Buffer*>(item.buffer);
                m_constantBufferGPUAddress = dx12Buf->GetGPUVirtualAddress();
                m_constantBufferBound = true;
            }
            break;
        }
        case EDescriptorType::VolatileCBV: {
            if (item.volatileData && item.volatileDataSize > 0) {
                // Find the CBV entry for this slot
                for (auto& cbv : m_volatileCBVs) {
                    if (cbv.slot == item.slot) {
                        size_t copySize = (std::min)(static_cast<size_t>(item.volatileDataSize), cbv.data.size());
                        std::memcpy(cbv.data.data(), item.volatileData, copySize);
                        cbv.bound = true;
                        break;
                    }
                }
            }
            break;
        }
        case EDescriptorType::PushConstants: {
            if (item.volatileData && item.volatileDataSize > 0) {
                size_t copySize = (std::min)(static_cast<size_t>(item.volatileDataSize), m_pushConstantData.size());
                std::memcpy(m_pushConstantData.data(), item.volatileData, copySize);
                m_pushConstantBound = true;
            }
            break;
        }
        case EDescriptorType::AccelerationStructure: {
            // TLAS binding - handled via SRV slot
            // TODO: Implement when ray tracing is needed
            break;
        }
    }
}

void CDX12DescriptorSet::Bind(const BindingSetItem* items, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        Bind(items[i]);
    }
}

bool CDX12DescriptorSet::IsComplete() const {
    // Check all SRVs are bound
    for (bool bound : m_srvBound) {
        if (!bound) return false;
    }
    // Check all UAVs are bound
    for (bool bound : m_uavBound) {
        if (!bound) return false;
    }
    // Check all samplers are bound
    for (bool bound : m_samplerBound) {
        if (!bound) return false;
    }
    // Check CBV if required
    if (m_layout->HasConstantBuffer() && !m_constantBufferBound) return false;
    if (m_layout->HasVolatileCBV()) {
        for (const auto& cbv : m_volatileCBVs) {
            if (!cbv.bound) return false;
        }
    }
    if (m_layout->HasPushConstants() && !m_pushConstantBound) return false;

    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12DescriptorSet::CopySRVsToStaging(
    CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device) {

    uint32_t count = static_cast<uint32_t>(m_srvHandles.size());
    if (count == 0) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    // Allocate contiguous block in staging ring
    SDescriptorHandle stagingHandle = stagingRing.AllocateContiguous(count);
    if (!stagingHandle.IsValid()) {
        // Staging ring overflow - this is a fatal error
        assert(false && "SRV staging ring overflow");
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    // Copy descriptors to staging
    device->CopyDescriptors(
        1, &stagingHandle.cpuHandle, &count,
        count, m_srvHandles.data(), nullptr,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return stagingHandle.gpuHandle;
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12DescriptorSet::CopyUAVsToStaging(
    CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device) {

    uint32_t count = static_cast<uint32_t>(m_uavHandles.size());
    if (count == 0) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    SDescriptorHandle stagingHandle = stagingRing.AllocateContiguous(count);
    if (!stagingHandle.IsValid()) {
        assert(false && "UAV staging ring overflow");
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    device->CopyDescriptors(
        1, &stagingHandle.cpuHandle, &count,
        count, m_uavHandles.data(), nullptr,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return stagingHandle.gpuHandle;
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12DescriptorSet::CopySamplersToStaging(
    CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device) {

    uint32_t count = static_cast<uint32_t>(m_samplerHandles.size());
    if (count == 0) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    SDescriptorHandle stagingHandle = stagingRing.AllocateContiguous(count);
    if (!stagingHandle.IsValid()) {
        assert(false && "Sampler staging ring overflow");
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    device->CopyDescriptors(
        1, &stagingHandle.cpuHandle, &count,
        count, m_samplerHandles.data(), nullptr,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    return stagingHandle.gpuHandle;
}

D3D12_GPU_VIRTUAL_ADDRESS CDX12DescriptorSet::AllocateVolatileCBV(CDX12DynamicBufferRing& bufferRing, uint32_t slot) {
    // Find CBV entry for this slot
    for (const auto& cbv : m_volatileCBVs) {
        if (cbv.slot == slot && !cbv.data.empty()) {
            // Allocate from ring buffer
            SDynamicAllocation alloc = bufferRing.Allocate(cbv.data.size());
            if (!alloc.IsValid()) {
                assert(false && "Dynamic buffer ring overflow");
                return 0;
            }

            // Copy data
            std::memcpy(alloc.cpuAddress, cbv.data.data(), cbv.data.size());

            return alloc.gpuAddress;
        }
    }
    return 0;
}

uint32_t CDX12DescriptorSet::GetVolatileCBVCount() const {
    return static_cast<uint32_t>(m_volatileCBVs.size());
}

uint32_t CDX12DescriptorSet::GetVolatileCBVSlot(uint32_t index) const {
    if (index < m_volatileCBVs.size()) {
        return m_volatileCBVs[index].slot;
    }
    return UINT32_MAX;
}

} // namespace DX12
} // namespace RHI
