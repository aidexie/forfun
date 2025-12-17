#pragma once

#include "DX12Common.h"
#include "DX12ResourceStateTracker.h"
#include "DX12DynamicBuffer.h"
#include "../ICommandList.h"

// ============================================
// DX12 Command List Implementation
// ============================================

namespace RHI {
namespace DX12 {

// Forward declarations
class CDX12RenderContext;
class CDX12Buffer;
class CDX12Texture;
class CDX12Sampler;
class CDX12PipelineState;

class CDX12CommandList : public ICommandList {
public:
    CDX12CommandList(CDX12RenderContext* context);
    ~CDX12CommandList() override;

    // Initialize command list
    bool Initialize();

    // Reset for new frame
    void Reset(ID3D12CommandAllocator* allocator);

    // Close command list (before execution)
    void Close();

    // Get native command list
    ID3D12GraphicsCommandList* GetNativeCommandList() { return m_commandList.Get(); }

    // Get resource state tracker
    CDX12ResourceStateTracker& GetStateTracker() { return m_stateTracker; }

    // ============================================
    // ICommandList Implementation
    // ============================================

    // Render Target Operations
    void SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil) override;
    void SetRenderTargetSlice(ITexture* renderTarget, uint32_t arraySlice, ITexture* depthStencil) override;
    void SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice = 0) override;
    void ClearRenderTarget(ITexture* renderTarget, const float color[4]) override;
    void ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;
    void ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;

    // Pipeline State
    void SetPipelineState(IPipelineState* pso) override;
    void SetPrimitiveTopology(EPrimitiveTopology topology) override;
    void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) override;
    void SetScissorRect(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) override;

    // Resource Binding
    void SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset = 0) override;
    void SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset = 0) override;
    bool SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) override;
    void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) override;
    void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) override;
    void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) override;
    void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) override;
    void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) override;
    void ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) override;

    // Draw Commands
    void Draw(uint32_t vertexCount, uint32_t startVertex = 0) override;
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, int32_t baseVertex = 0) override;
    void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertex = 0, uint32_t startInstance = 0) override;
    void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndex = 0, int32_t baseVertex = 0, uint32_t startInstance = 0) override;

    // Compute Commands
    void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;

    // Resource Barriers
    void Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter) override;
    void UAVBarrier(IResource* resource) override;

    // Copy Operations
    void CopyTexture(ITexture* dst, ITexture* src) override;
    void CopyTextureToSlice(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src) override;
    void CopyTextureSubresource(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src, uint32_t srcArraySlice, uint32_t srcMipLevel) override;

    // Mipmap Generation
    void GenerateMips(ITexture* texture) override;

    // Unbind Operations
    void UnbindRenderTargets() override;
    void UnbindShaderResources(EShaderStage stage, uint32_t startSlot = 0, uint32_t numSlots = 8) override;

    // Debug Events
    void BeginEvent(const wchar_t* name) override;
    void EndEvent() override;

private:
    // Helper to transition resource to required state
    void TransitionResource(CDX12Texture* texture, D3D12_RESOURCE_STATES targetState);
    void TransitionResource(CDX12Buffer* buffer, D3D12_RESOURCE_STATES targetState);

    // Flush pending barriers before draw/dispatch
    void FlushBarriers();

    // Bind descriptor heaps if needed
    void EnsureDescriptorHeapsBound();

private:
    CDX12RenderContext* m_context = nullptr;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // Resource state tracking
    CDX12ResourceStateTracker m_stateTracker;

    // Current bound state
    CDX12PipelineState* m_currentPSO = nullptr;
    bool m_descriptorHeapsBound = false;

    // Cached primitive topology
    D3D12_PRIMITIVE_TOPOLOGY m_currentTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // Pending CBV bindings (GPU virtual addresses)
    // These are bound as root CBVs before draw calls (after PSO is set)
    static constexpr uint32_t MAX_CBV_SLOTS = 7;
    D3D12_GPU_VIRTUAL_ADDRESS m_pendingCBVs[MAX_CBV_SLOTS] = {};
    bool m_cbvDirty = false;

    // Pending SRV bindings (CPU handles from non-shader-visible heap)
    // These are copied to a contiguous staging region before binding
    static constexpr uint32_t MAX_SRV_SLOTS = 25;
    static constexpr uint32_t MAX_SAMPLER_SLOTS = 8;
    static constexpr uint32_t MAX_UAV_SLOTS = 8;
    D3D12_CPU_DESCRIPTOR_HANDLE m_pendingSRVCpuHandles[MAX_SRV_SLOTS] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_pendingSamplerCpuHandles[MAX_SAMPLER_SLOTS] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_pendingUAVCpuHandles[MAX_UAV_SLOTS] = {};
    bool m_srvDirty = false;
    bool m_samplerDirty = false;
    bool m_uavDirty = false;

    // Track if current PSO is compute (for choosing Graphics vs Compute root calls)
    bool m_isComputePSO = false;

    // Bind pending resources before draw (requires PSO to be set first)
    void BindPendingResources();
    // Bind pending resources for compute dispatch
    void BindPendingResourcesCompute();

    // Dynamic constant buffer ring (owned by RenderContext, set during initialization)
    CDX12DynamicBufferRing* m_dynamicBuffer = nullptr;

public:
    // Set dynamic buffer ring (called by RenderContext)
    void SetDynamicBufferRing(CDX12DynamicBufferRing* ring) { m_dynamicBuffer = ring; }
};

} // namespace DX12
} // namespace RHI
