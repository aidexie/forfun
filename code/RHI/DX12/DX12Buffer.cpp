#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// CDX12Buffer Implementation
// ============================================

CDX12Buffer::CDX12Buffer(ID3D12Resource* resource, const BufferDesc& desc, ID3D12Device* device)
    : m_resource(resource)
    , m_desc(desc)
    , m_device(device)
{
    // Set initial state based on heap type
    D3D12_HEAP_TYPE heapType = GetHeapType(desc.cpuAccess, desc.usage);
    m_currentState = GetInitialResourceState(heapType, desc.usage);
}

CDX12Buffer::~CDX12Buffer() {
    // Unmap if still mapped
    if (m_mappedData) {
        m_resource->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }

    // Free descriptor handles
    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    if (m_cbvHandle.IsValid()) {
        heapMgr.FreeCBVSRVUAV(m_cbvHandle);
    }
    if (m_srvHandle.IsValid()) {
        heapMgr.FreeCBVSRVUAV(m_srvHandle);
    }
    if (m_uavHandle.IsValid()) {
        heapMgr.FreeCBVSRVUAV(m_uavHandle);
    }
}

void* CDX12Buffer::Map() {
    if (m_desc.cpuAccess != ECPUAccess::Write) {
        CFFLog::Error("[CDX12Buffer] Cannot map buffer without Write CPU access");
        return nullptr;
    }

    if (m_mappedData) {
        // Already mapped
        return m_mappedData;
    }

    D3D12_RANGE readRange = { 0, 0 };  // We don't intend to read
    HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
    if (FAILED(hr)) {
        CFFLog::Error("[CDX12Buffer] Map failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    return m_mappedData;
}

void CDX12Buffer::Unmap() {
    if (!m_mappedData) {
        return;
    }

    D3D12_RANGE writtenRange = { 0, m_desc.size };
    m_resource->Unmap(0, &writtenRange);
    m_mappedData = nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Buffer::GetCBV() {
    if (!m_cbvHandle.IsValid()) {
        CreateCBV();
    }
    return m_cbvHandle.cpuHandle;
}

SDescriptorHandle CDX12Buffer::GetSRV() {
    if (!m_srvHandle.IsValid()) {
        CreateSRV();
    }
    return m_srvHandle;
}

SDescriptorHandle CDX12Buffer::GetUAV() {
    if (!m_uavHandle.IsValid()) {
        CreateUAV();
    }
    return m_uavHandle;
}

void CDX12Buffer::CreateCBV() {
    if (!(m_desc.usage & EBufferUsage::Constant)) {
        CFFLog::Warning("[CDX12Buffer] Creating CBV for non-constant buffer");
    }

    m_cbvHandle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
    if (!m_cbvHandle.IsValid()) {
        CFFLog::Error("[CDX12Buffer] Failed to allocate CBV descriptor");
        return;
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_resource->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = AlignUp(m_desc.size, CONSTANT_BUFFER_ALIGNMENT);

    m_device->CreateConstantBufferView(&cbvDesc, m_cbvHandle.cpuHandle);
}

void CDX12Buffer::CreateSRV() {
    if (!(m_desc.usage & EBufferUsage::Structured)) {
        CFFLog::Warning("[CDX12Buffer] Creating SRV for non-structured buffer");
    }

    m_srvHandle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
    if (!m_srvHandle.IsValid()) {
        CFFLog::Error("[CDX12Buffer] Failed to allocate SRV descriptor");
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = m_desc.size / m_desc.structureByteStride;
    srvDesc.Buffer.StructureByteStride = m_desc.structureByteStride;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    m_device->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srvHandle.cpuHandle);
}

void CDX12Buffer::CreateUAV() {
    if (!(m_desc.usage & EBufferUsage::UnorderedAccess)) {
        CFFLog::Warning("[CDX12Buffer] Creating UAV for non-UAV buffer");
    }

    m_uavHandle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
    if (!m_uavHandle.IsValid()) {
        CFFLog::Error("[CDX12Buffer] Failed to allocate UAV descriptor");
        return;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = m_desc.size / m_desc.structureByteStride;
    uavDesc.Buffer.StructureByteStride = m_desc.structureByteStride;
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    m_device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, m_uavHandle.cpuHandle);
}

// ============================================
// Format Conversion Utilities
// ============================================

DXGI_FORMAT ToDXGIFormat(ETextureFormat format) {
    switch (format) {
        case ETextureFormat::Unknown:               return DXGI_FORMAT_UNKNOWN;
        case ETextureFormat::R8_UNORM:              return DXGI_FORMAT_R8_UNORM;
        case ETextureFormat::R8G8B8A8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM;
        case ETextureFormat::R8G8B8A8_UNORM_SRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case ETextureFormat::R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case ETextureFormat::R16G16_FLOAT:          return DXGI_FORMAT_R16G16_FLOAT;
        case ETextureFormat::R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case ETextureFormat::R32G32B32A32_FLOAT:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case ETextureFormat::B8G8R8A8_UNORM:        return DXGI_FORMAT_B8G8R8A8_UNORM;
        case ETextureFormat::B8G8R8A8_UNORM_SRGB:   return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case ETextureFormat::D24_UNORM_S8_UINT:     return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case ETextureFormat::D32_FLOAT:             return DXGI_FORMAT_D32_FLOAT;
        case ETextureFormat::R24G8_TYPELESS:        return DXGI_FORMAT_R24G8_TYPELESS;
        case ETextureFormat::R32_TYPELESS:          return DXGI_FORMAT_R32_TYPELESS;
        case ETextureFormat::R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case ETextureFormat::BC1_UNORM:             return DXGI_FORMAT_BC1_UNORM;
        case ETextureFormat::BC1_UNORM_SRGB:        return DXGI_FORMAT_BC1_UNORM_SRGB;
        case ETextureFormat::BC3_UNORM:             return DXGI_FORMAT_BC3_UNORM;
        case ETextureFormat::BC3_UNORM_SRGB:        return DXGI_FORMAT_BC3_UNORM_SRGB;
        case ETextureFormat::BC5_UNORM:             return DXGI_FORMAT_BC5_UNORM;
        case ETextureFormat::BC7_UNORM:             return DXGI_FORMAT_BC7_UNORM;
        case ETextureFormat::BC7_UNORM_SRGB:        return DXGI_FORMAT_BC7_UNORM_SRGB;
        case ETextureFormat::R32_UINT:              return DXGI_FORMAT_R32_UINT;
        case ETextureFormat::R32G32_UINT:           return DXGI_FORMAT_R32G32_UINT;
        case ETextureFormat::R32G32B32A32_UINT:     return DXGI_FORMAT_R32G32B32A32_UINT;
        default:
            CFFLog::Warning("[DX12] Unknown texture format: %d", static_cast<int>(format));
            return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_RESOURCE_STATES ToD3D12ResourceState(EResourceState state) {
    switch (state) {
        case EResourceState::Common:          return D3D12_RESOURCE_STATE_COMMON;
        case EResourceState::RenderTarget:    return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case EResourceState::DepthWrite:      return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case EResourceState::DepthRead:       return D3D12_RESOURCE_STATE_DEPTH_READ;
        case EResourceState::ShaderResource:  return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case EResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case EResourceState::CopySource:      return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case EResourceState::CopyDest:        return D3D12_RESOURCE_STATE_COPY_DEST;
        case EResourceState::Present:         return D3D12_RESOURCE_STATE_PRESENT;
        default:                              return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12_RESOURCE_FLAGS GetResourceFlags(ETextureUsage usage) {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

    if (usage & ETextureUsage::RenderTarget) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (usage & ETextureUsage::DepthStencil) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if (usage & ETextureUsage::UnorderedAccess) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    // If depth stencil without SRV, deny shader resource
    if ((usage & ETextureUsage::DepthStencil) && !(usage & ETextureUsage::ShaderResource)) {
        flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }

    return flags;
}

D3D12_HEAP_TYPE GetHeapType(ECPUAccess cpuAccess, EBufferUsage usage) {
    switch (cpuAccess) {
        case ECPUAccess::Write:
            return D3D12_HEAP_TYPE_UPLOAD;
        case ECPUAccess::Read:
            return D3D12_HEAP_TYPE_READBACK;
        default:
            return D3D12_HEAP_TYPE_DEFAULT;
    }
}

D3D12_RESOURCE_STATES GetInitialResourceState(D3D12_HEAP_TYPE heapType, EBufferUsage bufferUsage) {
    switch (heapType) {
        case D3D12_HEAP_TYPE_UPLOAD:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_HEAP_TYPE_READBACK:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        default:
            // Default heap - start in common state
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12_RESOURCE_STATES GetInitialResourceState(D3D12_HEAP_TYPE heapType, ETextureUsage textureUsage) {
    switch (heapType) {
        case D3D12_HEAP_TYPE_UPLOAD:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_HEAP_TYPE_READBACK:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        default:
            // Default heap - start in common state
            // DX12 will implicitly promote as needed
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

} // namespace DX12
} // namespace RHI
