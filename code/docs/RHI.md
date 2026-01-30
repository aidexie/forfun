# RHI (Render Hardware Interface) Documentation

## Overview

本引擎使用 **RHI (Render Hardware Interface)** 抽象层来支持多个图形 API 后端。当前支持 **DX11** 和 **DX12**，可在运行时切换。

**完成日期**: 2025-12-18
**当前状态**: ✅ DX12 迁移完成，双后端可用

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│         (SceneRenderer, ShadowPass, ClusteredLighting)       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      RHI Public API                          │
│              (IRenderContext, ICommandList)                  │
└─────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┴───────────────┐
              ▼                               ▼
┌─────────────────────────┐     ┌─────────────────────────┐
│      DX11 Backend       │     │      DX12 Backend       │
│   (CDX11RenderContext)  │     │   (CDX12RenderContext)  │
│   (CDX11CommandList)    │     │   (CDX12CommandList)    │
│   (CDX11Buffer/Texture) │     │   (CDX12Buffer/Texture) │
└─────────────────────────┘     └─────────────────────────┘
              │                               │
              ▼                               ▼
┌─────────────────────────┐     ┌─────────────────────────┐
│    Direct3D 11 API      │     │    Direct3D 12 API      │
└─────────────────────────┘     └─────────────────────────┘
```

---

## Backend Selection

**配置文件**: `render.json` (项目根目录)
```json
{
  "renderBackend": "DX11"
}
```
可选值: `"DX11"` (默认, 稳定) | `"DX12"` (完整功能)

---

## RHI Public Interfaces

### IRenderContext

设备和资源管理接口：

```cpp
class IRenderContext {
    // Lifecycle
    void Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height);

    // Frame management
    void BeginFrame();
    void EndFrame();
    void Present();

    // Resource creation
    IBuffer* CreateBuffer(const BufferDesc& desc, const void* initialData);
    ITexture* CreateTexture(const TextureDesc& desc, const SubresourceData* data);
    ISampler* CreateSampler(const SamplerDesc& desc);
    IShader* CreateShader(const ShaderDesc& desc);
    IPipelineState* CreatePipelineState(const PipelineStateDesc& desc);
    IPipelineState* CreateComputePipelineState(const ComputePipelineDesc& desc);

    // Backbuffer access
    ITexture* GetBackbuffer();
    ITexture* GetDepthStencil();
    ICommandList* GetCommandList();
};
```

### ICommandList

命令录制接口：

```cpp
class ICommandList {
    // Render targets
    void SetRenderTargets(ITexture** rts, uint32_t count, ITexture* depthStencil);
    void ClearRenderTarget(ITexture* rt, const float* color);
    void ClearDepthStencil(ITexture* ds, float depth, uint8_t stencil);

    // Pipeline state
    void SetPipelineState(IPipelineState* pso);
    void SetViewport(float x, float y, float w, float h, float minDepth, float maxDepth);
    void SetScissorRect(int32_t x, int32_t y, uint32_t w, uint32_t h);
    void SetPrimitiveTopology(EPrimitiveTopology topology);

    // Resource binding
    void SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset);
    void SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset);
    void SetConstantBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer);
    void SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size);
    void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture);
    void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer);
    void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler);
    void SetUnorderedAccess(uint32_t slot, IBuffer* buffer);
    void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture);

    // Draw commands
    void Draw(uint32_t vertexCount, uint32_t startVertex);
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex);
    void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance);
    void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex, uint32_t startInstance);

    // Compute
    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

    // Misc
    void GenerateMips(ITexture* texture);
    void CopyTexture(ITexture* dst, ITexture* src);
    void BeginEvent(const char* name);
    void EndEvent();
};
```

---

## File Structure

```
RHI/
├── ICommandList.h              # Command list interface
├── IRenderContext.h            # Render context interface
├── RHICommon.h                 # Common enums and types
├── RHIDescriptors.h            # Resource descriptors
├── RHIResources.h              # Resource interfaces (IBuffer, ITexture, etc.)
├── RHIPointers.h               # Smart pointer typedefs
├── RHIFactory.cpp              # Backend factory
├── RHIManager.cpp              # Singleton manager
├── ShaderCompiler.h            # Shader compilation interface
│
├── DX11/                       # DX11 Backend
│   ├── DX11Context.h/cpp       # Device, SwapChain
│   ├── DX11RenderContext.h/cpp # IRenderContext implementation
│   ├── DX11CommandList.h/cpp   # ICommandList implementation
│   ├── DX11Resources.h         # CDX11Buffer, CDX11Texture, etc.
│   ├── DX11Buffer.cpp
│   ├── DX11Texture.cpp
│   └── DX11ShaderCompiler.cpp
│
└── DX12/                       # DX12 Backend
    ├── DX12Common.h            # ComPtr, DX12_CHECK macro, utilities
    ├── DX12Context.h/cpp       # Device, SwapChain, Fence, DeferredDeletionQueue
    ├── DX12RenderContext.h/cpp # IRenderContext implementation
    ├── DX12CommandList.h/cpp   # ICommandList implementation
    ├── DX12Resources.h         # CDX12Buffer, CDX12Texture, etc.
    ├── DX12Buffer.cpp
    ├── DX12Texture.cpp
    ├── DX12DescriptorHeap.h/cpp       # Descriptor heap management
    ├── DX12DynamicBuffer.h/cpp        # Per-frame constant buffer ring
    ├── DX12UploadManager.h/cpp        # Texture/buffer upload to DEFAULT heap
    ├── DX12ResourceStateTracker.h/cpp # Automatic resource barriers
    ├── DX12PipelineState.h/cpp        # PSO builder and cache
    ├── DX12GenerateMipsPass.h/cpp     # Compute-based mipmap generation
    └── DX12Debug.cpp                  # Debug layer, DRED, InfoQueue
```

---

## DX12 Backend Details

### Design Decisions

| 决策点 | 选择 | 理由 |
|--------|------|------|
| Root Signature | **固定布局** | 简单、PSO 缓存效率高 |
| 描述符堆策略 | **静态分配 + Staging Ring** | Copy 到连续区域解决 descriptor table 问题 |
| 资源状态跟踪 | **隐式自动跟踪** | 上层代码无需修改 |
| 帧同步 | **Triple Buffering (3帧)** | 平衡延迟和资源占用 |
| 资源生命周期 | **Deferred Deletion Queue** | 防止 GPU 使用中的资源被删除 |

### Root Signature Layout

```
┌─────────────────────────────────────────────────────────────┐
│                    Graphics Root Signature                   │
├──────────────┬──────────────────────────────────────────────┤
│ Parameter 0  │ Root CBV (b0) - PerFrame Constants           │
│ Parameter 1  │ Root CBV (b1) - PerObject Constants          │
│ Parameter 2  │ Root CBV (b2) - Material Constants           │
│ Parameter 3  │ Root CBV (b3) - ClusteredParams              │
│ Parameter 4  │ Root CBV (b4) - CB_Probes                    │
│ Parameter 5  │ Root CBV (b5) - CB_LightProbeParams          │
│ Parameter 6  │ Root CBV (b6) - CB_VolumetricLightmap        │
│ Parameter 7  │ Descriptor Table: SRV t0-t24 (Textures)      │
│ Parameter 8  │ Descriptor Table: UAV u0-u7 (RW Resources)   │
│ Parameter 9  │ Descriptor Table: Sampler s0-s7              │
└──────────────┴──────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    Compute Root Signature                    │
├──────────────┬──────────────────────────────────────────────┤
│ Parameter 0  │ Root CBV (b0) - Compute Constants            │
│ Parameter 1  │ Descriptor Table: SRV t0-t24 (Input)         │
│ Parameter 2  │ Descriptor Table: UAV u0-u7 (Output)         │
│ Parameter 3  │ Descriptor Table: Sampler s0-s7              │
└──────────────┴──────────────────────────────────────────────┘
```

### Descriptor Heap Configuration

| Heap Type | Visibility | Size | Usage |
|-----------|------------|------|-------|
| CBV_SRV_UAV (CPU) | CPU-only | 4096 | Persistent SRV/UAV (copy source) |
| CBV_SRV_UAV (Staging) | GPU-visible | 3072 | Per-frame staging ring |
| SAMPLER (CPU) | CPU-only | 256 | Persistent samplers |
| SAMPLER (Staging) | GPU-visible | 768 | Per-frame staging ring |
| RTV | CPU-only | 128 | Render targets |
| DSV | CPU-only | 32 | Depth stencil |

### Frame Synchronization

```
┌─────────────────────────────────────────────────────────────┐
│                    3-Frame Pipeline                          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Frame N-2    Frame N-1    Frame N                          │
│  ┌──────┐    ┌──────┐    ┌──────┐                          │
│  │ GPU  │    │ GPU  │    │ CPU  │                          │
│  │ Done │    │ Exec │    │ Record│                          │
│  └──────┘    └──────┘    └──────┘                          │
│      ▲                        │                              │
│      │                        │                              │
│      └────── Fence Wait ──────┘                              │
│              (if needed)                                     │
│                                                              │
├─────────────────────────────────────────────────────────────┤
│ BeginFrame():                                                │
│   1. Wait for Frame[N-2] fence (if not completed)           │
│   2. Process deferred deletions                              │
│   3. Reset CommandAllocator[N % 3]                          │
│   4. Reset CommandList                                       │
│                                                              │
│ EndFrame():                                                  │
│   1. Close CommandList                                       │
│   2. Execute on CommandQueue                                 │
│   3. Signal fence with current value                         │
│                                                              │
│ Present():                                                   │
│   1. SwapChain->Present()                                   │
│   2. Advance frame index                                     │
└─────────────────────────────────────────────────────────────┘
```

### Deferred Resource Binding

DX12 要求 Root Signature 先绑定才能设置资源。解决方案是延迟绑定：

```cpp
// 设置资源时只记录 pending
SetConstantBuffer(slot, buffer)  → m_pendingCBVs[slot] = address
SetShaderResource(slot, texture) → m_pendingSRVs[slot] = cpuHandle
SetSampler(slot, sampler)        → m_pendingSamplers[slot] = cpuHandle

// Draw 之前统一绑定
BindPendingResources() {
    // 1. Copy 散落的 descriptor 到连续 staging 区域
    // 2. 绑定 CBV (root descriptor)
    // 3. 绑定 SRV/UAV/Sampler (descriptor table)
}
```

### Deferred Deletion Queue

防止资源在 GPU 使用时被 CPU 删除：

```cpp
// CDX12Texture 析构时
CDX12Texture::~CDX12Texture() {
    // 不立即释放，入队等待 GPU fence
    CDX12Context::Instance().DeferredRelease(m_resource.Get());
}

// MoveToNextFrame 时处理
void CDX12Context::MoveToNextFrame() {
    // ... fence wait ...
    uint64_t completedValue = m_fence->GetCompletedValue();
    m_deletionQueue.ProcessCompleted(completedValue);
}
```

---

## DX12 vs DX11 Differences

| 特性 | DX11 | DX12 |
|------|------|------|
| 资源状态 | 自动管理 | 需要显式 Barrier |
| 命令录制 | 立即执行 | 录制后 Execute |
| 帧同步 | 驱动管理 | 手动 Fence |
| Descriptor | 随用随绑 | 需要连续内存的 Table |
| Constant Buffer | 可随时更新 | 需要 Ring Buffer |
| GenerateMips | 内置函数 | 需要 Compute Shader |
| PSO | 分散状态 | 统一 PSO 对象 |

---

## Resolved Issues

### 1. SRV MipLevels = 0
**问题**: DX12 SRV 创建时 MipLevels=0 无效
**解决方案**: 使用 `UINT(-1)` 表示所有 mip

### 2. Root Signature Not Set
**问题**: SetConstantBuffer 在 SetPipelineState 之前调用
**解决方案**: 延迟绑定模式

### 3. Descriptor Table Non-Contiguous
**问题**: GPU 期望 descriptor table 连续，但 SRV 分散在 heap
**解决方案**: Staging Ring + CopyDescriptors

### 4. Resource Deleted While Still In Use
**问题**: 临时资源在 GPU 使用中被删除
**解决方案**: CDX12DeferredDeletionQueue

### 5. ComPtr Reference Count Leak
**问题**: Detach()/AddRef() 导致引用计数泄漏
**解决方案**: 使用 ComPtr.Get() 传递，让 ComPtr 自动管理

### 6. GenerateMips Not Available
**问题**: DX12 没有内置 GenerateMips
**解决方案**: CDX12GenerateMipsPass (Compute Shader)

### 7. D3D12MA Memory Leaks at Shutdown
**问题**: D3D12MA 在 Shutdown 时报告内存泄漏（PlaceholderTexture, KTX2DTexture, unnamed buffer）
**根本原因**:
- `CScene::Shutdown()` 没有调用 `m_lightmap2D.UnloadLightmap()`
- `CLightmap2DManager` 持有 `TextureHandlePtr`，其中包含对 placeholder 和 KTX2 纹理的引用
- 这些资源直到静态单例析构时才释放，而此时 `CDX12MemoryAllocator` 已经 Shutdown

**解决方案**:
1. 在 `CScene::Shutdown()` 中添加 `m_lightmap2D.UnloadLightmap()` 调用
2. 确保所有持有 GPU 资源的管理器在 RHI Shutdown 之前释放资源

**教训**:
- 静态单例的析构顺序不可控，必须显式调用 Shutdown() 释放 GPU 资源
- 使用 `TextureHandlePtr` 时要注意其生命周期，避免跨越 RHI Shutdown 边界
- D3D12MA 的 leak detection 功能可以帮助定位未释放的资源

**相关代码**:
```cpp
// Scene.cpp - 正确的 Shutdown 顺序
void CScene::Shutdown() {
    Clear();  // 先销毁 GameObjects
    m_volumetricLightmap.Shutdown();
    m_lightProbeManager.Shutdown();
    m_probeManager.Shutdown();
    m_skybox.Shutdown();
    m_lightmap2D.UnloadLightmap();  // 释放 lightmap 纹理
    m_initialized = false;
}
```

---

## Usage Examples

### Creating a Texture

```cpp
RHI::TextureDesc desc = RHI::TextureDesc::RenderTarget(1920, 1080, RHI::ETextureFormat::R16G16B16A16_FLOAT);
desc.debugName = "HDR_RenderTarget";
RHI::TexturePtr texture(ctx->CreateTexture(desc, nullptr));
```

### Setting Render Targets

```cpp
ITexture* rts[] = { hdrRT.get() };
cmdList->SetRenderTargets(rts, 1, depthBuffer.get());
cmdList->ClearRenderTarget(hdrRT.get(), clearColor);
cmdList->ClearDepthStencil(depthBuffer.get(), 1.0f, 0);
```

### Drawing

```cpp
cmdList->SetPipelineState(pso.get());
cmdList->SetVertexBuffer(0, vbo.get(), sizeof(Vertex), 0);
cmdList->SetIndexBuffer(ibo.get(), RHI::EIndexFormat::UInt32, 0);
cmdList->SetConstantBufferData(RHI::EShaderStage::Vertex, 0, &perFrame, sizeof(perFrame));
cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 0, albedoTex.get());
cmdList->SetSampler(RHI::EShaderStage::Pixel, 0, linearSampler.get());
cmdList->DrawIndexed(indexCount, 0, 0);
```

### Compute Dispatch

```cpp
cmdList->SetPipelineState(computePSO.get());
cmdList->SetConstantBufferData(RHI::EShaderStage::Compute, 0, &params, sizeof(params));
cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 0, inputBuffer.get());
cmdList->SetUnorderedAccess(0, outputBuffer.get());
cmdList->Dispatch(groupsX, groupsY, groupsZ);
```

---

## References

- [NVRHI - NVIDIA Rendering Hardware Interface](https://github.com/NVIDIA-RTX/NVRHI)
- [Diligent Engine](https://github.com/DiligentGraphics/DiligentEngine)
- [The Forge Cross-Platform Framework](https://github.com/ConfettiFX/The-Forge)
- [Microsoft DirectX-Specs](https://microsoft.github.io/DirectX-Specs/)
- [D3D12 Programming Guide](https://docs.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide)

---

*Last Updated: 2026-01-30*
