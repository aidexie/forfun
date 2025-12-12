#include "DX11Resources.h"
#include "DX11Utils.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX11 {

// ============================================
// Buffer View Creation Implementation
// ============================================

ID3D11ShaderResourceView* CDX11Buffer::GetOrCreateSRV() {
    if (m_srv) {
        return m_srv.Get();
    }

    if (!m_device || !m_buffer) {
        return nullptr;
    }

    // Only structured buffers can have SRV
    if (!(m_desc.usage & EBufferUsage::Structured)) {
        CFFLog::Error("CDX11Buffer::GetOrCreateSRV: Buffer is not a structured buffer");
        return nullptr;
    }

    if (m_desc.structureByteStride == 0) {
        CFFLog::Error("CDX11Buffer::GetOrCreateSRV: structureByteStride is 0");
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;  // Must be UNKNOWN for structured buffers
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = m_desc.size / m_desc.structureByteStride;

    HRESULT hr = m_device->CreateShaderResourceView(m_buffer.Get(), &srvDesc, &m_srv);
    if (FAILED(hr)) {
        CFFLog::Error("CDX11Buffer::GetOrCreateSRV failed: %s", HRESULTToString(hr).c_str());
        CFFLog::Error("  Size: %u, Stride: %u, NumElements: %u",
                      m_desc.size, m_desc.structureByteStride, srvDesc.Buffer.NumElements);
        return nullptr;
    }

    return m_srv.Get();
}

ID3D11UnorderedAccessView* CDX11Buffer::GetOrCreateUAV() {
    if (m_uav) {
        return m_uav.Get();
    }

    if (!m_device || !m_buffer) {
        return nullptr;
    }

    // Check if buffer supports UAV
    if (!(m_desc.usage & EBufferUsage::UnorderedAccess)) {
        CFFLog::Error("CDX11Buffer::GetOrCreateUAV: Buffer does not have UnorderedAccess usage");
        return nullptr;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;

    if (m_desc.usage & EBufferUsage::Structured) {
        // Structured buffer UAV
        if (m_desc.structureByteStride == 0) {
            CFFLog::Error("CDX11Buffer::GetOrCreateUAV: structureByteStride is 0 for structured buffer");
            return nullptr;
        }
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.Buffer.NumElements = m_desc.size / m_desc.structureByteStride;
        uavDesc.Buffer.Flags = 0;
    } else {
        // Raw buffer UAV (ByteAddressBuffer)
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.Buffer.NumElements = m_desc.size / 4;  // 4 bytes per element
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    }

    HRESULT hr = m_device->CreateUnorderedAccessView(m_buffer.Get(), &uavDesc, &m_uav);
    if (FAILED(hr)) {
        CFFLog::Error("CDX11Buffer::GetOrCreateUAV failed: %s", HRESULTToString(hr).c_str());
        CFFLog::Error("  Size: %u, Stride: %u, NumElements: %u",
                      m_desc.size, m_desc.structureByteStride, uavDesc.Buffer.NumElements);
        return nullptr;
    }

    return m_uav.Get();
}

} // namespace DX11
} // namespace RHI
