#include "DX12CommandList.h"
#include "DX12Common.h"
#include "DX12RenderContext.h"
#include "DX12Context.h"
#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "../../Core/FFLog.h"

// PIX events - optional, requires WinPixEventRuntime
// #include <pix3.h>
// #define USE_PIX

namespace RHI {
namespace DX12 {

// ============================================
// Constructor / Destructor
// ============================================

CDX12CommandList::CDX12CommandList(CDX12RenderContext* context)
    : m_context(context)
{
}

CDX12CommandList::~CDX12CommandList() {
}

bool CDX12CommandList::Initialize() {
    auto& dx12Context = CDX12Context::Instance();
    ID3D12Device* device = dx12Context.GetDevice();

    // Create command list (initially closed)
    HRESULT hr = DX12_CHECK(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        dx12Context.GetCurrentCommandAllocator(),
        nullptr,  // Initial PSO
        IID_PPV_ARGS(&m_commandList)
    ));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12CommandList] CreateCommandList failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    // Close it initially - will be reset in BeginFrame
    m_commandList->Close();

    DX12_SET_DEBUG_NAME(m_commandList, "MainCommandList");
    return true;
}

void CDX12CommandList::Reset(ID3D12CommandAllocator* allocator) {
    allocator->Reset();
    m_commandList->Reset(allocator, nullptr);
    m_descriptorHeapsBound = false;
    m_currentPSO = nullptr;
    m_currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;  // Force re-set on next draw

    // Reset pending descriptor bindings
    memset(m_pendingSRVs, 0, sizeof(m_pendingSRVs));
    memset(m_pendingSamplers, 0, sizeof(m_pendingSamplers));
    m_srvDirty = false;
    m_samplerDirty = false;
}

void CDX12CommandList::Close() {
    FlushBarriers();
    m_commandList->Close();
}

// ============================================
// Helper Methods
// ============================================

void CDX12CommandList::TransitionResource(CDX12Texture* texture, D3D12_RESOURCE_STATES targetState) {
    if (!texture) return;
    D3D12_RESOURCE_STATES currentState = texture->GetCurrentState();
    if (NeedsTransition(currentState, targetState)) {
        m_stateTracker.TransitionResourceExplicit(texture->GetD3D12Resource(), currentState, targetState);
        texture->SetCurrentState(targetState);
    }
}

void CDX12CommandList::TransitionResource(CDX12Buffer* buffer, D3D12_RESOURCE_STATES targetState) {
    if (!buffer) return;
    D3D12_RESOURCE_STATES currentState = buffer->GetCurrentState();
    if (NeedsTransition(currentState, targetState)) {
        m_stateTracker.TransitionResourceExplicit(buffer->GetD3D12Resource(), currentState, targetState);
        buffer->SetCurrentState(targetState);
    }
}

void CDX12CommandList::FlushBarriers() {
    m_stateTracker.FlushBarriers(m_commandList.Get());
}

void CDX12CommandList::EnsureDescriptorHeapsBound() {
    if (m_descriptorHeapsBound) return;

    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    ID3D12DescriptorHeap* heaps[] = {
        heapMgr.GetCBVSRVUAVHeap().GetHeap(),
        heapMgr.GetSamplerHeap().GetHeap()
    };
    m_commandList->SetDescriptorHeaps(2, heaps);
    m_descriptorHeapsBound = true;
}

// ============================================
// Render Target Operations
// ============================================

void CDX12CommandList::SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE* pDSV = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};

    for (uint32_t i = 0; i < numRTs; ++i) {
        if (renderTargets[i]) {
            CDX12Texture* tex = static_cast<CDX12Texture*>(renderTargets[i]);
            TransitionResource(tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
            rtvHandles[i] = tex->GetOrCreateRTV();
        }
    }

    if (depthStencil) {
        CDX12Texture* dsTex = static_cast<CDX12Texture*>(depthStencil);
        TransitionResource(dsTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsvHandle = dsTex->GetOrCreateDSV();
        pDSV = &dsvHandle;
    }

    FlushBarriers();
    m_commandList->OMSetRenderTargets(numRTs, rtvHandles, FALSE, pDSV);
}

void CDX12CommandList::SetRenderTargetSlice(ITexture* renderTarget, uint32_t arraySlice, ITexture* depthStencil) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE* pDSV = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};

    if (renderTarget) {
        CDX12Texture* tex = static_cast<CDX12Texture*>(renderTarget);
        TransitionResource(tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvHandle = tex->GetOrCreateRTVSlice(arraySlice, 0);
    }

    if (depthStencil) {
        CDX12Texture* dsTex = static_cast<CDX12Texture*>(depthStencil);
        TransitionResource(dsTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsvHandle = dsTex->GetOrCreateDSV();
        pDSV = &dsvHandle;
    }

    FlushBarriers();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, pDSV);
}

void CDX12CommandList::SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice) {
    if (!depthStencil) return;

    CDX12Texture* dsTex = static_cast<CDX12Texture*>(depthStencil);
    TransitionResource(dsTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    FlushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsTex->GetOrCreateDSVSlice(arraySlice);
    m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
}

void CDX12CommandList::ClearRenderTarget(ITexture* renderTarget, const float color[4]) {
    if (!renderTarget) return;

    CDX12Texture* tex = static_cast<CDX12Texture*>(renderTarget);
    TransitionResource(tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    FlushBarriers();

    m_commandList->ClearRenderTargetView(tex->GetOrCreateRTV(), color, 0, nullptr);
}

void CDX12CommandList::ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) {
    if (!depthStencil) return;

    CDX12Texture* dsTex = static_cast<CDX12Texture*>(depthStencil);
    TransitionResource(dsTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    FlushBarriers();

    D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
    if (clearDepth) flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (clearStencil) flags |= D3D12_CLEAR_FLAG_STENCIL;

    m_commandList->ClearDepthStencilView(dsTex->GetOrCreateDSV(), flags, depth, stencil, 0, nullptr);
}

void CDX12CommandList::ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) {
    if (!depthStencil) return;

    CDX12Texture* dsTex = static_cast<CDX12Texture*>(depthStencil);
    TransitionResource(dsTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    FlushBarriers();

    D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
    if (clearDepth) flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (clearStencil) flags |= D3D12_CLEAR_FLAG_STENCIL;

    m_commandList->ClearDepthStencilView(dsTex->GetOrCreateDSVSlice(arraySlice), flags, depth, stencil, 0, nullptr);
}

// ============================================
// Pipeline State
// ============================================

void CDX12CommandList::SetPipelineState(IPipelineState* pso) {
    if (!pso) return;

    CDX12PipelineState* dx12PSO = static_cast<CDX12PipelineState*>(pso);
    if (m_currentPSO == dx12PSO) return;

    m_currentPSO = dx12PSO;
    m_commandList->SetPipelineState(dx12PSO->GetPSO());

    // Set root signature
    if (dx12PSO->IsCompute()) {
        m_commandList->SetComputeRootSignature(dx12PSO->GetRootSignature());
    } else {
        m_commandList->SetGraphicsRootSignature(dx12PSO->GetRootSignature());
    }

    EnsureDescriptorHeapsBound();
}

void CDX12CommandList::SetPrimitiveTopology(EPrimitiveTopology topology) {
    D3D12_PRIMITIVE_TOPOLOGY d3dTopology;
    switch (topology) {
        case EPrimitiveTopology::PointList:     d3dTopology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case EPrimitiveTopology::LineList:      d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case EPrimitiveTopology::LineStrip:     d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case EPrimitiveTopology::TriangleList:  d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case EPrimitiveTopology::TriangleStrip: d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        default:                                d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    }

    if (m_currentTopology != d3dTopology) {
        m_currentTopology = d3dTopology;
        m_commandList->IASetPrimitiveTopology(d3dTopology);
    }
}

void CDX12CommandList::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth) {
    D3D12_VIEWPORT viewport;
    viewport.TopLeftX = x;
    viewport.TopLeftY = y;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = minDepth;
    viewport.MaxDepth = maxDepth;
    m_commandList->RSSetViewports(1, &viewport);
}

void CDX12CommandList::SetScissorRect(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    D3D12_RECT rect;
    rect.left = static_cast<LONG>(left);
    rect.top = static_cast<LONG>(top);
    rect.right = static_cast<LONG>(right);
    rect.bottom = static_cast<LONG>(bottom);
    m_commandList->RSSetScissorRects(1, &rect);
}

// ============================================
// Resource Binding
// ============================================

void CDX12CommandList::SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset) {
    if (!buffer) {
        m_commandList->IASetVertexBuffers(slot, 0, nullptr);
        return;
    }

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = dx12Buffer->GetGPUVirtualAddress() + offset;
    vbv.SizeInBytes = dx12Buffer->GetDesc().size - offset;
    vbv.StrideInBytes = stride;

    m_commandList->IASetVertexBuffers(slot, 1, &vbv);
}

void CDX12CommandList::SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset) {
    if (!buffer) {
        m_commandList->IASetIndexBuffer(nullptr);
        return;
    }

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);

    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = dx12Buffer->GetGPUVirtualAddress() + offset;
    ibv.SizeInBytes = dx12Buffer->GetDesc().size - offset;
    ibv.Format = (format == EIndexFormat::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    m_commandList->IASetIndexBuffer(&ibv);
}

void CDX12CommandList::SetConstantBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) {
    if (!buffer) return;

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    EnsureDescriptorHeapsBound();

    // Root parameter 0-6 are root CBVs (b0-b6)
    // Using root CBV directly (no descriptor)
    if (slot < 7) {
        D3D12_GPU_VIRTUAL_ADDRESS address = dx12Buffer->GetGPUVirtualAddress();
        if (stage == EShaderStage::Compute) {
            m_commandList->SetComputeRootConstantBufferView(slot, address);
        } else {
            m_commandList->SetGraphicsRootConstantBufferView(slot, address);
        }
    }
    // For slots >= 7, would need descriptor table (not implemented in fixed layout)
}

void CDX12CommandList::SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) {
    if (!texture || slot >= MAX_SRV_SLOTS) return;

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    TransitionResource(dx12Texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // Get the SRV - this is already in the shader-visible heap
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = dx12Texture->GetOrCreateSRV();

    // Get GPU handle from the shader-visible heap
    // Since SRVs are allocated from the shader-visible CBV_SRV_UAV heap,
    // we need to calculate the GPU handle from the CPU handle
    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    auto& heap = heapMgr.GetCBVSRVUAVHeap();

    // Calculate index from CPU handle
    SIZE_T cpuHandlePtr = cpuHandle.ptr;
    SIZE_T heapStartPtr = heap.GetCPUStart().ptr;
    uint32_t descriptorSize = heap.GetDescriptorSize();
    uint32_t index = static_cast<uint32_t>((cpuHandlePtr - heapStartPtr) / descriptorSize);

    // Calculate GPU handle
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = heap.GetGPUStart();
    gpuHandle.ptr += index * descriptorSize;

    m_pendingSRVs[slot] = gpuHandle;
    m_srvDirty = true;
}

void CDX12CommandList::SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) {
    if (!buffer) return;

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    TransitionResource(dx12Buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FlushBarriers();

    EnsureDescriptorHeapsBound();
    // Similar to SetShaderResource - needs proper descriptor table binding
}

void CDX12CommandList::SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) {
    if (!sampler || slot >= MAX_SAMPLER_SLOTS) return;

    EnsureDescriptorHeapsBound();

    CDX12Sampler* dx12Sampler = static_cast<CDX12Sampler*>(sampler);

    // Samplers are already allocated in the shader-visible sampler heap
    m_pendingSamplers[slot] = dx12Sampler->GetGPUHandle();
    m_samplerDirty = true;
}

void CDX12CommandList::SetUnorderedAccess(uint32_t slot, IBuffer* buffer) {
    if (!buffer) return;

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    TransitionResource(dx12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers();

    EnsureDescriptorHeapsBound();
    // UAV binding via descriptor table
}

void CDX12CommandList::SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) {
    if (!texture) return;

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    TransitionResource(dx12Texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers();

    EnsureDescriptorHeapsBound();
}

void CDX12CommandList::ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) {
    if (!buffer) return;

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    TransitionResource(dx12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // ClearUnorderedAccessViewUint requires both CPU and GPU descriptor handles
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = dx12Buffer->GetUAV();
    // Would need to copy to shader-visible heap and get GPU handle
    // This is a simplified implementation
    // m_commandList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, dx12Buffer->GetD3D12Resource(), values, 0, nullptr);
}

// ============================================
// Draw Commands
// ============================================

void CDX12CommandList::Draw(uint32_t vertexCount, uint32_t startVertex) {
    BindPendingDescriptorTables();
    FlushBarriers();
    m_commandList->DrawInstanced(vertexCount, 1, startVertex, 0);
}

void CDX12CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) {
    BindPendingDescriptorTables();
    FlushBarriers();
    m_commandList->DrawIndexedInstanced(indexCount, 1, startIndex, baseVertex, 0);
}

void CDX12CommandList::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) {
    BindPendingDescriptorTables();
    FlushBarriers();
    m_commandList->DrawInstanced(vertexCountPerInstance, instanceCount, startVertex, startInstance);
}

void CDX12CommandList::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) {
    BindPendingDescriptorTables();
    FlushBarriers();
    m_commandList->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
}

// ============================================
// Compute Commands
// ============================================

void CDX12CommandList::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) {
    FlushBarriers();
    m_commandList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

// ============================================
// Resource Barriers
// ============================================

void CDX12CommandList::Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter) {
    if (!resource) return;

    // Determine if it's a texture or buffer and transition
    // This is a simplified implementation - actual would need type checking
    D3D12_RESOURCE_STATES before = ToD3D12ResourceState(stateBefore);
    D3D12_RESOURCE_STATES after = ToD3D12ResourceState(stateAfter);

    ID3D12Resource* d3dResource = static_cast<ID3D12Resource*>(resource->GetNativeHandle());
    m_stateTracker.TransitionResource(d3dResource, after);
}

void CDX12CommandList::UAVBarrier(IResource* resource) {
    ID3D12Resource* d3dResource = resource ? static_cast<ID3D12Resource*>(resource->GetNativeHandle()) : nullptr;
    m_stateTracker.UAVBarrier(d3dResource);
}

// ============================================
// Copy Operations
// ============================================

void CDX12CommandList::CopyTexture(ITexture* dst, ITexture* src) {
    if (!dst || !src) return;

    CDX12Texture* dstTex = static_cast<CDX12Texture*>(dst);
    CDX12Texture* srcTex = static_cast<CDX12Texture*>(src);

    TransitionResource(dstTex, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionResource(srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FlushBarriers();

    m_commandList->CopyResource(dstTex->GetD3D12Resource(), srcTex->GetD3D12Resource());
}

void CDX12CommandList::CopyTextureToSlice(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src) {
    CopyTextureSubresource(dst, dstArraySlice, dstMipLevel, src, 0, 0);
}

void CDX12CommandList::CopyTextureSubresource(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src, uint32_t srcArraySlice, uint32_t srcMipLevel) {
    if (!dst || !src) return;

    CDX12Texture* dstTex = static_cast<CDX12Texture*>(dst);
    CDX12Texture* srcTex = static_cast<CDX12Texture*>(src);

    TransitionResource(dstTex, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionResource(srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FlushBarriers();

    const TextureDesc& dstDesc = dstTex->GetDesc();
    const TextureDesc& srcDesc = srcTex->GetDesc();

    // Check if resources are buffers (staging) or textures
    D3D12_RESOURCE_DESC dstResDesc = dstTex->GetD3D12Resource()->GetDesc();
    D3D12_RESOURCE_DESC srcResDesc = srcTex->GetD3D12Resource()->GetDesc();

    bool dstIsBuffer = (dstResDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
    bool srcIsBuffer = (srcResDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = dstTex->GetD3D12Resource();

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = srcTex->GetD3D12Resource();

    if (dstIsBuffer) {
        // Destination is a buffer - use placed footprint
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 totalSize = 0;
        m_context->GetDevice()->GetCopyableFootprints(&srcResDesc, srcMipLevel, 1, 0, &footprint, nullptr, nullptr, &totalSize);
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint = footprint;
    } else {
        // Destination is a texture - use subresource index
        UINT dstSubresource = CalcSubresource(dstMipLevel, dstArraySlice, 0, dstDesc.mipLevels, dstDesc.arraySize);
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = dstSubresource;
    }

    if (srcIsBuffer) {
        // Source is a buffer - use placed footprint
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 totalSize = 0;
        m_context->GetDevice()->GetCopyableFootprints(&dstResDesc, dstMipLevel, 1, 0, &footprint, nullptr, nullptr, &totalSize);
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;
    } else {
        // Source is a texture - use subresource index
        UINT srcSubresource = CalcSubresource(srcMipLevel, srcArraySlice, 0, srcDesc.mipLevels, srcDesc.arraySize);
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = srcSubresource;
    }

    m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
}

// ============================================
// Mipmap Generation
// ============================================

void CDX12CommandList::GenerateMips(ITexture* texture) {
    // DX12 doesn't have built-in GenerateMips like DX11
    // Would need compute shader implementation
    CFFLog::Warning("[DX12CommandList] GenerateMips not implemented - requires compute shader");
}

// ============================================
// Unbind Operations
// ============================================

void CDX12CommandList::UnbindRenderTargets() {
    m_commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
}

void CDX12CommandList::UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t numSlots) {
    // DX12 doesn't have explicit unbind - resources are managed via descriptors
    // State transitions handle hazards
}

// ============================================
// Debug Events
// ============================================

void CDX12CommandList::BeginEvent(const wchar_t* name) {
#ifdef USE_PIX
    PIXBeginEvent(m_commandList.Get(), 0, name);
#else
    (void)name;
#endif
}

void CDX12CommandList::EndEvent() {
#ifdef USE_PIX
    PIXEndEvent(m_commandList.Get());
#endif
}

} // namespace DX12
} // namespace RHI
