#pragma once
#include "RHICommon.h"
#include "RHIResources.h"

// ============================================
// Command List Interface
// ============================================

namespace RHI {

// Forward declarations for ray tracing
class IAccelerationStructure;
class IShaderBindingTable;
class IRayTracingPipelineState;
struct DispatchRaysDesc;

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

    // Set render target to specific array slice (for cubemap face rendering)
    // renderTarget: texture with RTV slice views created
    // arraySlice: array slice index (0-5 for cubemap faces)
    // depthStencil: depth stencil texture (can be nullptr)
    virtual void SetRenderTargetSlice(ITexture* renderTarget, uint32_t arraySlice, ITexture* depthStencil) = 0;

    // Set depth stencil only (for depth-only rendering like shadow maps)
    // Uses specific array slice for texture arrays (CSM support)
    virtual void SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice = 0) = 0;


    // Clear render target
    virtual void ClearRenderTarget(ITexture* renderTarget, const float color[4]) = 0;

    // Clear depth stencil
    virtual void ClearDepthStencil(ITexture* depthStencil, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) = 0;

    // Clear depth stencil array slice (for CSM shadow mapping)
    virtual void ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) = 0;

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

    // Set constant buffer data directly (PREFERRED for per-frame/per-draw data)
    // This allocates from a per-frame ring buffer and binds the data inline
    // - DX12: Allocates from CDX12DynamicBufferRing, each call gets unique GPU address
    // - DX11: Falls back to UpdateSubresource (safe due to driver-managed buffering)
    // data: pointer to constant data
    // size: size of data in bytes (must be 256-byte aligned in DX12)
    // Returns true if successful
    virtual bool SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) = 0;

    // Set shader resource (texture or structured buffer)
    virtual void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
    virtual void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;

    // Set sampler
    virtual void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) = 0;

    // Set unordered access view (for compute shaders or pixel shader UAV)
    virtual void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) = 0;
    virtual void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) = 0;
    virtual void SetUnorderedAccessTextureMip(uint32_t slot, ITexture* texture, uint32_t mipLevel) = 0;

    // Clear UAV buffer with uint values (for resetting atomic counters, etc.)
    // values: array of 4 uint32_t values to clear with
    virtual void ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) = 0;

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

    // ============================================
    // Copy Operations
    // ============================================

    // Copy entire texture
    virtual void CopyTexture(ITexture* dst, ITexture* src) = 0;

    // Copy texture to specific array slice and mip level of destination
    virtual void CopyTextureToSlice(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src) = 0;

    // Copy specific subresource from source to destination
    // Useful for cubemap array operations where both src and dst have array slices
    virtual void CopyTextureSubresource(
        ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel,
        ITexture* src, uint32_t srcArraySlice, uint32_t srcMipLevel) = 0;

    // ============================================
    // Mipmap Generation
    // ============================================

    // Generate mipmaps for a texture
    // Texture must have been created with GENERATE_MIPS flag
    virtual void GenerateMips(ITexture* texture) = 0;

    // ============================================
    // Unbind Operations (for resource hazard prevention)
    // ============================================

    // Unbind render targets (set all RT slots to nullptr)
    virtual void UnbindRenderTargets() = 0;

    // Unbind shader resources for a stage (slots 0-7)
    virtual void UnbindShaderResources(EShaderStage stage, uint32_t startSlot = 0, uint32_t numSlots = 8) = 0;

    // ============================================
    // Debug Events (GPU profiling markers for RenderDoc/PIX)
    // ============================================

    // Begin a named event region
    virtual void BeginEvent(const wchar_t* name) = 0;

    // End the current event region
    virtual void EndEvent() = 0;

    // ============================================
    // Ray Tracing Commands (DXR)
    // ============================================
    // These methods are no-ops on backends that don't support ray tracing.

    // Build acceleration structure (BLAS or TLAS)
    // Executes the build on GPU command queue
    virtual void BuildAccelerationStructure(IAccelerationStructure* as) = 0;

    // Set ray tracing pipeline state
    virtual void SetRayTracingPipelineState(IRayTracingPipelineState* pso) = 0;

    // Dispatch rays
    // Launches ray generation shaders
    virtual void DispatchRays(const DispatchRaysDesc& desc) = 0;

    // Set acceleration structure for shader access
    // Binds TLAS to shader resource slot for TraceRay() calls
    virtual void SetAccelerationStructure(uint32_t slot, IAccelerationStructure* tlas) = 0;
};

// RAII wrapper for debug events
// Usage:
//   {
//       RHI::CScopedDebugEvent evt(cmdList, L"Shadow Pass");
//       // ... rendering code ...
//   } // Automatically calls EndEvent
class CScopedDebugEvent {
public:
    CScopedDebugEvent(ICommandList* cmdList, const wchar_t* name)
        : m_cmdList(cmdList)
    {
        if (m_cmdList) {
            m_cmdList->BeginEvent(name);
        }
    }

    ~CScopedDebugEvent() {
        if (m_cmdList) {
            m_cmdList->EndEvent();
        }
    }

    // Delete copy/move to prevent misuse
    CScopedDebugEvent(const CScopedDebugEvent&) = delete;
    CScopedDebugEvent& operator=(const CScopedDebugEvent&) = delete;

private:
    ICommandList* m_cmdList;
};

} // namespace RHI
