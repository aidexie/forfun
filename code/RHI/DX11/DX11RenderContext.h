#pragma once
#include "../IRenderContext.h"
#include "DX11CommandList.h"
#include "DX11Resources.h"
#include "Core/DX11Context.h"
#include <memory>

namespace RHI {
namespace DX11 {

class CDX11RenderContext : public IRenderContext {
public:
    CDX11RenderContext();
    ~CDX11RenderContext() override;

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
    IBuffer* CreateBuffer(const BufferDesc& desc, const void* initialData) override;
    ITexture* CreateTexture(const TextureDesc& desc, const void* initialData) override;
    ISampler* CreateSampler(const SamplerDesc& desc) override;
    IShader* CreateShader(const ShaderDesc& desc) override;
    IPipelineState* CreatePipelineState(const PipelineStateDesc& desc) override;
    IPipelineState* CreateComputePipelineState(const ComputePipelineDesc& desc) override;

    // Backbuffer Access
    ITexture* GetBackbuffer() override;
    ITexture* GetDepthStencil() override;

    // Query
    EBackend GetBackend() const override { return EBackend::DX11; }
    uint32_t GetWidth() const override;
    uint32_t GetHeight() const override;
    bool SupportsRaytracing() const override { return false; }
    bool SupportsAsyncCompute() const override { return false; }
    bool SupportsMeshShaders() const override { return false; }

    // Advanced
    void* GetNativeDevice() override;
    void* GetNativeContext() override;

private:
    std::unique_ptr<CDX11CommandList> m_commandList;
    std::unique_ptr<CDX11Texture> m_backbufferWrapper;
    std::unique_ptr<CDX11Texture> m_depthStencilWrapper;
    bool m_initialized = false;
};

} // namespace DX11
} // namespace RHI
