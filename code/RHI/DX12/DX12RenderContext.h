#pragma once

#include "DX12Common.h"
#include "DX12DescriptorHeap.h"
#include "DX12CommandList.h"
#include "DX12DynamicBuffer.h"
#include "DX12GenerateMipsPass.h"
#include "../IRenderContext.h"
#include "../RHIRayTracing.h"

// ============================================
// DX12 Render Context Implementation
// ============================================

namespace RHI {
namespace DX12 {

class CDX12RenderContext : public IRenderContext {
public:
    CDX12RenderContext();
    ~CDX12RenderContext() override;

    // ============================================
    // IRenderContext Implementation
    // ============================================

    // Lifecycle
    bool Initialize(void* nativeWindowHandle, uint32_t width, uint32_t height) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

    // Frame Control
    void BeginFrame() override;
    void EndFrame() override;
    void Present(bool vsync) override;

    // Command List Access
    ICommandList* GetCommandList() override;

    // Resource Creation
    IBuffer* CreateBuffer(const BufferDesc& desc, const void* initialData = nullptr) override;
    ITexture* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) override;
    ITexture* CreateTextureWithData(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources) override;
    ISampler* CreateSampler(const SamplerDesc& desc) override;
    IShader* CreateShader(const ShaderDesc& desc) override;
    IPipelineState* CreatePipelineState(const PipelineStateDesc& desc) override;
    IPipelineState* CreateComputePipelineState(const ComputePipelineDesc& desc) override;
    ITexture* WrapNativeTexture(void* nativeTexture, void* nativeSRV, uint32_t width, uint32_t height, ETextureFormat format) override;
    ITexture* WrapExternalTexture(void* nativeTexture, const TextureDesc& desc) override;

    // Backbuffer Access
    ITexture* GetBackbuffer() override;
    ITexture* GetDepthStencil() override;

    // Query
    EBackend GetBackend() const override { return EBackend::DX12; }
    uint32_t GetWidth() const override;
    uint32_t GetHeight() const override;
    bool SupportsRaytracing() const override;
    bool SupportsAsyncCompute() const override { return true; }  // DX12 always supports
    bool SupportsMeshShaders() const override;

    // Advanced
    void* GetNativeDevice() override;
    void* GetNativeContext() override;

    // Ray Tracing (DXR)
    AccelerationStructurePrebuildInfo GetAccelerationStructurePrebuildInfo(const BLASDesc& desc) override;
    AccelerationStructurePrebuildInfo GetAccelerationStructurePrebuildInfo(const TLASDesc& desc) override;
    IAccelerationStructure* CreateBLAS(const BLASDesc& desc, IBuffer* scratchBuffer, IBuffer* resultBuffer) override;
    IAccelerationStructure* CreateTLAS(const TLASDesc& desc, IBuffer* scratchBuffer, IBuffer* resultBuffer, IBuffer* instanceBuffer) override;
    IRayTracingPipelineState* CreateRayTracingPipelineState(const RayTracingPipelineDesc& desc) override;
    IShaderBindingTable* CreateShaderBindingTable(const ShaderBindingTableDesc& desc) override;

    // ============================================
    // DX12-Specific Accessors
    // ============================================

    ID3D12Device* GetDevice() const;
    ID3D12CommandQueue* GetCommandQueue() const;
    ID3D12RootSignature* GetGraphicsRootSignature() const { return m_graphicsRootSignature.Get(); }
    ID3D12RootSignature* GetComputeRootSignature() const { return m_computeRootSignature.Get(); }

    // Access to internal passes
    CDX12GenerateMipsPass& GetGenerateMipsPass() { return m_generateMipsPass; }

private:
    // Root signature creation
    bool CreateRootSignatures();

    // Depth stencil management
    void CreateDepthStencilBuffer();
    void ReleaseDepthStencilBuffer();

    // Internal texture creation helper
    ITexture* CreateTextureInternal(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources);

private:
    // Command list
    std::unique_ptr<CDX12CommandList> m_commandList;

    // Root signatures (shared by all PSOs)
    ComPtr<ID3D12RootSignature> m_graphicsRootSignature;
    ComPtr<ID3D12RootSignature> m_computeRootSignature;

    // Backbuffer wrappers (one per frame in flight)
    std::unique_ptr<class CDX12Texture> m_backbufferWrappers[3];

    // Depth stencil buffer
    std::unique_ptr<class CDX12Texture> m_depthStencilBuffer;

    // Dynamic constant buffer ring for per-draw data
    std::unique_ptr<CDX12DynamicBufferRing> m_dynamicBufferRing;

    // GenerateMips compute pass
    CDX12GenerateMipsPass m_generateMipsPass;

    // Frame state
    bool m_frameInProgress = false;

    // Helper to create backbuffer wrappers
    void CreateBackbufferWrappers();
    void ReleaseBackbufferWrappers();
};

} // namespace DX12
} // namespace RHI
