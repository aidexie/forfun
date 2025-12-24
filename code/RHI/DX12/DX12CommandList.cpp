#include "DX12CommandList.h"
#include "DX12Common.h"
#include "DX12RenderContext.h"
#include "DX12Context.h"
#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "DX12AccelerationStructure.h"
#include "../RHIManager.h"
#include "../../Core/FFLog.h"
#include "../../Core/PathManager.h"

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

    // Query for ID3D12GraphicsCommandList4 (DXR support) - cache it to avoid per-call QueryInterface
    if (SUCCEEDED(m_commandList->QueryInterface(IID_PPV_ARGS(&m_commandList4)))) {
        CFFLog::Info("[DX12CommandList] ID3D12GraphicsCommandList4 available (DXR support)");
    } else {
        CFFLog::Warning("[DX12CommandList] ID3D12GraphicsCommandList4 not available (no DXR support)");
        // m_commandList4 remains nullptr - ray tracing methods will check this
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

void CDX12CommandList::SetUnorderedAccessTextureMip(uint32_t slot, ITexture* texture, uint32_t mipLevel) {
    if (slot >= MAX_UAV_SLOTS) return;

    if (!texture) {
        m_pendingUAVCpuHandles[slot] = {};
        m_uavDirty = true;
        return;
    }

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    // Note: Resource barrier for specific mip is handled by caller (GenerateMipsPass)

    EnsureDescriptorHeapsBound();

    // Get UAV handle for specific mip level
    SDescriptorHandle uavHandle = dx12Texture->GetOrCreateUAVSlice(mipLevel);

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

void CDX12CommandList::CopyBuffer(IBuffer* dst, uint64_t dstOffset, IBuffer* src, uint64_t srcOffset, uint64_t numBytes) {
    if (!dst || !src || numBytes == 0) return;

    CDX12Buffer* dstBuf = static_cast<CDX12Buffer*>(dst);
    CDX12Buffer* srcBuf = static_cast<CDX12Buffer*>(src);

    TransitionResource(dstBuf, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionResource(srcBuf, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FlushBarriers();

    m_commandList->CopyBufferRegion(
        dstBuf->GetD3D12Resource(), dstOffset,
        srcBuf->GetD3D12Resource(), srcOffset,
        numBytes);
}

// ============================================
// Mipmap Generation
// ============================================

void CDX12CommandList::GenerateMips(ITexture* texture) {
    // Delegate to GenerateMipsPass owned by RenderContext
    m_context->GetGenerateMipsPass().Execute(this, texture);
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

void CDX12CommandList::BindPendingResourcesRayTracing() {
    // Ray Tracing Root Signature layout:
    // Parameter 0: Root CBV (b0)
    // Parameter 1: SRV Descriptor Table (t0-t4)
    // Parameter 2: UAV Descriptor Table (u0)
    // Parameter 3: Sampler Descriptor Table (s0)

    CFFLog::Info("[BindPendingResourcesRayTracing] cbvDirty=%d, srvDirty=%d, uavDirty=%d, samplerDirty=%d",
                 m_cbvDirty, m_srvDirty, m_uavDirty, m_samplerDirty);

    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    auto device = CDX12Context::Instance().GetDevice();

    // Bind CBV (parameter 0) - use first pending CBV if set
    if (m_cbvDirty) {
        if (m_pendingCBVs[0] != 0) {
            m_commandList4->SetComputeRootConstantBufferView(0, m_pendingCBVs[0]);
        }
        m_cbvDirty = false;
    }

    if (m_srvDirty) {
        uint32_t maxBoundSlot = 0;
        for (uint32_t i = 0; i < MAX_SRV_SLOTS; ++i) {
            if (m_pendingSRVCpuHandles[i].ptr != 0) {
                maxBoundSlot = i + 1;
            }
        }

        CFFLog::Info("[BindPendingResourcesRayTracing] SRV: maxBoundSlot=%u, pendingSRV[0].ptr=0x%llX",
                     maxBoundSlot, m_pendingSRVCpuHandles[0].ptr);

        if (maxBoundSlot > 0) {
            auto& stagingRing = heapMgr.GetSRVStagingRing();
            SDescriptorHandle staging = stagingRing.AllocateContiguous(maxBoundSlot);

            if (staging.IsValid()) {
                CFFLog::Info("[BindPendingResourcesRayTracing] SRV staging: gpuHandle=0x%llX", staging.gpuHandle.ptr);
                uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                for (uint32_t i = 0; i < maxBoundSlot; ++i)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE dest = { staging.cpuHandle.ptr + i * increment };
                    if (m_pendingSRVCpuHandles[i].ptr != 0) {
                        device->CopyDescriptorsSimple(1, dest, m_pendingSRVCpuHandles[i],
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    } else {
                        // Create null SRV for unbound slots
                        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
                        nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        nullSrvDesc.Texture2D.MipLevels = 1;
                        device->CreateShaderResourceView(nullptr, &nullSrvDesc, dest);
                    }
                }

                m_commandList4->SetComputeRootDescriptorTable(1, staging.gpuHandle);
            } else {
                CFFLog::Error("[DX12CommandList] BindPendingResourcesRayTracing: Failed to allocate SRV staging descriptors");
            }
        }
        m_srvDirty = false;
    }

    // Bind UAV table (parameter 2) - u0
    if (m_uavDirty) {
        CFFLog::Info("[BindPendingResourcesRayTracing] UAV: pendingUAV[0].ptr=0x%llX", m_pendingUAVCpuHandles[0].ptr);
        if (m_pendingUAVCpuHandles[0].ptr != 0) {
            auto& stagingRing = heapMgr.GetSRVStagingRing();
            SDescriptorHandle staging = stagingRing.AllocateContiguous(1);

            if (staging.IsValid()) {
                CFFLog::Info("[BindPendingResourcesRayTracing] UAV staging: gpuHandle=0x%llX", staging.gpuHandle.ptr);
                device->CopyDescriptorsSimple(1, staging.cpuHandle, m_pendingUAVCpuHandles[0],
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                m_commandList4->SetComputeRootDescriptorTable(2, staging.gpuHandle);
            } else {
                CFFLog::Error("[DX12CommandList] BindPendingResourcesRayTracing: Failed to allocate UAV staging descriptors");
            }
        }
        m_uavDirty = false;
    }

    // Bind Sampler table (parameter 3) - s0
    if (m_samplerDirty) {
        if (m_pendingSamplerCpuHandles[0].ptr != 0) {
            auto& samplerStagingRing = heapMgr.GetSamplerStagingRing();
            SDescriptorHandle staging = samplerStagingRing.AllocateContiguous(1);

            if (staging.IsValid()) {
                device->CopyDescriptorsSimple(1, staging.cpuHandle, m_pendingSamplerCpuHandles[0],
                                              D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                m_commandList4->SetComputeRootDescriptorTable(3, staging.gpuHandle);
            }
        } else {
            // Allocate dummy sampler for unbound slot
            auto& samplerStagingRing = heapMgr.GetSamplerStagingRing();
            SDescriptorHandle staging = samplerStagingRing.AllocateContiguous(1);
            if (staging.IsValid()) {
                m_commandList4->SetComputeRootDescriptorTable(3, staging.gpuHandle);
            }
        }
        m_samplerDirty = false;
    }
}

// ============================================
// Ray Tracing Commands
// ============================================

void CDX12CommandList::BuildAccelerationStructure(IAccelerationStructure* as) {
    if (!as) {
        CFFLog::Warning("[DX12CommandList] BuildAccelerationStructure: null acceleration structure");
        return;
    }

    // Use cached ID3D12GraphicsCommandList4 (queried once in Initialize)
    if (!m_commandList4) {
        CFFLog::Error("[DX12CommandList] BuildAccelerationStructure: ID3D12GraphicsCommandList4 not available");
        return;
    }

    auto* dx12AS = static_cast<CDX12AccelerationStructure*>(as);

    // Result buffer is already created in RAYTRACING_ACCELERATION_STRUCTURE state
    // Scratch buffer needs to be transitioned from COMMON to UNORDERED_ACCESS
    ID3D12Resource* resultBuffer = dx12AS->GetResultBuffer();
    ID3D12Resource* scratchBuffer = dx12AS->GetScratchBuffer();

    if (scratchBuffer) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = scratchBuffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    // Get build desc from acceleration structure
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = dx12AS->GetBuildDesc();

    // Execute the build
    m_commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // Insert UAV barrier to ensure build completes before use
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = resultBuffer;
    m_commandList->ResourceBarrier(1, &uavBarrier);

    dx12AS->MarkBuilt();
}

void CDX12CommandList::SetRayTracingPipelineState(IRayTracingPipelineState* pso) {
    if (!pso) {
        CFFLog::Warning("[DX12CommandList] SetRayTracingPipelineState: null PSO");
        return;
    }

    // Use cached ID3D12GraphicsCommandList4 (queried once in Initialize)
    if (!m_commandList4) {
        CFFLog::Error("[DX12CommandList] SetRayTracingPipelineState: ID3D12GraphicsCommandList4 not available");
        return;
    }

    ID3D12StateObject* stateObject = static_cast<ID3D12StateObject*>(pso->GetNativeHandle());
    m_commandList4->SetPipelineState1(stateObject);

    // Re-set the root signature after SetPipelineState1
    // According to DXR samples, root signature should be set after pipeline state
    ID3D12RootSignature* rtRootSig = m_context->GetRayTracingRootSignature();
    if (rtRootSig) {
        m_commandList->SetComputeRootSignature(rtRootSig);
    }
}

void CDX12CommandList::DispatchRays(const DispatchRaysDesc& desc) {
    if (!desc.shaderBindingTable) {
        CFFLog::Warning("[DX12CommandList] DispatchRays: null SBT");
        return;
    }

    // Use cached ID3D12GraphicsCommandList4 (queried once in Initialize)
    if (!m_commandList4) {
        CFFLog::Error("[DX12CommandList] DispatchRays: ID3D12GraphicsCommandList4 not available");
        return;
    }

    // Bind pending resources (SRV/UAV/Sampler tables) for ray tracing
    BindPendingResourcesRayTracing();

    // Flush any pending resource barriers before dispatch
    FlushBarriers();

    IShaderBindingTable* sbt = desc.shaderBindingTable;

    D3D12_DISPATCH_RAYS_DESC d3dDesc = {};

    // Ray generation shader record
    d3dDesc.RayGenerationShaderRecord.StartAddress = sbt->GetRayGenShaderRecordAddress();
    d3dDesc.RayGenerationShaderRecord.SizeInBytes = sbt->GetRayGenShaderRecordSize();

    // Miss shader table
    d3dDesc.MissShaderTable.StartAddress = sbt->GetMissShaderTableAddress();
    d3dDesc.MissShaderTable.SizeInBytes = sbt->GetMissShaderTableSize();
    d3dDesc.MissShaderTable.StrideInBytes = sbt->GetMissShaderTableStride();

    // Hit group table (address must be 0 if size is 0)
    uint64_t hitGroupSize = sbt->GetHitGroupTableSize();
    d3dDesc.HitGroupTable.StartAddress = (hitGroupSize > 0) ? sbt->GetHitGroupTableAddress() : 0;
    d3dDesc.HitGroupTable.SizeInBytes = hitGroupSize;
    d3dDesc.HitGroupTable.StrideInBytes = sbt->GetHitGroupTableStride();

    // Dispatch dimensions
    d3dDesc.Width = desc.width;
    d3dDesc.Height = desc.height;
    d3dDesc.Depth = desc.depth;

    m_commandList4->DispatchRays(&d3dDesc);
}

void CDX12CommandList::SetAccelerationStructure(uint32_t slot, IAccelerationStructure* tlas) {
    if (!tlas) {
        CFFLog::Warning("[DX12CommandList] SetAccelerationStructure: null TLAS");
        return;
    }

    // Get the GPU virtual address of the TLAS
    uint64_t gpuVA = tlas->GetGPUVirtualAddress();
    if (gpuVA == 0) {
        CFFLog::Warning("[DX12CommandList] SetAccelerationStructure: TLAS has no GPU address");
        return;
    }

    // For ray tracing, the TLAS is accessed via its GPU virtual address
    // The shader uses: RaytracingAccelerationStructure g_Scene : register(t0);
    //
    // Create an SRV descriptor for the TLAS and put it in the pending SRV slot.
    // The acceleration structure SRV has a special format with ViewDimension =
    // D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE

    // Allocate a descriptor from the CPU-side CBV/SRV/UAV heap
    auto& heapManager = CDX12DescriptorHeapManager::Instance();
    SDescriptorHandle handle = heapManager.AllocateCBVSRVUAV();
    if (!handle.IsValid()) {
        CFFLog::Error("[DX12CommandList] SetAccelerationStructure: Failed to allocate descriptor");
        return;
    }

    // Create an SRV descriptor for the TLAS
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = gpuVA;

    // Create the SRV (note: pResource is nullptr for acceleration structures)
    m_context->GetDevice()->CreateShaderResourceView(nullptr, &srvDesc, handle.cpuHandle);

    // Store in pending SRV slot for binding
    if (slot < MAX_SRV_SLOTS) {
        m_pendingSRVCpuHandles[slot] = handle.cpuHandle;
        m_srvDirty = true;
    }

    // Note: We're leaking the descriptor handle here since we don't track it.
    // For a production implementation, we would need to track and free it
    // after the command list completes. For now, this works but wastes descriptors.
}

} // namespace DX12
} // namespace RHI
