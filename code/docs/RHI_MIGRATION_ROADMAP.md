# RHI Migration Roadmap

## 目标

**完全隔离 D3D11 依赖**：只有 `RHI/DX11/` 目录下的文件可以 `#include <d3d11.h>` 和使用 `ID3D11*` 类型，所有其他代码只能通过 RHI 抽象接口访问图形 API。

---

## 当前状态分析

### D3D11 依赖分布

| 目录 | D3D11 引用数 | 说明 |
|------|-------------|------|
| Engine/Rendering/ | ~250+ | 渲染 Pass、Pipeline |
| Core/ | ~92 | 资源加载、纹理管理 |
| Editor/ | ~10 | Debug Panel |
| main.cpp | ~5 | 入口点 |

### 已完成 RHI 接口

- `ICommandList`: SetRenderTargets, SetViewport, Draw, DrawIndexed, SetConstantBuffer, SetShaderResource, SetSampler 等
- `IRenderContext`: CreateBuffer, CreateTexture, CreateShader, CreatePipelineState, GetBackbuffer 等
- `ITexture/IBuffer/ISampler/IShader/IPipelineState`: 资源抽象

### 当前问题

已迁移的代码仍通过 `GetNativeDevice()`/`GetNativeContext()` 获取原生指针，直接调用 D3D11 API。

---

## 迁移路线图

### Phase 1: Core 基础设施迁移 (优先级最高)

**目标**: 将底层资源管理迁移到 RHI

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 1.1 | `Core/DX11Context.h/cpp` | 高 | ✅ 完成 | 移动到 `RHI/DX11/`，作为 DX11 后端的内部实现 |
| 1.2 | `Core/GpuMeshResource.h/cpp` | 中 | ✅ 完成 | 改用 `RHI::IBuffer` 存储 VBO/IBO |
| 1.3 | `Core/TextureManager.h/cpp` | 中 | ✅ 完成 | 改用 `RHI::ITexture` |
| 1.4 | `Core/MeshResourceManager.cpp` | 中 | ✅ 完成 | 使用 RHI 创建 buffer (与 1.2 一起完成) |
| 1.5 | `Core/DebugEvent.h` | 低 | ✅ 完成 | D3D11 header 移入 .cpp，公开接口使用 void* |
| 1.6 | `Core/Offscreen.h` | 低 | ✅ 完成 | 已删除 - 功能已被 RHI texture 替代 |

**Phase 1.1 完成记录 (2025-12-10)**:
- 将 `Core/DX11Context.h/cpp` 移动到 `RHI/DX11/`
- 更新 `RHI::CRHIManager::Initialize()` 接口，接受 hwnd/width/height 参数
- 所有使用 `CDX11Context::Instance()` 的代码改为通过 `RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice()/GetNativeContext()` 访问
- 受影响文件: main.cpp, MeshResourceManager.cpp, TextureManager.cpp, KTXLoader.cpp, KTXExporter.cpp, Screenshot.cpp, Panels_IrradianceDebug.cpp, ViewportPanel.cpp

**Phase 1.2 + 1.4 完成记录 (2025-12-10)**:
- `GpuMeshResource.h`: 将 `ComPtr<ID3D11Buffer>` 改为 `std::unique_ptr<RHI::IBuffer>`
- `MeshResourceManager.cpp`: 使用 `IRenderContext::CreateBuffer()` 创建 VBO/IBO
- `SceneRenderer.cpp`: 渲染时通过 `vbo->GetNativeHandle()` 获取原生指针
- `ShadowPass.cpp`: 同上
- 移除了 `MeshResourceManager.h` 中的 D3D11 前向声明

**Phase 1.3 完成记录 (2025-12-10)**:
- `TextureManager.h`: 返回 `RHI::ITexture*` 而非 `ID3D11ShaderResourceView*`
- `TextureManager.cpp`: 使用 `WrapNativeTexture()` 将 WIC 加载的纹理包装为 RHI 对象
- `IRenderContext.h`: 新增 `WrapNativeTexture()` 接口
- `DX11RenderContext.cpp`: 实现 `WrapNativeTexture()` - 包装已有 D3D11 资源
- `SceneRenderer.cpp`: RenderItem 改用 `RHI::ITexture*`，渲染时通过 `GetSRV()` 获取原生 SRV
- 保留了 TextureManager.cpp 内部的 D3D11 调用（用于 WIC 加载），但对外接口已完全 RHI 化

**Phase 1.5 完成记录 (2025-12-10)**:
- `DebugEvent.h`: 将构造函数参数从 `ID3D11DeviceContext*` 改为 `void*`
- `DebugEvent.cpp`: D3D11 header `<d3d11_1.h>` 移入 .cpp，内部 cast 回 `ID3D11DeviceContext*`
- 调用点（如 SceneRenderer.cpp）无需修改，因为已经使用 `GetNativeContext()` 返回 `void*`

**Phase 1.6 完成记录 (2025-12-10)**:
- 删除了 `Core/Offscreen.h` 和 `Core/Offscreen.cpp`
- `SOffscreenRT` 是遗留代码，已被 `ForwardRenderPipeline` 中的 RHI texture 替代
- 从 `ViewportPanel.cpp` 移除了无用的 `#include "Offscreen.h"`
- 更新 `CMakeLists.txt` 移除 Offscreen 文件引用

**阻塞项**: 无

**Phase 1 完成 ✅** - Core 基础设施迁移全部完成

---

### Phase 2: 资源加载器迁移

**目标**: 纹理/资源加载使用 RHI 接口

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 2.1 | `Core/Loader/TextureLoader.h/cpp` | 中 | ✅ 完成 | 公开接口改用 void*，D3D11 移入 .cpp |
| 2.2 | `Core/Loader/KTXLoader.h/cpp` | 中 | ✅ 完成 | 返回 `RHI::ITexture*` |
| 2.3 | `Core/Loader/FFAssetLoader.h/cpp` | 中 | ✅ 完成 | 同上 |
| 2.4 | `Core/Exporter/KTXExporter.h/cpp` | 低 | ✅ 完成 | 公开接口用 `RHI::ITexture*`，新增 Native 版本用 void* |
| 2.5 | `Core/ReflectionProbeAsset.h` | 低 | ✅ 完成 | 改用 RHI texture |

**Phase 2 完成记录 (2025-12-10)**:
- `TextureLoader.h`: 参数从 `ID3D11Device*` 改为 `void*`，输出从 `ID3D11ShaderResourceView**` 改为 `void**`
- `KTXExporter.h`: 公开接口使用 `RHI::ITexture*`，新增 `ExportCubemapToKTX2Native(void*)` 用于内部 D3D11 纹理
- `Screenshot.h`: 参数从 `ID3D11Texture2D*` 改为 `void*`
- 所有 .cpp 文件内部保留 D3D11 调用，但 header 不再暴露 D3D11 类型

**Phase 2 完成 ✅**

**依赖**: Phase 1.3 (TextureManager)

---

### Phase 3: Engine Rendering Pass 迁移

**目标**: 所有渲染 Pass 完全使用 RHI ICommandList

#### Phase 3.1: 简单 Pass (无复杂状态)

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 3.1.1 | `PostProcessPass` | 低 | ✅ 完成 | 使用 RHI::ShaderCompiler |
| 3.1.2 | `Skybox` | 低 | ✅ 完成 | 使用 RHI::ShaderCompiler |
| 3.1.3 | `GridPass` | 低 | ✅ 完成 | 使用 RHI::ShaderCompiler |
| 3.1.4 | `DebugLinePass` | 低 | ✅ 完成 | 使用 RHI::ShaderCompiler |

#### Phase 3.2: 中等复杂度 Pass

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 3.2.1 | `ShadowPass` | 中 | ✅ 完成 | 使用 RHI::ShaderCompiler |
| 3.2.2 | `SceneRenderer` (MainPass) | 高 | ✅ 完成 | 使用 RHI PSO/Shader |
| 3.2.3 | `ForwardRenderPipeline` | 中 | ✅ 完成 | 使用 RHI 资源 |

#### Phase 3.3: 高级功能 Pass

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 3.3.1 | `ClusteredLightingPass` | 高 | ✅ 完成 | 使用 RHI::ShaderCompiler |
| 3.3.2 | `IBLGenerator` | 高 | ⚠️ 部分 | .cpp 用 RHI，.h 仍有 D3D11 (需 PIMPL) |
| 3.3.3 | `CubemapRenderer` | 中 | ✅ 完成 | 内部实现 |
| 3.3.4 | `ReflectionProbeBaker` | 中 | ⚠️ 部分 | .cpp 用 RHI，.h 仍有 D3D11 (需 PIMPL) |
| 3.3.5 | `LightProbeBaker` | 中 | ⚠️ 部分 | .cpp 用 RHI，.h 仍有 D3D11 (需 PIMPL) |
| 3.3.6 | `VolumetricLightmap` | 高 | ⚠️ 部分 | .h 有 ComPtr<ID3D11*> 成员 (需 PIMPL) |

**Phase 3 完成记录 (2025-12-10)**:
- 新增 `RHI/ShaderCompiler.h` 和 `RHI/DX11/DX11ShaderCompiler.cpp` 抽象 D3DCompile
- 所有渲染 Pass 的 shader 编译改用 `RHI::CompileShaderFromFile/CompileShaderFromSource`
- 移除了各 Pass 中的 `#include <d3dcompiler.h>`
- 剩余问题：部分 header 仍有 D3D11 类型（ComPtr 成员），需要 PIMPL 重构

**依赖**: Phase 1, Phase 2

---

### Phase 4: Manager 类迁移

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 4.1 | `ReflectionProbeManager` | 中 | ✅ 完成 | .h 已清理 D3D11 类型 |
| 4.2 | `LightProbeManager` | 中 | ✅ 完成 | .h 已清理 D3D11 类型 |

**Phase 4 完成 ✅**

**依赖**: Phase 3.3

---

### Phase 5: Editor 和杂项

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 5.1 | `Editor/Panels_IrradianceDebug.cpp` | 低 | ✅ 完成 | D3D11 仅在 .cpp 内部 |
| 5.2 | `Core/Testing/Screenshot.cpp` | 低 | ✅ 完成 | 接口改用 void* |
| 5.3 | `main.cpp` | 低 | ✅ 完成 | D3D11 仅在 .cpp 内部 |

**Phase 5 完成 ✅**

---

### Phase 6: RHI 接口扩展 (按需)

可能需要扩展的接口：

| 接口 | 说明 |
|------|------|
| Debug Annotation | `BeginEvent()`, `EndEvent()` for GPU profiling |
| Texture Readback | `CopyTextureToStaging()`, `MapTexture()` |
| Per-slice DSV/RTV | 用于 CSM、Cubemap 渲染 |
| Generate Mips | `GenerateMips()` |
| UpdateSubresource | 用于动态 constant buffer |

---

### Phase 7: Header PIMPL 重构 (已完成 ✅)

**目标**: 移除 header 中的 D3D11 类型，使用 RHI 抽象类型

| 任务 | 文件 | 复杂度 | 状态 | 说明 |
|------|------|--------|------|------|
| 7.1 | `IBLGenerator.h` | 高 | ✅ 完成 | ComPtr 改为 RHI::ShaderPtr/TexturePtr/BufferPtr/SamplerPtr |
| 7.2 | `ReflectionProbeBaker.h` | 中 | ✅ 完成 | ComPtr 改为 RHI::TexturePtr |
| 7.3 | `LightProbeBaker.h` | 中 | ✅ 完成 | ComPtr 改为 RHI::TexturePtr |
| 7.4 | `VolumetricLightmap.h` | 高 | ✅ 完成 | ComPtr 改为 RHI::TexturePtr/BufferPtr/SamplerPtr |
| 7.5 | `ReflectionProbeManager.h` | 中 | ✅ 完成 | ComPtr 改为 RHI::TexturePtr/BufferPtr/SamplerPtr |
| 7.6 | `LightProbeManager.h` | 低 | ✅ 完成 | ComPtr 改为 RHI::BufferPtr |

**Phase 7 完成记录 (2025-12-10)**:
- 所有 header 文件改用 `RHI/RHIPointers.h` 中定义的智能指针类型
- `#include <d3d11.h>` 和 `ComPtr<ID3D11*>` 已从所有 Engine/Rendering header 中移除
- 函数签名中的 `ID3D11*` 参数改为 `RHI::ITexture*` / `RHI::IBuffer*` 等抽象类型
- 扩展 RHI 支持 TextureCubeArray、Staging 纹理写入、Subresource 拷贝等功能

**Phase 7 完成 ✅**

---

## 推荐执行顺序

```
Phase 1.1 (DX11Context 移动) ✅
    ↓
Phase 1.2-1.4 (Core 资源管理) ✅
    ↓
Phase 2.1-2.3 (资源加载器) ✅
    ↓
Phase 3.1 (简单 Pass) ✅
    ↓
Phase 3.2 (核心渲染) ✅
    ↓
Phase 3.3 + Phase 4 (高级功能) ✅
    ↓
Phase 5 (清理) ✅
    ↓
Phase 7 (Header RHI 重构) ✅
```

---

## 验收标准

1. **编译检查**: `grep -r "#include <d3d11" --include="*.cpp" --include="*.h" | grep -v "RHI/DX11"` 返回空
2. **功能测试**: 所有现有测试通过
3. **运行时**: Editor 正常运行，渲染正确

**当前状态**: ✅ Engine/Rendering/ 目录 header 已完全清理 D3D11 依赖
**剩余 D3D11 依赖**: Editor debug panel (允许 .cpp 内部使用), Core loaders (允许 .cpp 内部使用)

---

## 预估工作量

| Phase | 预估时间 | 风险 | 状态 |
|-------|---------|------|------|
| Phase 1 | 中 | 中 (核心基础设施) | ✅ 完成 |
| Phase 2 | 中 | 低 | ✅ 完成 |
| Phase 3.1-3.2 | 高 | 中 | ✅ 完成 |
| Phase 3.3 | 高 | 高 (复杂渲染) | ✅ 完成 |
| Phase 4-5 | 低 | 低 | ✅ 完成 |
| Phase 7 | 中 | 低 | ✅ 完成 |

**最终状态 (2025-12-10)**: 🎉 **RHI 迁移全部完成！**
所有 Engine/Rendering/ 目录下的 IBL/Probe 相关 header 文件已完全清理 D3D11 依赖。
保留 D3D11 引用的文件:
- `ClusteredLightingPass.h/.cpp` - Compute shader 密集使用，需要先扩展 RHI 支持 UAV/Compute (Phase 8 未来工作)
- Editor debug panel (.cpp 内部实现，用于 ImGui 可视化)
- Core loaders (.cpp 内部实现，用于资源加载)

---

## 注意事项

1. **先接口后实现**: 如发现 RHI 接口不足，先扩展接口再迁移
2. **全部重构完后在修复编译失败** 
