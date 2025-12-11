# RHI (Rendering Hardware Interface)

## 概述

RHI 是一个跨平台渲染抽象层，用于隔离底层图形 API（D3D11/D3D12/Vulkan）依赖。

**设计原则**：只有 `RHI/DX11/` 目录下的文件可以 `#include <d3d11.h>`，所有其他代码只能通过 RHI 抽象接口访问图形 API。

**迁移状态**: Phase 1-9 完成 (2025-12-11) - 所有核心渲染代码已 RHI 化

---

## 目录结构

```
RHI/
├── ICommandList.h        # 命令录制接口
├── IRenderContext.h      # 设备 + SwapChain 接口
├── RHICommon.h           # 枚举、常量定义
├── RHIDescriptors.h      # 资源描述结构体
├── RHIResources.h        # 资源接口 (Buffer/Texture/Shader/PSO)
├── RHIPointers.h         # 智能指针类型定义
├── RHIFactory.h/cpp      # 后端工厂函数
├── RHIManager.h/cpp      # 单例管理器
├── ShaderCompiler.h      # Shader 编译抽象
├── README.md             # 本文档
└── DX11/                 # DX11 后端实现
    ├── DX11Context.h/cpp       # D3D11 Device/SwapChain
    ├── DX11RenderContext.h/cpp # IRenderContext 实现
    ├── DX11CommandList.h/cpp   # ICommandList 实现
    ├── DX11Resources.h         # IBuffer/ITexture 等实现
    ├── DX11ShaderCompiler.cpp  # D3DCompile 封装
    └── DX11Utils.h             # 枚举转换辅助函数
```

---

## 核心接口

### IRenderContext (设备 + 资源工厂)

```cpp
class IRenderContext {
    // 生命周期
    bool Initialize(void* hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height);

    // 帧控制
    void BeginFrame();
    void EndFrame();
    void Present(bool vsync);

    // 资源创建
    IBuffer* CreateBuffer(const BufferDesc& desc, const void* initialData = nullptr);
    ITexture* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr);
    ITexture* CreateTextureWithData(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources);
    ISampler* CreateSampler(const SamplerDesc& desc);
    IShader* CreateShader(const ShaderDesc& desc);
    IPipelineState* CreatePipelineState(const PipelineStateDesc& desc);
    IPipelineState* CreateComputePipelineState(const ComputePipelineDesc& desc);

    // 纹理包装 (用于 WIC/KTX 加载器)
    ITexture* WrapNativeTexture(void* nativeTexture, void* nativeSRV, ...);
    ITexture* WrapExternalTexture(void* nativeTexture, const TextureDesc& desc);

    // 后端访问 (仅限必要时使用)
    void* GetNativeDevice();   // ID3D11Device*
    void* GetNativeContext();  // ID3D11DeviceContext*
};
```

### ICommandList (命令录制)

```cpp
class ICommandList {
    // 渲染目标
    void SetRenderTargets(uint32_t numRTs, ITexture* const* renderTargets, ITexture* depthStencil);
    void SetRenderTargetSlice(ITexture* renderTarget, uint32_t arraySlice, ITexture* depthStencil);
    void SetDepthStencilOnly(ITexture* depthStencil, uint32_t arraySlice = 0);
    void ClearRenderTarget(ITexture* renderTarget, const float color[4]);
    void ClearDepthStencil(ITexture* depthStencil, ...);
    void ClearDepthStencilSlice(ITexture* depthStencil, uint32_t arraySlice, ...);

    // 管线状态
    void SetPipelineState(IPipelineState* pso);
    void SetPrimitiveTopology(EPrimitiveTopology topology);
    void SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth);
    void SetScissorRect(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);

    // 资源绑定
    void SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset = 0);
    void SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset = 0);
    void SetConstantBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer);
    void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture);
    void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer);
    void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler);
    void SetUnorderedAccess(uint32_t slot, IBuffer* buffer);
    void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture);

    // 绘制/计算
    void Draw(uint32_t vertexCount, uint32_t startVertex = 0);
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, int32_t baseVertex = 0);
    void DrawInstanced(...);
    void DrawIndexedInstanced(...);
    void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ);

    // 拷贝/Mipmap
    void CopyTexture(ITexture* dst, ITexture* src);
    void CopyTextureToSlice(ITexture* dst, uint32_t dstArraySlice, uint32_t dstMipLevel, ITexture* src);
    void CopyTextureSubresource(...);
    void GenerateMips(ITexture* texture);

    // 解绑 (防止资源冲突)
    void UnbindRenderTargets();
    void UnbindShaderResources(EShaderStage stage, uint32_t startSlot = 0, uint32_t numSlots = 8);

    // 资源屏障 (DX12 需要, DX11 空实现)
    void Barrier(IResource* resource, EResourceState stateBefore, EResourceState stateAfter);
    void UAVBarrier(IResource* resource);

    // GPU 调试事件 (RenderDoc/PIX 标记)
    void BeginEvent(const wchar_t* name);
    void EndEvent();
};

// RAII Debug Event Wrapper
class CScopedDebugEvent {
    CScopedDebugEvent(ICommandList* cmdList, const wchar_t* name);
    ~CScopedDebugEvent();  // 自动调用 EndEvent()
};
```

### 资源接口

```cpp
// Buffer
class IBuffer : public IResource {
    void* Map();        // CPU 写入映射
    void Unmap();
    uint32_t GetSize() const;
};

// Texture
class ITexture : public IResource {
    // 查询
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    uint32_t GetDepth() const;
    uint32_t GetArraySize() const;
    uint32_t GetMipLevels() const;
    ETextureFormat GetFormat() const;

    // 视图 (返回原生指针)
    void* GetRTV();
    void* GetDSV();
    void* GetSRV();
    void* GetUAV();

    // 数组/Cubemap 切片视图
    void* GetDSVSlice(uint32_t arrayIndex);  // CSM 阴影
    void* GetRTVSlice(uint32_t arrayIndex);  // Cubemap 渲染
    void* GetSRVSlice(uint32_t arrayIndex, uint32_t mipLevel = 0);  // Debug 可视化

    // Staging 纹理 CPU 访问
    MappedTexture Map(uint32_t arraySlice = 0, uint32_t mipLevel = 0);
    void Unmap(uint32_t arraySlice = 0, uint32_t mipLevel = 0);
};

// Sampler, Shader, PipelineState...
```

### 智能指针 (RHIPointers.h)

```cpp
namespace RHI {
    using BufferPtr = std::unique_ptr<IBuffer, RHIDeleter>;
    using TexturePtr = std::unique_ptr<ITexture, RHIDeleter>;
    using SamplerPtr = std::unique_ptr<ISampler, RHIDeleter>;
    using ShaderPtr = std::unique_ptr<IShader, RHIDeleter>;
    using PipelineStatePtr = std::unique_ptr<IPipelineState, RHIDeleter>;
}
```

### Shader 编译 (ShaderCompiler.h)

```cpp
SCompiledShader CompileShaderFromFile(
    const std::string& filepath,
    const char* entryPoint,
    const char* target,  // "vs_5_0", "ps_5_0", "cs_5_0"
    IShaderIncludeHandler* includeHandler = nullptr,
    bool debug = false
);

SCompiledShader CompileShaderFromSource(
    const std::string& source,
    const char* entryPoint,
    const char* target,
    IShaderIncludeHandler* includeHandler = nullptr,
    bool debug = false
);
```

---

## 使用示例

### 基本渲染流程

```cpp
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"

// 1. 初始化 RHI
RHI::CRHIManager::Instance().Initialize(hwnd, width, height);
auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();

// 2. 创建资源
RHI::BufferDesc cbDesc(sizeof(CB_Frame), RHI::EBufferUsage::Constant, RHI::ECPUAccess::Write);
RHI::BufferPtr cbFrame(ctx->CreateBuffer(cbDesc));

RHI::TextureDesc rtDesc;
rtDesc.width = 1920; rtDesc.height = 1080;
rtDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
rtDesc.usage = RHI::ETextureUsage::RenderTarget | RHI::ETextureUsage::ShaderResource;
RHI::TexturePtr hdrRT(ctx->CreateTexture(rtDesc));

// 3. 渲染
ctx->BeginFrame();
auto* cmd = ctx->GetCommandList();

// 更新 Constant Buffer
void* data = cbFrame->Map();
memcpy(data, &frameData, sizeof(CB_Frame));
cbFrame->Unmap();

// 设置渲染目标
RHI::ITexture* rts[] = { hdrRT.get() };
cmd->SetRenderTargets(1, rts, depthBuffer);

// 绘制
cmd->SetPipelineState(pso);
cmd->SetConstantBuffer(RHI::EShaderStage::Vertex, 0, cbFrame.get());
cmd->SetShaderResource(RHI::EShaderStage::Pixel, 0, albedoTexture);
cmd->DrawIndexed(indexCount);

ctx->EndFrame();
ctx->Present(true);
```

### GPU Debug Event 使用

```cpp
#include "RHI/ICommandList.h"

void RenderShadowPass(RHI::ICommandList* cmd) {
    RHI::CScopedDebugEvent evt(cmd, L"Shadow Pass");
    // ... 渲染代码 ...
}  // 自动结束事件

// RenderDoc 中显示:
// - Shadow Pass
//   - (draw calls)
```

### Shader 编译

```cpp
#include "RHI/ShaderCompiler.h"

auto result = RHI::CompileShaderFromFile(
    "E:/forfun/source/code/Shader/MainPass.vs.hlsl",
    "main",
    "vs_5_0"
);

if (!result.success) {
    FFLog::Error("Shader compilation failed: %s", result.errorMessage.c_str());
}

RHI::ShaderDesc desc;
desc.type = RHI::EShaderType::Vertex;
desc.bytecode = result.bytecode.data();
desc.bytecodeSize = result.bytecode.size();
auto* shader = ctx->CreateShader(desc);
```

---

## 后端实现状态

| 后端 | 状态 | 说明 |
|------|------|------|
| **DX11** | ✅ 完成 | 全功能实现，Phase 1-9 迁移完成 |
| **DX12** | 📋 计划中 | 目录结构已创建，待实现 |
| **Vulkan** | 📋 未来 | 暂无计划 |

---

## D3D11 依赖规则

### 允许使用 D3D11 的文件

| 位置 | 说明 |
|------|------|
| `RHI/DX11/*` | RHI 后端实现 |
| `main.cpp` | ImGui DX11 后端集成 |
| `Core/Loader/*.cpp` | WIC/KTX 加载器内部实现 |
| `Core/Exporter/*.cpp` | 导出器内部实现 |
| `Core/Testing/Screenshot.cpp` | 截图功能内部实现 |

### 禁止使用 D3D11 的位置

- 所有 `Engine/Rendering/*.h` 文件
- 所有 `Editor/*.h` 文件
- 任何公开头文件

### 验证命令

```bash
# 检查是否有泄漏的 D3D11 依赖
grep -r "#include <d3d11" --include="*.h" | grep -v "RHI/DX11"
```

---

## RHI 扩展历史

| Phase | 新增接口 | 用途 |
|-------|---------|------|
| Phase 1 | `WrapNativeTexture()` | 包装 WIC 加载的纹理 |
| Phase 3 | `ShaderCompiler` | 抽象 D3DCompile |
| Phase 7 | `CreateTextureWithData()`, Subresource 拷贝 | Cubemap/数组纹理支持 |
| Phase 8 | `GenerateMips()`, `ETextureMiscFlags` | Mipmap 生成 |
| Phase 9 | `BeginEvent()/EndEvent()`, `CScopedDebugEvent` | GPU 调试标记 |
| Phase 9 | `GetSRVSlice()` | 按 face/mip 获取 SRV |

---

## 参考文档

- `docs/RHI_MIGRATION_ROADMAP.md` - 迁移路线图和详细记录
- `docs/RENDERING.md` - 渲染系统文档（包含 RHI 使用示例）

---

**Last Updated**: 2025-12-11
