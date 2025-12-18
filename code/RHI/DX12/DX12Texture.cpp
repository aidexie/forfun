#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "DX12Context.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// CDX12Texture Implementation
// ============================================

CDX12Texture::CDX12Texture(ID3D12Resource* resource, const TextureDesc& desc, ID3D12Device* device)
    : m_resource(resource)
    , m_desc(desc)
    , m_device(device)
{
    // Initial state matches what was used in CreateCommittedResource
    // For DEFAULT heap, DX12 creates resources in COMMON state
    // Staging textures (UPLOAD/READBACK) have different initial states
    if (desc.usage & ETextureUsage::Staging) {
        if (desc.cpuAccess == ECPUAccess::Read) {
            m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
        } else {
            m_currentState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
    } else {
        // DEFAULT heap resources start in COMMON state
        m_currentState = D3D12_RESOURCE_STATE_COMMON;
    }
}

CDX12Texture::~CDX12Texture() {
    auto& heapMgr = CDX12DescriptorHeapManager::Instance();

    // Free default views - SRVs/UAVs are in CPU heap, RTV/DSV are in their own heaps
    if (m_defaultSRV.IsValid()) heapMgr.FreeCBVSRVUAV(m_defaultSRV);
    if (m_defaultRTV.IsValid()) heapMgr.FreeRTV(m_defaultRTV);
    if (m_defaultDSV.IsValid()) heapMgr.FreeDSV(m_defaultDSV);
    if (m_defaultUAV.IsValid()) heapMgr.FreeCBVSRVUAV(m_defaultUAV);

    // Free cached views
    for (auto& [key, handle] : m_srvCache) {
        heapMgr.FreeCBVSRVUAV(handle);
    }
    for (auto& [key, handle] : m_rtvCache) {
        heapMgr.FreeRTV(handle);
    }
    for (auto& [slice, handle] : m_dsvCache) {
        heapMgr.FreeDSV(handle);
    }
    for (auto& [mip, handle] : m_uavCache) {
        heapMgr.FreeCBVSRVUAV(handle);
    }

    // Defer release of the D3D12 resource until GPU is done using it
    // This prevents "resource deleted while still in use" errors
    if (m_resource) {
        CDX12Context::Instance().DeferredRelease(m_resource.Get());
    }
}

MappedTexture CDX12Texture::Map(uint32_t arraySlice, uint32_t mipLevel) {
    MappedTexture result;

    if (!(m_desc.usage & ETextureUsage::Staging)) {
        CFFLog::Error("[CDX12Texture] Cannot map non-staging texture");
        return result;
    }

    // For staging textures, we created a buffer (not a texture) in DX12
    // Buffers always use subresource 0
    D3D12_RESOURCE_DESC resDesc = m_resource->GetDesc();
    bool isBuffer = (resDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

    UINT subresource = isBuffer ? 0 : CalcSubresource(mipLevel, arraySlice, 0, m_desc.mipLevels, m_desc.arraySize);

    D3D12_RANGE readRange = {};
    if (m_desc.cpuAccess == ECPUAccess::Write) {
        readRange = { 0, 0 };  // No read
    }

    void* mappedData = nullptr;
    HRESULT hr = m_resource->Map(subresource, &readRange, &mappedData);
    if (FAILED(hr)) {
        CFFLog::Error("[CDX12Texture] Map failed: %s", HRESULTToString(hr).c_str());
        return result;
    }

    if (isBuffer) {
        // For staging buffers, we need to calculate the offset for the specific subresource
        // Create a temporary texture desc to get proper footprints
        D3D12_RESOURCE_DESC tempDesc = {};
        tempDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tempDesc.Width = m_desc.width;
        tempDesc.Height = m_desc.height;
        tempDesc.DepthOrArraySize = static_cast<UINT16>(m_desc.arraySize);
        tempDesc.MipLevels = static_cast<UINT16>(m_desc.mipLevels);
        tempDesc.Format = ToDXGIFormat(m_desc.format);
        tempDesc.SampleDesc.Count = 1;
        tempDesc.SampleDesc.Quality = 0;
        tempDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tempDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        UINT targetSubresource = CalcSubresource(mipLevel, arraySlice, 0, m_desc.mipLevels, m_desc.arraySize);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT numRows;
        UINT64 rowSizeInBytes;
        m_device->GetCopyableFootprints(&tempDesc, targetSubresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, nullptr);

        result.pData = static_cast<uint8_t*>(mappedData) + footprint.Offset;
        result.rowPitch = footprint.Footprint.RowPitch;
        result.depthPitch = footprint.Footprint.RowPitch * footprint.Footprint.Height;
    } else {
        // Regular texture map
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT numRows;
        UINT64 rowSizeInBytes;
        UINT64 totalBytes;
        m_device->GetCopyableFootprints(&resDesc, subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

        result.pData = mappedData;
        result.rowPitch = footprint.Footprint.RowPitch;
        result.depthPitch = footprint.Footprint.RowPitch * footprint.Footprint.Height;
    }

    return result;
}

void CDX12Texture::Unmap(uint32_t arraySlice, uint32_t mipLevel) {
    if (!(m_desc.usage & ETextureUsage::Staging)) {
        return;
    }

    // For staging textures, we created a buffer (not a texture) in DX12
    D3D12_RESOURCE_DESC resDesc = m_resource->GetDesc();
    bool isBuffer = (resDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

    UINT subresource = isBuffer ? 0 : CalcSubresource(mipLevel, arraySlice, 0, m_desc.mipLevels, m_desc.arraySize);

    D3D12_RANGE writtenRange = {};
    if (m_desc.cpuAccess == ECPUAccess::Write) {
        if (isBuffer) {
            // For staging buffer, the entire buffer was potentially written
            writtenRange = { 0, static_cast<SIZE_T>(resDesc.Width) };
        } else {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
            UINT64 totalBytes;
            m_device->GetCopyableFootprints(&resDesc, subresource, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
            writtenRange = { 0, static_cast<SIZE_T>(totalBytes) };
        }
    }

    m_resource->Unmap(subresource, &writtenRange);
}

SDescriptorHandle CDX12Texture::GetOrCreateSRV() {
    if (!m_defaultSRV.IsValid()) {
        uint32_t numSlices = m_desc.arraySize;
        if (m_desc.dimension == ETextureDimension::TexCube) {
            numSlices = 6;
        } else if (m_desc.dimension == ETextureDimension::TexCubeArray) {
            numSlices = m_desc.arraySize * 6;
        }
        m_defaultSRV = CreateSRV(0, m_desc.mipLevels, 0, numSlices);
    }
    return m_defaultSRV;
}

SDescriptorHandle CDX12Texture::GetOrCreateSRVSlice(uint32_t arraySlice, uint32_t mipLevel) {
    ViewKey key = { mipLevel, arraySlice };
    auto it = m_srvCache.find(key);
    if (it != m_srvCache.end()) {
        return it->second;
    }

    SDescriptorHandle handle = CreateSRV(mipLevel, 1, arraySlice, 1);
    m_srvCache[key] = handle;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Texture::GetOrCreateRTV() {
    if (!m_defaultRTV.IsValid()) {
        m_defaultRTV = CreateRTV(0, 0);
    }
    return m_defaultRTV.cpuHandle;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Texture::GetOrCreateRTVSlice(uint32_t arraySlice, uint32_t mipLevel) {
    ViewKey key = { mipLevel, arraySlice };
    auto it = m_rtvCache.find(key);
    if (it != m_rtvCache.end()) {
        return it->second.cpuHandle;
    }

    SDescriptorHandle handle = CreateRTV(mipLevel, arraySlice);
    m_rtvCache[key] = handle;
    return handle.cpuHandle;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Texture::GetOrCreateDSV() {
    if (!m_defaultDSV.IsValid()) {
        m_defaultDSV = CreateDSV(0);
    }
    return m_defaultDSV.cpuHandle;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Texture::GetOrCreateDSVSlice(uint32_t arraySlice) {
    auto it = m_dsvCache.find(arraySlice);
    if (it != m_dsvCache.end()) {
        return it->second.cpuHandle;
    }

    SDescriptorHandle handle = CreateDSV(arraySlice);
    m_dsvCache[arraySlice] = handle;
    return handle.cpuHandle;
}

SDescriptorHandle CDX12Texture::GetOrCreateUAV() {
    if (!m_defaultUAV.IsValid()) {
        m_defaultUAV = CreateUAV(0);
    }
    return m_defaultUAV;
}

SDescriptorHandle CDX12Texture::GetOrCreateUAVSlice(uint32_t mipLevel) {
    // Mip 0 uses the default UAV
    if (mipLevel == 0) {
        return GetOrCreateUAV();
    }

    // Check cache for this mip level
    auto it = m_uavCache.find(mipLevel);
    if (it != m_uavCache.end()) {
        return it->second;
    }

    // Create new UAV for this mip level
    SDescriptorHandle handle = CreateUAV(mipLevel);
    m_uavCache[mipLevel] = handle;
    return handle;
}

SDescriptorHandle CDX12Texture::CreateSRV(uint32_t mipLevel, uint32_t numMips, uint32_t arraySlice, uint32_t numSlices) {
    // Allocate from CPU-only heap - will be copied to GPU staging at draw time
    SDescriptorHandle handle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
    if (!handle.IsValid()) {
        CFFLog::Error("[CDX12Texture] Failed to allocate SRV descriptor");
        return handle;
    }

    DXGI_FORMAT format = (m_desc.srvFormat != ETextureFormat::Unknown) ?
                         ToDXGIFormat(m_desc.srvFormat) : ToDXGIFormat(m_desc.format);

    // In DX12, MipLevels = 0 is invalid. Use -1 (UINT_MAX) to indicate "all mips from MostDetailedMip"
    // or use the actual mip count if numMips was 0
    UINT srvMipLevels = (numMips == 0) ? static_cast<UINT>(-1) : numMips;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    switch (m_desc.dimension) {
        case ETextureDimension::Tex2D:
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = mipLevel;
            srvDesc.Texture2D.MipLevels = srvMipLevels;
            srvDesc.Texture2D.PlaneSlice = 0;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            break;

        case ETextureDimension::Tex2DArray:
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = mipLevel;
            srvDesc.Texture2DArray.MipLevels = srvMipLevels;
            srvDesc.Texture2DArray.FirstArraySlice = arraySlice;
            srvDesc.Texture2DArray.ArraySize = numSlices;
            srvDesc.Texture2DArray.PlaneSlice = 0;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            break;

        case ETextureDimension::Tex3D:
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srvDesc.Texture3D.MostDetailedMip = mipLevel;
            srvDesc.Texture3D.MipLevels = srvMipLevels;
            srvDesc.Texture3D.ResourceMinLODClamp = 0.0f;
            break;

        case ETextureDimension::TexCube:
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = mipLevel;
            srvDesc.TextureCube.MipLevels = srvMipLevels;
            srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            break;

        case ETextureDimension::TexCubeArray:
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.TextureCubeArray.MostDetailedMip = mipLevel;
            srvDesc.TextureCubeArray.MipLevels = srvMipLevels;
            srvDesc.TextureCubeArray.First2DArrayFace = arraySlice;
            srvDesc.TextureCubeArray.NumCubes = numSlices / 6;
            srvDesc.TextureCubeArray.ResourceMinLODClamp = 0.0f;
            break;
    }

    m_device->CreateShaderResourceView(m_resource.Get(), &srvDesc, handle.cpuHandle);
    return handle;
}

SDescriptorHandle CDX12Texture::CreateRTV(uint32_t mipLevel, uint32_t arraySlice) {
    SDescriptorHandle handle = CDX12DescriptorHeapManager::Instance().AllocateRTV();
    if (!handle.IsValid()) {
        CFFLog::Error("[CDX12Texture] Failed to allocate RTV descriptor");
        return handle;
    }

    DXGI_FORMAT format = (m_desc.rtvFormat != ETextureFormat::Unknown) ?
                         ToDXGIFormat(m_desc.rtvFormat) : ToDXGIFormat(m_desc.format);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;

    bool isArray = m_desc.dimension == ETextureDimension::Tex2DArray ||
                   m_desc.dimension == ETextureDimension::TexCube ||
                   m_desc.dimension == ETextureDimension::TexCubeArray;

    if (isArray) {
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = mipLevel;
        rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.PlaneSlice = 0;
    } else if (m_desc.dimension == ETextureDimension::Tex3D) {
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rtvDesc.Texture3D.MipSlice = mipLevel;
        rtvDesc.Texture3D.FirstWSlice = arraySlice;
        rtvDesc.Texture3D.WSize = 1;
    } else {
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = mipLevel;
        rtvDesc.Texture2D.PlaneSlice = 0;
    }

    m_device->CreateRenderTargetView(m_resource.Get(), &rtvDesc, handle.cpuHandle);
    return handle;
}

SDescriptorHandle CDX12Texture::CreateDSV(uint32_t arraySlice) {
    SDescriptorHandle handle = CDX12DescriptorHeapManager::Instance().AllocateDSV();
    if (!handle.IsValid()) {
        CFFLog::Error("[CDX12Texture] Failed to allocate DSV descriptor");
        return handle;
    }

    DXGI_FORMAT format = (m_desc.dsvFormat != ETextureFormat::Unknown) ?
                         ToDXGIFormat(m_desc.dsvFormat) : ToDXGIFormat(m_desc.format);

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = format;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    bool isArray = m_desc.dimension == ETextureDimension::Tex2DArray ||
                   m_desc.dimension == ETextureDimension::TexCube ||
                   m_desc.dimension == ETextureDimension::TexCubeArray;

    if (isArray) {
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = arraySlice;
        dsvDesc.Texture2DArray.ArraySize = 1;
    } else {
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = 0;
    }

    m_device->CreateDepthStencilView(m_resource.Get(), &dsvDesc, handle.cpuHandle);
    return handle;
}

SDescriptorHandle CDX12Texture::CreateUAV(uint32_t mipLevel) {
    // Allocate from CPU-only heap - will be copied to GPU staging at draw time
    SDescriptorHandle handle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
    if (!handle.IsValid()) {
        CFFLog::Error("[CDX12Texture] Failed to allocate UAV descriptor");
        return handle;
    }

    DXGI_FORMAT format = (m_desc.uavFormat != ETextureFormat::Unknown) ?
                         ToDXGIFormat(m_desc.uavFormat) : ToDXGIFormat(m_desc.format);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = format;

    switch (m_desc.dimension) {
        case ETextureDimension::Tex2D:
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = mipLevel;
            uavDesc.Texture2D.PlaneSlice = 0;
            break;

        case ETextureDimension::Tex2DArray:
        case ETextureDimension::TexCube:
        case ETextureDimension::TexCubeArray:
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDesc.Texture2DArray.MipSlice = mipLevel;
            uavDesc.Texture2DArray.FirstArraySlice = 0;
            uavDesc.Texture2DArray.ArraySize = m_desc.arraySize;
            uavDesc.Texture2DArray.PlaneSlice = 0;
            break;

        case ETextureDimension::Tex3D:
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uavDesc.Texture3D.MipSlice = mipLevel;
            uavDesc.Texture3D.FirstWSlice = 0;
            uavDesc.Texture3D.WSize = m_desc.depth >> mipLevel;
            break;
    }

    m_device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, handle.cpuHandle);
    return handle;
}

// ============================================
// CDX12Sampler Implementation
// ============================================

CDX12Sampler::~CDX12Sampler() {
    if (m_handle.IsValid()) {
        CDX12DescriptorHeapManager::Instance().FreeSampler(m_handle);
    }
}

// ============================================
// CDX12Shader Implementation
// ============================================

CDX12Shader::CDX12Shader(EShaderType type, const void* bytecode, size_t bytecodeSize)
    : m_type(type)
{
    m_bytecode.resize(bytecodeSize);
    memcpy(m_bytecode.data(), bytecode, bytecodeSize);
}

// ============================================
// CDX12PipelineState Implementation
// ============================================

CDX12PipelineState::CDX12PipelineState(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig, bool isCompute)
    : m_pso(pso)
    , m_rootSignature(rootSig)
    , m_isCompute(isCompute)
{
}

} // namespace DX12
} // namespace RHI
