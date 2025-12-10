#pragma once
#include "RHICommon.h"
#include "RHIResources.h"

// ============================================
// Command List Interface
// ============================================

namespace RHI {

class ICommandList {
public:
    virtual ~ICommandList() = default;

    // ============================================
    // Render Target Operations
    // ============================================

    // Set render targets (pass nullptr for backbuffer)
    // numRTs: number of render targets (0-8)
    // renderTargets: array of render target textures
    // depthStencil: depth stencil texture (can be nullptr)
    virtual void SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil) = 0;

    // Clear render target
    virtual void ClearRenderTarget(ITexture* renderTarget, const float color[4]) = 0;

    // Clear depth stencil
    virtual void ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) = 0;

    // ============================================
    // Pipeline State
    // ============================================

    // Set graphics pipeline state
    virtual void SetPipelineState(IPipelineState* pso) = 0;

    // Set primitive topology (if not using PSO topology)
    virtual void SetPrimitiveTopology(EPrimitiveTopology topology) = 0;

    // Set viewport
    virtual void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) = 0;

    // Set scissor rect
    virtual void SetScissorRect(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) = 0;

    // ============================================
    // Resource Binding
    // ============================================

    // Set vertex buffer
    virtual void SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset = 0) = 0;

    // Set index buffer
    virtual void SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset = 0) = 0;

    // Set constant buffer
    virtual void SetConstantBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;

    // Set shader resource (texture or structured buffer)
    virtual void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
    virtual void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;

    // Set sampler
    virtual void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) = 0;

    // Set unordered access view (for compute shaders or pixel shader UAV)
    virtual void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) = 0;
    virtual void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) = 0;

    // ============================================
    // Draw Commands
    // ============================================

    // Draw primitives
    virtual void Draw(uint32_t vertexCount, uint32_t startVertex = 0) = 0;

    // Draw indexed primitives
    virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, int32_t baseVertex = 0) = 0;

    // Draw instanced
    virtual void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                               uint32_t startVertex = 0, uint32_t startInstance = 0) = 0;

    // Draw indexed instanced
    virtual void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                                      uint32_t startIndex = 0, int32_t baseVertex = 0, uint32_t startInstance = 0) = 0;

    // ============================================
    // Compute Commands
    // ============================================

    // Dispatch compute shader
    virtual void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;

    // ============================================
    // Resource Barriers (DX12 explicit, DX11 no-op)
    // ============================================

    // Transition resource state
    virtual void Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter) = 0;

    // UAV barrier (ensure all UAV writes complete before next read)
    virtual void UAVBarrier(IResource* resource) = 0;
};

} // namespace RHI
