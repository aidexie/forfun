#include "DX11CommandList.h"
#include "DX11Resources.h"
#include "DX11Utils.h"
#include "../../Core/FFLog.h"
#include <d3d11_1.h>  // For ID3DUserDefinedAnnotation

// Enable this to log draw calls for debugging shader linkage errors
#define DEBUG_DRAW_CALLS 0

#if DEBUG_DRAW_CALLS
#include <Windows.h>
#include <cstdio>

static void DebugDrawLog(const wchar_t* eventName, const char* drawType, void* pso) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "[Draw] %s in event: %ls, PSO: %p\n", drawType, eventName, pso);
    OutputDebugStringA(buffer);
    CFFLog::Info("%s", buffer);
}
#endif

namespace RHI {
namespace DX11 {

// Align size to 16 bytes for constant buffers
static size_t AlignCBSize(size_t size) {
    return (size + 15) & ~15;
}

CDX11CommandList::CDX11CommandList(ID3D11DeviceContext* context, ID3D11Device* device)
    : m_context(context)
    , m_device(device)
{
    // Query for annotation interface for debug events
    if (m_context) {
        m_context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), (void**)&m_annotation);
    }
}

CDX11CommandList::~CDX11CommandList() {
    if (m_annotation) {
        m_annotation->Release();
        m_annotation = nullptr;
    }
}

void CDX11CommandList::ResetFrame() {
    // Reset all pool indices for the new frame
    for (auto& pair : m_dynamicCBPools) {
        pair.second.nextIndex = 0;
    }
}

ID3D11Buffer* CDX11CommandList::AcquireDynamicCB(size_t size) {
    size_t alignedSize = AlignCBSize(size);
    auto& pool = m_dynamicCBPools[alignedSize];

    // If we have a buffer available, use it
    if (pool.nextIndex < pool.buffers.size()) {
        return pool.buffers[pool.nextIndex++].Get();
    }

    // Need to create a new buffer
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(alignedSize);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = m_device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr)) {
        return nullptr;
    }

    pool.buffers.push_back(buffer);
    pool.nextIndex++;
    return buffer.Get();
}

void CDX11CommandList::SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil) {
    ID3D11RenderTargetView* rtvs[8] = {nullptr};

    for (uint32_t i = 0; i < numRTs && i < 8; i++) {
        if (renderTargets[i]) {
            CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(renderTargets[i]);
            rtvs[i] = dx11Tex->GetOrCreateRTV();
        }
    }

    ID3D11DepthStencilView* dsv = nullptr;
    if (depthStencil) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(depthStencil);
        dsv = dx11Tex->GetOrCreateDSV();
    }

    m_context->OMSetRenderTargets(numRTs, rtvs, dsv);
}

void CDX11CommandList::ClearRenderTarget(ITexture* renderTarget, const float color[4]) {
    if (!renderTarget) return;
    CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(renderTarget);
    ID3D11RenderTargetView* rtv = dx11Tex->GetOrCreateRTV();
    if (rtv) {
        m_context->ClearRenderTargetView(rtv, color);
    }
}

void CDX11CommandList::ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) {
    if (!depthStencil) return;
    CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(depthStencil);
    ID3D11DepthStencilView* dsv = dx11Tex->GetOrCreateDSV();
    if (dsv) {
        UINT flags = 0;
        if (clearDepth) flags |= D3D11_CLEAR_DEPTH;
        if (clearStencil) flags |= D3D11_CLEAR_STENCIL;
        m_context->ClearDepthStencilView(dsv, flags, depth, stencil);
    }
}

void CDX11CommandList::SetRenderTargetSlice(ITexture* renderTarget, uint32_t arraySlice, ITexture* depthStencil) {
    ID3D11RenderTargetView* rtv = nullptr;
    if (renderTarget) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(renderTarget);
        rtv = dx11Tex->GetOrCreateRTVSlice(arraySlice, 0);
    }

    ID3D11DepthStencilView* dsv = nullptr;
    if (depthStencil) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(depthStencil);
        dsv = dx11Tex->GetOrCreateDSV();
    }

    m_context->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
}

void CDX11CommandList::SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice) {
    ID3D11DepthStencilView* dsv = nullptr;
    if (depthStencil) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(depthStencil);
        if (depthStencil->GetArraySize() > 1) {
            dsv = dx11Tex->GetOrCreateDSVSlice(arraySlice);
        } else {
            dsv = dx11Tex->GetOrCreateDSV();
        }
    }
    m_context->OMSetRenderTargets(0, nullptr, dsv);
}

void CDX11CommandList::ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) {
    if (!depthStencil) return;
    CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(depthStencil);
    ID3D11DepthStencilView* dsv = dx11Tex->GetOrCreateDSVSlice(arraySlice);
    if (dsv) {
        UINT flags = 0;
        if (clearDepth) flags |= D3D11_CLEAR_DEPTH;
        if (clearStencil) flags |= D3D11_CLEAR_STENCIL;
        m_context->ClearDepthStencilView(dsv, flags, depth, stencil);
    }
}

void CDX11CommandList::SetPipelineState(IPipelineState* pso) {
    if (!pso) return;

    m_currentPSO = pso;  // Track current PSO for debug

    CDX11PipelineState* d3dPSO = static_cast<CDX11PipelineState*>(pso);

    // Check if it's a compute pipeline
    if (d3dPSO->GetComputeShader()) {
        m_context->CSSetShader(d3dPSO->GetComputeShader()->GetComputeShader(), nullptr, 0);
        return;
    }

    // Graphics pipeline
    m_context->IASetInputLayout(d3dPSO->GetInputLayout());
    m_context->IASetPrimitiveTopology(d3dPSO->GetTopology());
    m_context->RSSetState(d3dPSO->GetRasterizerState());
    m_context->OMSetDepthStencilState(d3dPSO->GetDepthStencilState(), 0);
    m_context->OMSetBlendState(d3dPSO->GetBlendState(), nullptr, 0xFFFFFFFF);

    if (d3dPSO->GetVertexShader()) {
        m_context->VSSetShader(d3dPSO->GetVertexShader()->GetVertexShader(), nullptr, 0);
    }
    if (d3dPSO->GetPixelShader()) {
        m_context->PSSetShader(d3dPSO->GetPixelShader()->GetPixelShader(), nullptr, 0);
    } else {
        // Clear PS to avoid linkage errors with depth-only passes (e.g., ShadowPass)
        m_context->PSSetShader(nullptr, nullptr, 0);
    }
    if (d3dPSO->GetGeometryShader()) {
        m_context->GSSetShader(d3dPSO->GetGeometryShader()->GetGeometryShader(), nullptr, 0);
    } else {
        m_context->GSSetShader(nullptr, nullptr, 0);
    }
    if (d3dPSO->GetHullShader()) {
        m_context->HSSetShader(d3dPSO->GetHullShader()->GetHullShader(), nullptr, 0);
    } else {
        m_context->HSSetShader(nullptr, nullptr, 0);
    }
    if (d3dPSO->GetDomainShader()) {
        m_context->DSSetShader(d3dPSO->GetDomainShader()->GetDomainShader(), nullptr, 0);
    } else {
        m_context->DSSetShader(nullptr, nullptr, 0);
    }
}

void CDX11CommandList::SetPrimitiveTopology(EPrimitiveTopology topology) {
    m_context->IASetPrimitiveTopology(ToD3D11Topology(topology));
}

void CDX11CommandList::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth) {
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = x;
    viewport.TopLeftY = y;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = minDepth;
    viewport.MaxDepth = maxDepth;
    m_context->RSSetViewports(1, &viewport);
}

void CDX11CommandList::SetScissorRect(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    D3D11_RECT rect = {};
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = bottom;
    m_context->RSSetScissorRects(1, &rect);
}

void CDX11CommandList::SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset) {
    ID3D11Buffer* d3dBuffer = buffer ? static_cast<CDX11Buffer*>(buffer)->GetD3D11Buffer() : nullptr;
    m_context->IASetVertexBuffers(slot, 1, &d3dBuffer, &stride, &offset);
}

void CDX11CommandList::SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset) {
    ID3D11Buffer* d3dBuffer = buffer ? static_cast<CDX11Buffer*>(buffer)->GetD3D11Buffer() : nullptr;
    m_context->IASetIndexBuffer(d3dBuffer, ToDXGIFormat(format), offset);
}

#ifndef FF_LEGACY_BINDING_DISABLED
bool CDX11CommandList::SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) {
    if (!data || size == 0) return false;

    // Get a dynamic constant buffer from the pool
    ID3D11Buffer* buffer = AcquireDynamicCB(size);
    if (!buffer) return false;

    // Map, copy data, unmap
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    memcpy(mapped.pData, data, size);
    m_context->Unmap(buffer, 0);

    // Bind the buffer
    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetConstantBuffers(slot, 1, &buffer);
            break;
        case EShaderStage::Pixel:
            m_context->PSSetConstantBuffers(slot, 1, &buffer);
            break;
        case EShaderStage::Compute:
            m_context->CSSetConstantBuffers(slot, 1, &buffer);
            break;
        case EShaderStage::Geometry:
            m_context->GSSetConstantBuffers(slot, 1, &buffer);
            break;
        case EShaderStage::Hull:
            m_context->HSSetConstantBuffers(slot, 1, &buffer);
            break;
        case EShaderStage::Domain:
            m_context->DSSetConstantBuffers(slot, 1, &buffer);
            break;
    }

    return true;
}

void CDX11CommandList::SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) {
    ID3D11ShaderResourceView* srv = nullptr;
    if (texture) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(texture);
        srv = dx11Tex->GetOrCreateSRV();
    }

    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Pixel:
            m_context->PSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Compute:
            m_context->CSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Geometry:
            m_context->GSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Hull:
            m_context->HSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Domain:
            m_context->DSSetShaderResources(slot, 1, &srv);
            break;
    }
}

void CDX11CommandList::SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) {
    ID3D11ShaderResourceView* srv = nullptr;

    if (buffer) {
        CDX11Buffer* dx11Buffer = static_cast<CDX11Buffer*>(buffer);
        srv = dx11Buffer->GetOrCreateSRV();
    }

    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Pixel:
            m_context->PSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Compute:
            m_context->CSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Geometry:
            m_context->GSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Hull:
            m_context->HSSetShaderResources(slot, 1, &srv);
            break;
        case EShaderStage::Domain:
            m_context->DSSetShaderResources(slot, 1, &srv);
            break;
    }
}

void CDX11CommandList::SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) {
    ID3D11SamplerState* d3dSampler = sampler ? static_cast<CDX11Sampler*>(sampler)->GetD3D11Sampler() : nullptr;

    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetSamplers(slot, 1, &d3dSampler);
            break;
        case EShaderStage::Pixel:
        if(d3dSampler)
            m_context->PSSetSamplers(slot, 1, &d3dSampler);
            break;
        case EShaderStage::Compute:
            m_context->CSSetSamplers(slot, 1, &d3dSampler);
            break;
        case EShaderStage::Geometry:
            m_context->GSSetSamplers(slot, 1, &d3dSampler);
            break;
    }
}

void CDX11CommandList::SetUnorderedAccess(uint32_t slot, IBuffer* buffer) {
    ID3D11UnorderedAccessView* uav = nullptr;

    if (buffer) {
        CDX11Buffer* dx11Buffer = static_cast<CDX11Buffer*>(buffer);
        uav = dx11Buffer->GetOrCreateUAV();
    }

    m_context->CSSetUnorderedAccessViews(slot, 1, &uav, nullptr);
}

void CDX11CommandList::SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) {
    ID3D11UnorderedAccessView* uav = nullptr;
    if (texture) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(texture);
        uav = dx11Tex->GetOrCreateUAV();
    }
    m_context->CSSetUnorderedAccessViews(slot, 1, &uav, nullptr);
}

void CDX11CommandList::SetUnorderedAccessTextureMip(uint32_t slot, ITexture* texture, uint32_t mipLevel) {
    ID3D11UnorderedAccessView* uav = nullptr;
    if (texture) {
        CDX11Texture* dx11Tex = static_cast<CDX11Texture*>(texture);
        uav = dx11Tex->GetOrCreateUAVSlice(mipLevel);
    }
    m_context->CSSetUnorderedAccessViews(slot, 1, &uav, nullptr);
}
#endif // FF_LEGACY_BINDING_DISABLED

void CDX11CommandList::ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) {
    if (!buffer) return;

    CDX11Buffer* dx11Buffer = static_cast<CDX11Buffer*>(buffer);
    ID3D11UnorderedAccessView* uav = dx11Buffer->GetOrCreateUAV();
    if (uav) {
        m_context->ClearUnorderedAccessViewUint(uav, values);
    }
}

void CDX11CommandList::Draw(uint32_t vertexCount, uint32_t startVertex) {
#if DEBUG_DRAW_CALLS
    static const wchar_t* s_lastEvent = nullptr;
    if (m_currentEventName && m_currentEventName != s_lastEvent) {
        DebugDrawLog(m_currentEventName, "Draw", m_currentPSO);
        s_lastEvent = m_currentEventName;
    }
#endif
    m_context->Draw(vertexCount, startVertex);
}

void CDX11CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) {
#if DEBUG_DRAW_CALLS
    static const wchar_t* s_lastEvent = nullptr;
    if (m_currentEventName && m_currentEventName != s_lastEvent) {
        DebugDrawLog(m_currentEventName, "DrawIndexed", m_currentPSO);
        s_lastEvent = m_currentEventName;
    }
#endif
    m_context->DrawIndexed(indexCount, startIndex, baseVertex);
}

void CDX11CommandList::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                                     uint32_t startVertex, uint32_t startInstance) {
#if DEBUG_DRAW_CALLS
    static const wchar_t* s_lastEvent = nullptr;
    if (m_currentEventName && m_currentEventName != s_lastEvent) {
        DebugDrawLog(m_currentEventName, "DrawInstanced", m_currentPSO);
        s_lastEvent = m_currentEventName;
    }
#endif
    m_context->DrawInstanced(vertexCountPerInstance, instanceCount, startVertex, startInstance);
}

void CDX11CommandList::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                                            uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) {
#if DEBUG_DRAW_CALLS
    static const wchar_t* s_lastEvent = nullptr;
    if (m_currentEventName && m_currentEventName != s_lastEvent) {
        DebugDrawLog(m_currentEventName, "DrawIndexedInstanced", m_currentPSO);
        s_lastEvent = m_currentEventName;
    }
#endif
    m_context->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
}

void CDX11CommandList::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) {
    m_context->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

void CDX11CommandList::Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter) {
    // DX11 handles resource transitions automatically - no-op
}

void CDX11CommandList::UAVBarrier(IResource* resource) {
    // DX11 handles UAV barriers automatically - no-op
}

void CDX11CommandList::CopyTexture(ITexture* dst, ITexture* src) {
    if (!dst || !src) return;
    ID3D11Resource* dstRes = static_cast<ID3D11Resource*>(dst->GetNativeHandle());
    ID3D11Resource* srcRes = static_cast<ID3D11Resource*>(src->GetNativeHandle());
    if (dstRes && srcRes) {
        m_context->CopyResource(dstRes, srcRes);
    }
}

void CDX11CommandList::CopyTextureToSlice(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src) {
    if (!dst || !src) return;
    ID3D11Resource* dstRes = static_cast<ID3D11Resource*>(dst->GetNativeHandle());
    ID3D11Resource* srcRes = static_cast<ID3D11Resource*>(src->GetNativeHandle());
    if (!dstRes || !srcRes) return;

    // Calculate destination subresource
    UINT dstMipLevels = dst->GetMipLevels();
    UINT dstSubresource = D3D11CalcSubresource(dstMipLevel, dstArraySlice, dstMipLevels);
    m_context->CopySubresourceRegion(dstRes, dstSubresource, 0, 0, 0, srcRes, 0, nullptr);
}

void CDX11CommandList::CopyTextureSubresource(
    ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel,
    ITexture* src, uint32_t srcArraySlice, uint32_t srcMipLevel)
{
    if (!dst || !src) return;
    ID3D11Resource* dstRes = static_cast<ID3D11Resource*>(dst->GetNativeHandle());
    ID3D11Resource* srcRes = static_cast<ID3D11Resource*>(src->GetNativeHandle());
    if (!dstRes || !srcRes) return;

    UINT dstMipLevels = dst->GetMipLevels();
    UINT srcMipLevels = src->GetMipLevels();
    UINT dstSubresource = D3D11CalcSubresource(dstMipLevel, dstArraySlice, dstMipLevels);
    UINT srcSubresource = D3D11CalcSubresource(srcMipLevel, srcArraySlice, srcMipLevels);

    m_context->CopySubresourceRegion(dstRes, dstSubresource, 0, 0, 0, srcRes, srcSubresource, nullptr);
}

void CDX11CommandList::CopyBuffer(IBuffer* dst, uint64_t dstOffset, IBuffer* src, uint64_t srcOffset, uint64_t numBytes) {
    if (!dst || !src || numBytes == 0) return;

    ID3D11Resource* dstRes = static_cast<ID3D11Resource*>(dst->GetNativeHandle());
    ID3D11Resource* srcRes = static_cast<ID3D11Resource*>(src->GetNativeHandle());
    if (!dstRes || !srcRes) return;

    D3D11_BOX srcBox;
    srcBox.left = static_cast<UINT>(srcOffset);
    srcBox.right = static_cast<UINT>(srcOffset + numBytes);
    srcBox.top = 0;
    srcBox.bottom = 1;
    srcBox.front = 0;
    srcBox.back = 1;

    m_context->CopySubresourceRegion(dstRes, 0, static_cast<UINT>(dstOffset), 0, 0, srcRes, 0, &srcBox);
}

void CDX11CommandList::UnbindRenderTargets() {
    ID3D11RenderTargetView* nullRTV = nullptr;
    m_context->OMSetRenderTargets(1, &nullRTV, nullptr);
}

#ifndef FF_LEGACY_BINDING_DISABLED
void CDX11CommandList::UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t numSlots) {
    ID3D11ShaderResourceView* nullSRVs[16] = { nullptr };
    uint32_t count = (numSlots > 16) ? 16 : numSlots;

    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetShaderResources(startSlot, count, nullSRVs);
            break;
        case EShaderStage::Pixel:
            m_context->PSSetShaderResources(startSlot, count, nullSRVs);
            break;
        case EShaderStage::Compute:
            m_context->CSSetShaderResources(startSlot, count, nullSRVs);
            break;
        case EShaderStage::Geometry:
            m_context->GSSetShaderResources(startSlot, count, nullSRVs);
            break;
        case EShaderStage::Hull:
            m_context->HSSetShaderResources(startSlot, count, nullSRVs);
            break;
        case EShaderStage::Domain:
            m_context->DSSetShaderResources(startSlot, count, nullSRVs);
            break;
    }
}
#endif // FF_LEGACY_BINDING_DISABLED

void CDX11CommandList::GenerateMips(ITexture* texture) {
    if (!texture) return;

    CDX11Texture* dx11Texture = static_cast<CDX11Texture*>(texture);
    ID3D11ShaderResourceView* srv = dx11Texture->GetOrCreateSRV();
    if (srv) {
        m_context->GenerateMips(srv);
    }
}

void CDX11CommandList::BeginEvent(const wchar_t* name) {
    m_currentEventName = name;  // Track current event for debug
    if (m_annotation) {
        m_annotation->BeginEvent(name);
    }
}

void CDX11CommandList::EndEvent() {
    m_currentEventName = nullptr;  // Clear event name
    if (m_annotation) {
        m_annotation->EndEvent();
    }
}

} // namespace DX11
} // namespace RHI
