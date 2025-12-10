#pragma once
#include "RHICommon.h"
#include "RHIDescriptors.h"
#include "RHIResources.h"
#include "ICommandList.h"

// ============================================
// Render Context Interface (Device + SwapChain)
// ============================================

namespace RHI {

class IRenderContext {
public:
    virtual ~IRenderContext() = default;

    // ============================================
    // Lifecycle
    // ============================================

    // Initialize device and swapchain
    // nativeWindowHandle: HWND on Windows
    virtual bool Initialize(void* nativeWindowHandle, uint32_t width, uint32_t height) = 0;

    // Shutdown and release resources
    virtual void Shutdown() = 0;

    // Handle window resize
    virtual void OnResize(uint32_t width, uint32_t height) = 0;

    // ============================================
    // Frame Control
    // ============================================

    // Begin frame (prepare command list)
    virtual void BeginFrame() = 0;

    // End frame (submit commands)
    virtual void EndFrame() = 0;

    // Present backbuffer
    virtual void Present(bool vsync) = 0;

    // ============================================
    // Command List Access
    // ============================================

    // Get command list for recording commands
    // In DX11: returns immediate context wrapper
    // In DX12: returns current frame's command list
    virtual ICommandList* GetCommandList() = 0;

    // ============================================
    // Resource Creation
    // ============================================

    // Create buffer
    virtual IBuffer* CreateBuffer(const BufferDesc& desc, const void* initialData = nullptr) = 0;

    // Create texture
    virtual ITexture* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) = 0;

    // Create sampler
    virtual ISampler* CreateSampler(const SamplerDesc& desc) = 0;

    // Create shader from compiled bytecode
    virtual IShader* CreateShader(const ShaderDesc& desc) = 0;

    // Create graphics pipeline state
    virtual IPipelineState* CreatePipelineState(const PipelineStateDesc& desc) = 0;

    // Create compute pipeline state
    virtual IPipelineState* CreateComputePipelineState(const ComputePipelineDesc& desc) = 0;

    // Wrap an existing native texture (e.g., from WIC loader) into RHI abstraction
    // DX11: nativeSRV is ID3D11ShaderResourceView*, nativeTexture is ID3D11Texture2D* (can be nullptr)
    // The RHI takes ownership of these resources
    virtual ITexture* WrapNativeTexture(void* nativeTexture, void* nativeSRV, uint32_t width, uint32_t height, ETextureFormat format) = 0;

    // Wrap an existing native texture (e.g., cubemap from bakers) without taking ownership
    // Useful for passing D3D11 textures to RHI copy operations
    // The caller retains ownership of the native texture
    // desc: Used to provide metadata about the texture (width, height, format, isCubemap, etc.)
    virtual ITexture* WrapExternalTexture(void* nativeTexture, const TextureDesc& desc) = 0;

    // ============================================
    // Backbuffer Access
    // ============================================

    // Get backbuffer as texture (for setting as render target)
    virtual ITexture* GetBackbuffer() = 0;

    // Get default depth stencil buffer
    virtual ITexture* GetDepthStencil() = 0;

    // ============================================
    // Query
    // ============================================

    // Get backend type
    virtual EBackend GetBackend() const = 0;

    // Get current render target dimensions
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    // Check feature support
    virtual bool SupportsRaytracing() const = 0;
    virtual bool SupportsAsyncCompute() const = 0;
    virtual bool SupportsMeshShaders() const = 0;

    // ============================================
    // Advanced (for low-level access if needed)
    // ============================================

    // Get native device handle
    // DX11: returns ID3D11Device*
    // DX12: returns ID3D12Device*
    virtual void* GetNativeDevice() = 0;
    
    // Get native device context handle (for immediate context)
    // DX11: returns ID3D11DeviceContext*
    // DX12: returns ID3D12GraphicsCommandList* (of current command list)
    virtual void* GetNativeContext() = 0;
};

} // namespace RHI
