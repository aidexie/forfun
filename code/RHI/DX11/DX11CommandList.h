#pragma once
#include "../ICommandList.h"
#include "../RHIRayTracing.h"
#include "DX11Resources.h"
#include <d3d11.h>
#include <vector>
#include <unordered_map>
#include <wrl/client.h>

// Forward declaration
struct ID3DUserDefinedAnnotation;

namespace RHI {
namespace DX11 {

class CDX11CommandList : public ICommandList {
public:
    CDX11CommandList(ID3D11DeviceContext* context, ID3D11Device* device);
    ~CDX11CommandList() override;

    // Reset per-frame state (call at BeginFrame)
    void ResetFrame();

    // Render Target Operations
    void SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil) override;
    void SetRenderTargetSlice(ITexture* renderTarget, uint32_t arraySlice, ITexture* depthStencil) override;
    void SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice) override;
    void ClearRenderTarget(ITexture* renderTarget, const float color[4]) override;
    void ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;
    void ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;

    // Pipeline State
    void SetPipelineState(IPipelineState* pso) override;
    void SetPrimitiveTopology(EPrimitiveTopology topology) override;
    void SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth) override;
    void SetScissorRect(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) override;

    // Resource Binding
    void SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset) override;
    void SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset) override;
    void ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) override;

    // Draw Commands
    void Draw(uint32_t vertexCount, uint32_t startVertex) override;
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) override;
    void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                      uint32_t startVertex, uint32_t startInstance) override;
    void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                             uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) override;

    // Compute Commands
    void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;

    // Resource Barriers (DX11 no-op)
    void Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter) override;
    void UAVBarrier(IResource* resource) override;

    // Copy Operations
    void CopyTexture(ITexture* dst, ITexture* src) override;
    void CopyTextureToSlice(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src) override;
    void CopyTextureSubresource(
        ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel,
        ITexture* src, uint32_t srcArraySlice, uint32_t srcMipLevel) override;
    void CopyBuffer(IBuffer* dst, uint64_t dstOffset, IBuffer* src, uint64_t srcOffset, uint64_t numBytes) override;

    // Mipmap Generation
    void GenerateMips(ITexture* texture) override;

    // Unbind Operations
    void UnbindRenderTargets() override;

    // Debug Events
    void BeginEvent(const wchar_t* name) override;
    void EndEvent() override;

    // Ray Tracing (stubs - DX11 doesn't support ray tracing)
    void BuildAccelerationStructure(IAccelerationStructure* as) override {}
    void SetRayTracingPipelineState(IRayTracingPipelineState* pso) override {}
    void DispatchRays(const DispatchRaysDesc& desc) override {}
    void SetAccelerationStructure(uint32_t slot, IAccelerationStructure* tlas) override {}

    // Descriptor Set Binding (stub - DX11 doesn't support descriptor sets)
    void BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) override {}

    // Native Access (DX11 doesn't expose command list)
    void* GetNativeCommandList() override { return nullptr; }

private:
    ID3D11DeviceContext* m_context;  // Non-owning
    ID3D11Device* m_device;          // Non-owning, for creating dynamic CBs
    ID3DUserDefinedAnnotation* m_annotation = nullptr;  // Owned, for debug events

    // Debug tracking
    const wchar_t* m_currentEventName = nullptr;  // Current debug event name for error tracking
    IPipelineState* m_currentPSO = nullptr;  // Current PSO for validation

    // Dynamic constant buffer pool for SetConstantBufferData
    // Each size bucket has its own pool of buffers
    struct DynamicCBPool {
        std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> buffers;
        size_t nextIndex = 0;
    };
    std::unordered_map<size_t, DynamicCBPool> m_dynamicCBPools;  // Key = aligned size

    // Get or create a dynamic constant buffer of the given size
    ID3D11Buffer* AcquireDynamicCB(size_t size);
};

} // namespace DX11
} // namespace RHI
