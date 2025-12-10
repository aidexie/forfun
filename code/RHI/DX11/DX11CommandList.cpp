#include "DX11CommandList.h"
#include "DX11Utils.h"

namespace RHI {
namespace DX11 {

CDX11CommandList::CDX11CommandList(ID3D11DeviceContext* context)
    : m_context(context)
{
}

void CDX11CommandList::SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil) {
    ID3D11RenderTargetView* rtvs[8] = {nullptr};

    for (uint32_t i = 0; i < numRTs && i < 8; i++) {
        if (renderTargets[i]) {
            rtvs[i] = static_cast<ID3D11RenderTargetView*>(renderTargets[i]->GetRTV());
        }
    }

    ID3D11DepthStencilView* dsv = nullptr;
    if (depthStencil) {
        dsv = static_cast<ID3D11DepthStencilView*>(depthStencil->GetDSV());
    }

    m_context->OMSetRenderTargets(numRTs, rtvs, dsv);
}

void CDX11CommandList::ClearRenderTarget(ITexture* renderTarget, const float color[4]) {
    if (!renderTarget) return;
    ID3D11RenderTargetView* rtv = static_cast<ID3D11RenderTargetView*>(renderTarget->GetRTV());
    if (rtv) {
        m_context->ClearRenderTargetView(rtv, color);
    }
}

void CDX11CommandList::ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) {
    if (!depthStencil) return;
    ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(depthStencil->GetDSV());
    if (dsv) {
        UINT flags = 0;
        if (clearDepth) flags |= D3D11_CLEAR_DEPTH;
        if (clearStencil) flags |= D3D11_CLEAR_STENCIL;
        m_context->ClearDepthStencilView(dsv, flags, depth, stencil);
    }
}

void CDX11CommandList::SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice) {
    ID3D11DepthStencilView* dsv = nullptr;
    if (depthStencil) {
        if (depthStencil->GetArraySize() > 1) {
            dsv = static_cast<ID3D11DepthStencilView*>(depthStencil->GetDSVSlice(arraySlice));
        } else {
            dsv = static_cast<ID3D11DepthStencilView*>(depthStencil->GetDSV());
        }
    }
    m_context->OMSetRenderTargets(0, nullptr, dsv);
}

void CDX11CommandList::ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) {
    if (!depthStencil) return;
    ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(depthStencil->GetDSVSlice(arraySlice));
    if (dsv) {
        UINT flags = 0;
        if (clearDepth) flags |= D3D11_CLEAR_DEPTH;
        if (clearStencil) flags |= D3D11_CLEAR_STENCIL;
        m_context->ClearDepthStencilView(dsv, flags, depth, stencil);
    }
}

void CDX11CommandList::SetPipelineState(IPipelineState* pso) {
    if (!pso) return;

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

void CDX11CommandList::SetConstantBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) {
    ID3D11Buffer* d3dBuffer = buffer ? static_cast<CDX11Buffer*>(buffer)->GetD3D11Buffer() : nullptr;

    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetConstantBuffers(slot, 1, &d3dBuffer);
            break;
        case EShaderStage::Pixel:
            m_context->PSSetConstantBuffers(slot, 1, &d3dBuffer);
            break;
        case EShaderStage::Compute:
            m_context->CSSetConstantBuffers(slot, 1, &d3dBuffer);
            break;
        case EShaderStage::Geometry:
            m_context->GSSetConstantBuffers(slot, 1, &d3dBuffer);
            break;
        case EShaderStage::Hull:
            m_context->HSSetConstantBuffers(slot, 1, &d3dBuffer);
            break;
        case EShaderStage::Domain:
            m_context->DSSetConstantBuffers(slot, 1, &d3dBuffer);
            break;
    }
}

void CDX11CommandList::SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) {
    ID3D11ShaderResourceView* srv = texture ? static_cast<ID3D11ShaderResourceView*>(texture->GetSRV()) : nullptr;

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
    // For structured buffers, we need SRV (not implemented yet in CDX11Buffer)
    // For now, this is a placeholder
    ID3D11ShaderResourceView* srv = nullptr;

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
    }
}

void CDX11CommandList::SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) {
    ID3D11SamplerState* d3dSampler = sampler ? static_cast<CDX11Sampler*>(sampler)->GetD3D11Sampler() : nullptr;

    switch (stage) {
        case EShaderStage::Vertex:
            m_context->VSSetSamplers(slot, 1, &d3dSampler);
            break;
        case EShaderStage::Pixel:
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
    ID3D11UnorderedAccessView* uav = nullptr;  // TODO: Implement UAV in CDX11Buffer
    m_context->CSSetUnorderedAccessViews(slot, 1, &uav, nullptr);
}

void CDX11CommandList::SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) {
    ID3D11UnorderedAccessView* uav = texture ? static_cast<ID3D11UnorderedAccessView*>(texture->GetUAV()) : nullptr;
    m_context->CSSetUnorderedAccessViews(slot, 1, &uav, nullptr);
}

void CDX11CommandList::Draw(uint32_t vertexCount, uint32_t startVertex) {
    m_context->Draw(vertexCount, startVertex);
}

void CDX11CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) {
    m_context->DrawIndexed(indexCount, startIndex, baseVertex);
}

void CDX11CommandList::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                                     uint32_t startVertex, uint32_t startInstance) {
    m_context->DrawInstanced(vertexCountPerInstance, instanceCount, startVertex, startInstance);
}

void CDX11CommandList::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                                            uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) {
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
    UINT dstMipLevels = 1;
    D3D11_RESOURCE_DIMENSION dim;
    dstRes->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D*>(dstRes)->GetDesc(&desc);
        dstMipLevels = desc.MipLevels;
    }

    UINT dstSubresource = D3D11CalcSubresource(dstMipLevel, dstArraySlice, dstMipLevels);
    m_context->CopySubresourceRegion(dstRes, dstSubresource, 0, 0, 0, srcRes, 0, nullptr);
}

void CDX11CommandList::UnbindRenderTargets() {
    ID3D11RenderTargetView* nullRTV = nullptr;
    m_context->OMSetRenderTargets(1, &nullRTV, nullptr);
}

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

} // namespace DX11
} // namespace RHI
