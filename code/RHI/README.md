# RHI (Rendering Hardware Interface)

## 概述

RHI 是一个跨平台渲染抽象层，支持多个图形 API 后端（DX11, DX12, 未来可扩展 Vulkan）。

## 架构

```
IRenderContext (设备 + SwapChain)
    └── ICommandList (命令录制)
         ├── Pipeline State (PSO)
         ├── Resources (Buffer, Texture, Sampler)
         └── Draw/Dispatch 命令
```

## 核心接口

### IRenderContext
- **生命周期**: Initialize, Shutdown, OnResize
- **帧控制**: BeginFrame, EndFrame, Present
- **资源创建**: CreateBuffer, CreateTexture, CreateShader, CreatePipelineState
- **查询**: GetBackend, GetWidth, GetHeight, SupportsRaytracing

### ICommandList
- **渲染目标**: SetRenderTargets, ClearRenderTarget, ClearDepthStencil
- **管线状态**: SetPipelineState, SetViewport, SetScissorRect
- **资源绑定**: SetVertexBuffer, SetIndexBuffer, SetConstantBuffer, SetShaderResource, SetSampler
- **绘制命令**: Draw, DrawIndexed, DrawInstanced, Dispatch
- **资源屏障**: Barrier, UAVBarrier (DX12 需要, DX11 空实现)

### 资源接口
- **IBuffer**: Map/Unmap (动态 CB 更新)
- **ITexture**: GetRTV/DSV/SRV/UAV
- **ISampler**: 采样器状态
- **IShader**: Vertex/Pixel/Compute/Geometry/Hull/Domain
- **IPipelineState**: Graphics/Compute PSO

## 配置系统

通过 `assets/config/render.json` 选择后端：

```json
{
    "backend": "DX11",  // 或 "DX12"
    "window": {
        "width": 1280,
        "height": 720,
        "vsync": true
    }
}
```

## 后端实现状态

| 后端 | 状态 | 说明 |
|------|------|------|
| **DX11** | ✅ 已实现 | Phase 0.3 完成 |
| **DX12** | 🚧 计划中 | Phase 3 实现 |
| **Vulkan** | 📋 未来 | Phase 4+ |

## 使用示例

```cpp
#include "RHI/IRenderContext.h"
#include "RHI/RHIFactory.h"
#include "Core/RenderConfig.h"

// 1. 加载配置
SRenderConfig config;
SRenderConfig::Load(SRenderConfig::GetDefaultPath(), config);

// 2. 创建 RHI 后端
RHI::IRenderContext* rhi = RHI::CreateRenderContext(config.backend);
rhi->Initialize(hwnd, config.windowWidth, config.windowHeight);

// 3. 创建资源
RHI::BufferDesc bufDesc(sizeof(CB_Frame), RHI::EBufferUsage::Constant, RHI::ECPUAccess::Write);
RHI::IBuffer* cbFrame = rhi->CreateBuffer(bufDesc);

// 4. 渲染
rhi->BeginFrame();
auto* cmd = rhi->GetCommandList();

// Update CB
void* data = cbFrame->Map();
memcpy(data, &frameData, sizeof(CB_Frame));
cbFrame->Unmap();

// Draw
cmd->SetConstantBuffer(RHI::EShaderStage::Vertex, 0, cbFrame);
cmd->Draw(vertexCount);

rhi->EndFrame();
rhi->Present(config.vsync);
```

## Phase 0 完成清单

- [x] Phase 0.1: RHI 核心接口设计
- [x] Phase 0.2: 配置文件系统（选择后端）
- [x] Phase 0.4: 工厂函数 + 后端选择
- [ ] Phase 0.3: DX11 后端包装实现

## 下一步

实现 DX11 后端，包装现有的 `CDX11Context` 功能到 RHI 接口。
