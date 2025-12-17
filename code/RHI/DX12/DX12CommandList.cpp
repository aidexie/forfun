#include "DX12CommandList.h"
#include "DX12Common.h"
#include "DX12RenderContext.h"
#include "DX12Context.h"
#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "../ShaderCompiler.h"
#include "../RHIManager.h"
#include "../../Core/FFLog.h"
#include "../../Core/PathManager.h"
#include "../../Core/RenderDocCapture.h"

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
    m_isComputePSO = false;

    // Reset pending bindings
    memset(m_pendingCBVs, 0, sizeof(m_pendingCBVs));
    memset(m_pendingSRVCpuHandles, 0, sizeof(m_pendingSRVCpuHandles));
    memset(m_pendingSamplerCpuHandles, 0, sizeof(m_pendingSamplerCpuHandles));
    memset(m_pendingUAVCpuHandles, 0, sizeof(m_pendingUAVCpuHandles));
    m_cbvDirty = false;
    m_srvDirty = false;
    m_samplerDirty = false;
    m_uavDirty = false;
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
#if 0  // Enable for state tracking debug
        const char* debugName = texture->GetDesc().debugName ? texture->GetDesc().debugName : "unnamed";
        CFFLog::Info("[StateTrack] Texture '%s': 0x%X -> 0x%X", debugName, currentState, targetState);
#endif
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
        heapMgr.GetSRVStagingRing().GetHeap(),     // SRV staging ring's shader-visible heap
        heapMgr.GetSamplerStagingRing().GetHeap()  // Sampler staging ring's shader-visible heap
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
    m_isComputePSO = dx12PSO->IsCompute();
    m_commandList->SetPipelineState(dx12PSO->GetPSO());

    // Set root signature
    if (m_isComputePSO) {
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

bool CDX12CommandList::SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) {
    if (!data || size == 0 || slot >= MAX_CBV_SLOTS) return false;

    if (!m_dynamicBuffer) {
        CFFLog::Error("[DX12CommandList] SetConstantBufferData called but dynamic buffer ring not set!");
        return false;
    }

    // Allocate from ring buffer
    SDynamicAllocation alloc = m_dynamicBuffer->Allocate(size, CB_ALIGNMENT);
    if (!alloc.IsValid()) {
        CFFLog::Error("[DX12CommandList] Failed to allocate %zu bytes from dynamic buffer", size);
        return false;
    }

    // Copy data to the allocated region
    memcpy(alloc.cpuAddress, data, size);

    // Bind the GPU address
    m_pendingCBVs[slot] = alloc.gpuAddress;
    m_cbvDirty = true;

    return true;
}

void CDX12CommandList::SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) {
    if (!texture || slot >= MAX_SRV_SLOTS) return;

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    TransitionResource(dx12Texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // Get SRV handle from CPU-only heap (this is the copy source)
    SDescriptorHandle srvHandle = dx12Texture->GetOrCreateSRV();

    // Store CPU handle for later copy to staging region
    m_pendingSRVCpuHandles[slot] = srvHandle.cpuHandle;
    m_srvDirty = true;
}

void CDX12CommandList::SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) {
    if (slot >= MAX_SRV_SLOTS) return;

    if (!buffer) {
        // Unbind - clear the slot
        m_pendingSRVCpuHandles[slot] = {};
        m_srvDirty = true;
        return;
    }

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    TransitionResource(dx12Buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // Get SRV handle from CPU-only heap (this is the copy source)
    SDescriptorHandle srvHandle = dx12Buffer->GetSRV();

    // Store CPU handle for later copy to staging region
    m_pendingSRVCpuHandles[slot] = srvHandle.cpuHandle;
    m_srvDirty = true;
}

void CDX12CommandList::SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) {
    if (!sampler || slot >= MAX_SAMPLER_SLOTS) return;

    EnsureDescriptorHeapsBound();

    CDX12Sampler* dx12Sampler = static_cast<CDX12Sampler*>(sampler);

    // Store CPU handle for deferred binding (will be copied to staging region in BindPendingResources)
    m_pendingSamplerCpuHandles[slot] = dx12Sampler->GetCPUHandle();
    m_samplerDirty = true;
}

void CDX12CommandList::SetUnorderedAccess(uint32_t slot, IBuffer* buffer) {
    if (slot >= MAX_UAV_SLOTS) return;

    if (!buffer) {
        // Unbind - clear the slot
        m_pendingUAVCpuHandles[slot] = {};
        m_uavDirty = true;
        return;
    }

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    TransitionResource(dx12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // Get UAV handle from CPU-only heap
    SDescriptorHandle uavHandle = dx12Buffer->GetUAV();

    // Store CPU handle for later copy to staging region
    m_pendingUAVCpuHandles[slot] = uavHandle.cpuHandle;
    m_uavDirty = true;
}

void CDX12CommandList::SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) {
    if (slot >= MAX_UAV_SLOTS) return;

    if (!texture) {
        // Unbind - clear the slot
        m_pendingUAVCpuHandles[slot] = {};
        m_uavDirty = true;
        return;
    }

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    TransitionResource(dx12Texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // Get UAV handle from texture
    SDescriptorHandle uavHandle = dx12Texture->GetOrCreateUAV();

    // Store CPU handle for later copy to staging region
    m_pendingUAVCpuHandles[slot] = uavHandle.cpuHandle;
    m_uavDirty = true;
}

void CDX12CommandList::ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) {
    if (!buffer) return;

    CDX12Buffer* dx12Buffer = static_cast<CDX12Buffer*>(buffer);
    TransitionResource(dx12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushBarriers();

    EnsureDescriptorHeapsBound();

    // ClearUnorderedAccessViewUint requires:
    // 1. GPU descriptor handle in shader-visible heap (for GPU access)
    // 2. CPU descriptor handle in non-shader-visible heap (for the actual UAV description)
    // We need to copy the UAV to the staging ring to get a GPU handle

    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    auto device = CDX12Context::Instance().GetDevice();

    // Get CPU handle from buffer's UAV
    SDescriptorHandle uavHandle = dx12Buffer->GetUAV();

    // Allocate one descriptor from staging ring for GPU-visible handle
    auto& stagingRing = heapMgr.GetSRVStagingRing();
    SDescriptorHandle gpuHandle = stagingRing.AllocateContiguous(1);

    if (!gpuHandle.IsValid()) {
        CFFLog::Error("[DX12CommandList] ClearUnorderedAccessViewUint: Failed to allocate staging descriptor");
        return;
    }

    // Copy the UAV to staging ring
    device->CopyDescriptorsSimple(1, gpuHandle.cpuHandle, uavHandle.cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Now call ClearUnorderedAccessViewUint with both handles
    m_commandList->ClearUnorderedAccessViewUint(
        gpuHandle.gpuHandle,      // GPU handle in shader-visible heap
        uavHandle.cpuHandle,      // CPU handle with UAV description
        dx12Buffer->GetD3D12Resource(),
        values,
        0, nullptr);
}

// ============================================
// Draw Commands
// ============================================

void CDX12CommandList::Draw(uint32_t vertexCount, uint32_t startVertex) {
    BindPendingResources();
    FlushBarriers();
    m_commandList->DrawInstanced(vertexCount, 1, startVertex, 0);
}

void CDX12CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) {
    BindPendingResources();
    FlushBarriers();
    m_commandList->DrawIndexedInstanced(indexCount, 1, startIndex, baseVertex, 0);
}

void CDX12CommandList::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) {
    BindPendingResources();
    FlushBarriers();
    m_commandList->DrawInstanced(vertexCountPerInstance, instanceCount, startVertex, startInstance);
}

void CDX12CommandList::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) {
    BindPendingResources();
    FlushBarriers();
    m_commandList->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
}

// ============================================
// Compute Commands
// ============================================

void CDX12CommandList::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) {
    BindPendingResourcesCompute();
    FlushBarriers();
    m_commandList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

// ============================================
// Resource Barriers
// ============================================

void CDX12CommandList::Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter) {
    if (!resource) return;

    D3D12_RESOURCE_STATES after = ToD3D12ResourceState(stateAfter);

    // Try to cast to texture first, then buffer, to update tracked state
    // This ensures the resource's internal state tracking stays in sync
    if (auto* texture = dynamic_cast<ITexture*>(resource)) {
        CDX12Texture* dx12Tex = static_cast<CDX12Texture*>(texture);
        TransitionResource(dx12Tex, after);
    } else if (auto* buffer = dynamic_cast<IBuffer*>(resource)) {
        CDX12Buffer* dx12Buf = static_cast<CDX12Buffer*>(buffer);
        TransitionResource(dx12Buf, after);
    } else {
        // Fallback: use state tracker directly (won't update resource's tracked state)
        ID3D12Resource* d3dResource = static_cast<ID3D12Resource*>(resource->GetNativeHandle());
        m_stateTracker.TransitionResource(d3dResource, after);
    }
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

// CB structure matching GenerateMips.cs.hlsl
struct CB_GenerateMips {
    uint32_t srcMipSizeX;
    uint32_t srcMipSizeY;
    uint32_t dstMipSizeX;
    uint32_t dstMipSizeY;
    uint32_t srcMipLevel;
    uint32_t arraySlice;
    uint32_t isSRGB;
    uint32_t padding;
};

bool CDX12CommandList::EnsureGenerateMipsResources() {
    if (m_generateMipsPSO && m_generateMips2DPSO) {
        return true;  // Already initialized
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[DX12CommandList] EnsureGenerateMipsResources: No render context");
        return false;
    }

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile the GenerateMips compute shader for array/cubemap textures
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/GenerateMips.cs.hlsl";
    SCompiledShader compiled = CompileShaderFromFile(shaderPath, "main", "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[DX12CommandList] GenerateMips shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    // Create shader for array/cubemap
    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    m_generateMipsCS.reset(ctx->CreateShader(shaderDesc));
    if (!m_generateMipsCS) {
        CFFLog::Error("[DX12CommandList] Failed to create GenerateMips shader");
        return false;
    }

    // Create compute PSO for array/cubemap
    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = m_generateMipsCS.get();
    m_generateMipsPSO.reset(ctx->CreateComputePipelineState(psoDesc));
    if (!m_generateMipsPSO) {
        CFFLog::Error("[DX12CommandList] Failed to create GenerateMips PSO");
        return false;
    }

    // Compile the GenerateMips compute shader for 2D textures
    std::string shader2DPath = FFPath::GetSourceDir() + "/Shader/GenerateMips2D.cs.hlsl";
    SCompiledShader compiled2D = CompileShaderFromFile(shader2DPath, "main", "cs_5_0", nullptr, debugShaders);
    if (!compiled2D.success) {
        CFFLog::Error("[DX12CommandList] GenerateMips2D shader compilation failed: %s", compiled2D.errorMessage.c_str());
        return false;
    }

    // Create shader for 2D textures
    ShaderDesc shader2DDesc;
    shader2DDesc.type = EShaderType::Compute;
    shader2DDesc.bytecode = compiled2D.bytecode.data();
    shader2DDesc.bytecodeSize = compiled2D.bytecode.size();
    m_generateMips2DCS.reset(ctx->CreateShader(shader2DDesc));
    if (!m_generateMips2DCS) {
        CFFLog::Error("[DX12CommandList] Failed to create GenerateMips2D shader");
        return false;
    }

    // Create compute PSO for 2D textures
    ComputePipelineDesc pso2DDesc;
    pso2DDesc.computeShader = m_generateMips2DCS.get();
    m_generateMips2DPSO.reset(ctx->CreateComputePipelineState(pso2DDesc));
    if (!m_generateMips2DPSO) {
        CFFLog::Error("[DX12CommandList] Failed to create GenerateMips2D PSO");
        return false;
    }

    // Create linear sampler for bilinear filtering
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Clamp;
    samplerDesc.addressV = ETextureAddressMode::Clamp;
    samplerDesc.addressW = ETextureAddressMode::Clamp;
    m_generateMipsSampler.reset(ctx->CreateSampler(samplerDesc));
    if (!m_generateMipsSampler) {
        CFFLog::Error("[DX12CommandList] Failed to create GenerateMips sampler");
        return false;
    }

    CFFLog::Info("[DX12CommandList] GenerateMips resources initialized");
    return true;
}

void CDX12CommandList::GenerateMips(ITexture* texture) {
    if (!texture) {
        CFFLog::Warning("[DX12CommandList] GenerateMips: null texture");
        return;
    }

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    TextureDesc desc = dx12Texture->GetDesc();  // Copy, not const ref, so we can modify

    // Handle mipLevels == 0 (auto-calculate)
    // This shouldn't happen if texture was created correctly, but handle defensively
    if (desc.mipLevels == 0) {
        // Calculate max mip levels: floor(log2(max(width, height))) + 1
        uint32_t maxDim = std::max(desc.width, desc.height);
        desc.mipLevels = 1;
        while (maxDim > 1) {
            maxDim >>= 1;
            desc.mipLevels++;
        }
        CFFLog::Warning("[DX12CommandList] GenerateMips: mipLevels was 0, calculated %u", desc.mipLevels);
    }

    // Validate texture can have mips generated
    if (desc.mipLevels <= 1) {
        return;  // Nothing to generate
    }

    // Check if UAV flag is set (required for compute shader write)
    if (!(desc.usage & ETextureUsage::UnorderedAccess)) {
        CFFLog::Warning("[DX12CommandList] GenerateMips: texture lacks UnorderedAccess flag. Use ETextureMiscFlags::GenerateMips when creating.");
        return;
    }

    // Ensure compute resources are ready
    if (!EnsureGenerateMipsResources()) {
        CFFLog::Error("[DX12CommandList] GenerateMips: failed to initialize resources");
        return;
    }

    // RenderDoc capture for debugging GenerateMips
    // Captures the entire mipmap generation process
    static bool s_captureOnce = true;
    bool doCapture = s_captureOnce && CRenderDocCapture::IsAvailable();
    if (doCapture) {
        s_captureOnce = false;  // Only capture once
        //CRenderDocCapture::BeginFrameCapture();
        CFFLog::Info("[DX12CommandList] GenerateMips: RenderDoc capture started");
    }

    // Determine if this is a 2D texture or array/cubemap
    bool is2D = (desc.dimension == ETextureDimension::Tex2D) && (desc.arraySize == 1);

    // Get array size for iteration
    uint32_t arraySize = 1;
    if (!is2D) {
        if (desc.dimension == ETextureDimension::TexCube) {
            arraySize = 6;
        } else if (desc.dimension == ETextureDimension::TexCubeArray) {
            arraySize = desc.arraySize;  // Already face count
        } else {
            arraySize = desc.arraySize;
        }
    }

    // Determine if source is SRGB (for correct gamma handling)
    bool isSRGB = (desc.srvFormat == ETextureFormat::R8G8B8A8_UNORM_SRGB ||
                   desc.srvFormat == ETextureFormat::B8G8R8A8_UNORM_SRGB ||
                   desc.format == ETextureFormat::R8G8B8A8_UNORM_SRGB ||
                   desc.format == ETextureFormat::B8G8R8A8_UNORM_SRGB);

    // Select PSO based on texture dimension
    IPipelineState* pso = is2D ? m_generateMips2DPSO.get() : m_generateMipsPSO.get();
    SetPipelineState(pso);
    SetSampler(EShaderStage::Compute, 0, m_generateMipsSampler.get());

    // Get the D3D12 resource for per-subresource barriers
    ID3D12Resource* d3dResource = dx12Texture->GetD3D12Resource();

    // Create a UNORM SRV for GenerateMips (no automatic sRGB->linear conversion)
    // This allows explicit gamma handling in the shader
    SDescriptorHandle unormSrvHandle = {};
    {
        unormSrvHandle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
        if (!unormSrvHandle.IsValid()) {
            CFFLog::Error("[DX12CommandList] GenerateMips: Failed to allocate UNORM SRV descriptor");
            return;
        }

        // Determine UNORM format (strip SRGB suffix for explicit gamma handling)
        DXGI_FORMAT srvFormat = (desc.srvFormat != ETextureFormat::Unknown) ?
                                 ToDXGIFormat(desc.srvFormat) : ToDXGIFormat(desc.format);
        DXGI_FORMAT unormFormat = srvFormat;
        if (isSRGB) {
            switch (srvFormat) {
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: unormFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: unormFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
                default: break;
            }
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = unormFormat;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (is2D) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = desc.mipLevels;
            srvDesc.Texture2D.PlaneSlice = 0;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        } else {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = desc.mipLevels;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = arraySize;
            srvDesc.Texture2DArray.PlaneSlice = 0;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        }

        CDX12Context::Instance().GetDevice()->CreateShaderResourceView(
            d3dResource, &srvDesc, unormSrvHandle.cpuHandle);
    }

    // Get the current state of the texture (set by upload code)
    // After upload, texture is typically in PIXEL_SHADER_RESOURCE state
    D3D12_RESOURCE_STATES currentState = dx12Texture->GetCurrentState();

    // Generate each mip level
    // Strategy: For each mip generation:
    //   - Source mip (N-1): must be in NON_PIXEL_SHADER_RESOURCE for compute shader read
    //   - Dest mip (N): must be in UNORDERED_ACCESS for compute shader write
    // We use per-subresource barriers to achieve this
    for (uint32_t mip = 1; mip < desc.mipLevels; ++mip) {
        uint32_t srcWidth = std::max(1u, desc.width >> (mip - 1));
        uint32_t srcHeight = std::max(1u, desc.height >> (mip - 1));
        uint32_t dstWidth = std::max(1u, desc.width >> mip);
        uint32_t dstHeight = std::max(1u, desc.height >> mip);

        // Transition source mip to SRV state, dest mip to UAV state
        std::vector<D3D12_RESOURCE_BARRIER> barriers;

        for (uint32_t slice = 0; slice < arraySize; ++slice) {
            uint32_t srcSubresource = CalcSubresource(mip - 1, slice, 0, desc.mipLevels, arraySize);
            uint32_t dstSubresource = CalcSubresource(mip, slice, 0, desc.mipLevels, arraySize);

            // Source mip: transition to SRV (NON_PIXEL_SHADER_RESOURCE for compute)
            // For mip 0: comes from texture's current state (typically PIXEL_SHADER_RESOURCE after upload)
            // For mip > 0: comes from UAV state (was written in previous iteration)
            D3D12_RESOURCE_STATES srcStateBefore = (mip == 1) ? currentState : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            D3D12_RESOURCE_BARRIER srcBarrier = {};
            srcBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            srcBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            srcBarrier.Transition.pResource = d3dResource;
            srcBarrier.Transition.Subresource = srcSubresource;
            srcBarrier.Transition.StateBefore = srcStateBefore;
            srcBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            // Only add barrier if state actually changes
            if (srcBarrier.Transition.StateBefore != srcBarrier.Transition.StateAfter) {
                barriers.push_back(srcBarrier);
            }

            // Dest mip: transition to UAV
            // For first iteration: comes from texture's current state
            // For subsequent iterations: still in current state (not yet written)
            D3D12_RESOURCE_BARRIER dstBarrier = {};
            dstBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            dstBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            dstBarrier.Transition.pResource = d3dResource;
            dstBarrier.Transition.Subresource = dstSubresource;
            dstBarrier.Transition.StateBefore = currentState;  // All mips start in same state after upload
            dstBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers.push_back(dstBarrier);
        }

        if (!barriers.empty()) {
            m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        // For each array slice / cubemap face
        for (uint32_t slice = 0; slice < arraySize; ++slice) {
            // Setup constant buffer
            CB_GenerateMips cb;
            cb.srcMipSizeX = srcWidth;
            cb.srcMipSizeY = srcHeight;
            cb.dstMipSizeX = dstWidth;
            cb.dstMipSizeY = dstHeight;
            cb.srcMipLevel = mip - 1;
            cb.arraySlice = slice;
            cb.isSRGB = isSRGB ? 1 : 0;
            cb.padding = 0;

            SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

            // Bind source SRV (UNORM format - no auto sRGB conversion, shader handles gamma)
            m_pendingSRVCpuHandles[0] = unormSrvHandle.cpuHandle;
            m_srvDirty = true;

            // Bind destination UAV (current mip level)
            SDescriptorHandle uavHandle = dx12Texture->GetOrCreateUAVSlice(mip);
            m_pendingUAVCpuHandles[0] = uavHandle.cpuHandle;
            m_uavDirty = true;

            // Dispatch compute shader
            // numthreads(8, 8, 1) in shader
            uint32_t groupsX = (dstWidth + 7) / 8;
            uint32_t groupsY = (dstHeight + 7) / 8;
            Dispatch(groupsX, groupsY, 1);
        }

        // UAV barrier between mip levels to ensure writes complete before next read
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        uavBarrier.UAV.pResource = d3dResource;
        m_commandList->ResourceBarrier(1, &uavBarrier);
    }

    // Transition all mips to shader resource state for subsequent use
    // After the loop:
    // - Mip 0 to mipLevels-2: in NON_PIXEL_SHADER_RESOURCE (used as sources)
    // - Mip mipLevels-1 (last): in UNORDERED_ACCESS (just written, never read)
    {
        std::vector<D3D12_RESOURCE_BARRIER> finalBarriers;
        D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
            for (uint32_t slice = 0; slice < arraySize; ++slice) {
                uint32_t subresource = CalcSubresource(mip, slice, 0, desc.mipLevels, arraySize);

                // Determine current state of this mip
                D3D12_RESOURCE_STATES stateBefore;
                if (mip == desc.mipLevels - 1) {
                    // Last mip: was written but never used as source
                    stateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                } else {
                    // All other mips: were used as sources (transitioned to NON_PIXEL_SHADER_RESOURCE)
                    stateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }

                // Skip if already in target state
                if (stateBefore == targetState) continue;

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = d3dResource;
                barrier.Transition.Subresource = subresource;
                barrier.Transition.StateBefore = stateBefore;
                barrier.Transition.StateAfter = targetState;
                finalBarriers.push_back(barrier);
            }
        }
        if (!finalBarriers.empty()) {
            m_commandList->ResourceBarrier(static_cast<UINT>(finalBarriers.size()), finalBarriers.data());
        }
    }

    // Update tracked state
    dx12Texture->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Clear UAV bindings
    m_pendingUAVCpuHandles[0] = {};
    m_uavDirty = true;

    // End RenderDoc capture
    if (doCapture) {
        //CRenderDocCapture::EndFrameCapture();
        //CFFLog::Info("[DX12CommandList] GenerateMips: RenderDoc capture ended");
    }
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

// ============================================
// Pending Resource Binding
// ============================================

void CDX12CommandList::BindPendingResources() {
    // Root signature layout (from DX12RenderContext::CreateRootSignatures):
    // Parameter 0-6: Root CBV b0-b6
    // Parameter 7: SRV Descriptor Table t0-t24
    // Parameter 8: UAV Descriptor Table u0-u7
    // Parameter 9: Sampler Descriptor Table s0-s7

    // Bind CBVs (root constant buffer views)
    if (m_cbvDirty) {
        for (uint32_t i = 0; i < MAX_CBV_SLOTS; ++i) {
            if (m_pendingCBVs[i] != 0) {
                m_commandList->SetGraphicsRootConstantBufferView(i, m_pendingCBVs[i]);
            }
        }
        m_cbvDirty = false;
    }

    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    auto device = CDX12Context::Instance().GetDevice();

    // Bind SRV table - copy from CPU heap to contiguous staging region
    if (m_srvDirty) {
        // Count how many SRVs are actually bound
        uint32_t maxBoundSlot = 0;
        for (uint32_t i = 0; i < MAX_SRV_SLOTS; ++i) {
            if (m_pendingSRVCpuHandles[i].ptr != 0) {
                maxBoundSlot = i + 1;
            }
        }

        if (maxBoundSlot > 0) {
            // Allocate contiguous block from staging ring
            auto& stagingRing = heapMgr.GetSRVStagingRing();
            SDescriptorHandle staging = stagingRing.AllocateContiguous(maxBoundSlot);

            if (staging.IsValid()) {
                uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                // Copy each bound SRV to the staging region
                for (uint32_t i = 0; i < maxBoundSlot; ++i) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dest = { staging.cpuHandle.ptr + i * increment };
                    if (m_pendingSRVCpuHandles[i].ptr != 0) {
                        device->CopyDescriptorsSimple(1, dest, m_pendingSRVCpuHandles[i],
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                    // Note: Unbound slots will have garbage, but shader won't read them if slot is unused
                }

                // Bind the contiguous staging region as the descriptor table
                m_commandList->SetGraphicsRootDescriptorTable(7, staging.gpuHandle);
            } else {
                CFFLog::Error("[DX12CommandList] Failed to allocate SRV staging descriptors");
            }
            //memset(m_pendingSRVCpuHandles, 0, sizeof(m_pendingSRVCpuHandles));
        }
        m_srvDirty = false;
    }

    // Bind Sampler table - copy from CPU heap to contiguous staging region
    if (m_samplerDirty) {
        // Count how many samplers are actually bound
        uint32_t maxBoundSlot = 0;
        for (uint32_t i = 0; i < MAX_SAMPLER_SLOTS; ++i) {
            if (m_pendingSamplerCpuHandles[i].ptr != 0) {
                maxBoundSlot = i + 1;
            }
        }

        if (maxBoundSlot > 0) {
            // Allocate contiguous block from sampler staging ring
            auto& samplerStagingRing = heapMgr.GetSamplerStagingRing();
            SDescriptorHandle staging = samplerStagingRing.AllocateContiguous(maxBoundSlot);

            if (staging.IsValid()) {
                uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

                // Copy each bound sampler to the staging region
                for (uint32_t i = 0; i < maxBoundSlot; ++i) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dest = { staging.cpuHandle.ptr + i * increment };
                    if (m_pendingSamplerCpuHandles[i].ptr != 0) {
                        device->CopyDescriptorsSimple(1, dest, m_pendingSamplerCpuHandles[i],
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                    }
                    // Note: Unbound slots will have garbage, but shader won't read them if slot is unused
                }

                // Bind the contiguous staging region as the sampler descriptor table
                m_commandList->SetGraphicsRootDescriptorTable(9, staging.gpuHandle);
            } else {
                CFFLog::Error("[DX12CommandList] Failed to allocate Sampler staging descriptors");
            }
        }
        m_samplerDirty = false;
    }
}

void CDX12CommandList::BindPendingResourcesCompute() {
    // Compute Root Signature layout (same as graphics):
    // Parameter 0-6: Root CBV b0-b6
    // Parameter 7: SRV Descriptor Table t0-t24
    // Parameter 8: UAV Descriptor Table u0-u7
    // Parameter 9: Sampler Descriptor Table s0-s7

    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    auto device = CDX12Context::Instance().GetDevice();

    // Bind CBVs (root constant buffer views) - use Compute version
    if (m_cbvDirty) {
        for (uint32_t i = 0; i < MAX_CBV_SLOTS; ++i) {
            if (m_pendingCBVs[i] != 0) {
                m_commandList->SetComputeRootConstantBufferView(i, m_pendingCBVs[i]);
            }
        }
        m_cbvDirty = false;
    }

    // Bind SRV table (parameter 7) - copy from CPU heap to contiguous staging region
    if (m_srvDirty) {
        uint32_t maxBoundSlot = 0;
        for (uint32_t i = 0; i < MAX_SRV_SLOTS; ++i) {
            if (m_pendingSRVCpuHandles[i].ptr != 0) {
                maxBoundSlot = i + 1;
            }
        }

        if (maxBoundSlot > 0) {
            auto& stagingRing = heapMgr.GetSRVStagingRing();
            SDescriptorHandle staging = stagingRing.AllocateContiguous(maxBoundSlot);

            if (staging.IsValid()) {
                uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                for (uint32_t i = 0; i < maxBoundSlot; ++i) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dest = { staging.cpuHandle.ptr + i * increment };
                    if (m_pendingSRVCpuHandles[i].ptr != 0) {
                        device->CopyDescriptorsSimple(1, dest, m_pendingSRVCpuHandles[i],
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                }

                m_commandList->SetComputeRootDescriptorTable(7, staging.gpuHandle);
            } else {
                CFFLog::Error("[DX12CommandList] BindPendingResourcesCompute: Failed to allocate SRV staging descriptors");
            }
        }
        m_srvDirty = false;
    }

    // Bind UAV table (parameter 8) - copy from CPU heap to contiguous staging region
    if (m_uavDirty) {
        uint32_t maxBoundSlot = 0;
        for (uint32_t i = 0; i < MAX_UAV_SLOTS; ++i) {
            if (m_pendingUAVCpuHandles[i].ptr != 0) {
                maxBoundSlot = i + 1;
            }
        }

        if (maxBoundSlot > 0) {
            auto& stagingRing = heapMgr.GetSRVStagingRing();
            SDescriptorHandle staging = stagingRing.AllocateContiguous(maxBoundSlot);

            if (staging.IsValid()) {
                uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                for (uint32_t i = 0; i < maxBoundSlot; ++i) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dest = { staging.cpuHandle.ptr + i * increment };
                    if (m_pendingUAVCpuHandles[i].ptr != 0) {
                        device->CopyDescriptorsSimple(1, dest, m_pendingUAVCpuHandles[i],
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                }

                m_commandList->SetComputeRootDescriptorTable(8, staging.gpuHandle);
            } else {
                CFFLog::Error("[DX12CommandList] BindPendingResourcesCompute: Failed to allocate UAV staging descriptors");
            }
        }
        m_uavDirty = false;
    }
}

} // namespace DX12
} // namespace RHI
