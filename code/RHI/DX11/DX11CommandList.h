#pragma once
#include "../ICommandList.h"
#include "DX11Resources.h"
#include <d3d11.h>

namespace RHI {
namespace DX11 {

class CDX11CommandList : public ICommandList {
public:
    CDX11CommandList(ID3D11DeviceContext* context);
    ~CDX11CommandList() override = default;

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
    void SetConstantBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) override;
    void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) override;
    void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) override;
    void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) override;
    void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) override;
    void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) override;

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

    // Unbind Operations
    void UnbindRenderTargets() override;
    void UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t numSlots) override;

private:
    ID3D11DeviceContext* m_context;  // Non-owning
};

} // namespace DX11
} // namespace RHI
