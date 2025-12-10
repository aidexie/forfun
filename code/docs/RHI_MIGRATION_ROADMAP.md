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
| 1.2 | `Core/GpuMeshResource.h/cpp` | 中 | 待开始 | 改用 `RHI::IBuffer` 存储 VBO/IBO |
| 1.3 | `Core/TextureManager.h/cpp` | 中 | 待开始 | 改用 `RHI::ITexture` |
| 1.4 | `Core/MeshResourceManager.cpp` | 中 | 待开始 | 使用 RHI 创建 buffer |
| 1.5 | `Core/DebugEvent.h` | 低 | 待开始 | 需要 RHI 层封装 debug annotation |
| 1.6 | `Core/Offscreen.h` | 低 | 待开始 | 可能废弃，功能已被 RHI texture 替代 |

**Phase 1.1 完成记录 (2025-12-10)**:
- 将 `Core/DX11Context.h/cpp` 移动到 `RHI/DX11/`
- 更新 `RHI::CRHIManager::Initialize()` 接口，接受 hwnd/width/height 参数
- 所有使用 `CDX11Context::Instance()` 的代码改为通过 `RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice()/GetNativeContext()` 访问
- 受影响文件: main.cpp, MeshResourceManager.cpp, TextureManager.cpp, KTXLoader.cpp, KTXExporter.cpp, Screenshot.cpp, Panels_IrradianceDebug.cpp, ViewportPanel.cpp

**阻塞项**: 无

---

### Phase 2: 资源加载器迁移

**目标**: 纹理/资源加载使用 RHI 接口

| 任务 | 文件 | 复杂度 | 说明 |
|------|------|--------|------|
| 2.1 | `Core/Loader/TextureLoader.h/cpp` | 中 | 返回 `RHI::ITexture*` 而非 `ID3D11ShaderResourceView*` |
| 2.2 | `Core/Loader/KTXLoader.h/cpp` | 中 | 同上 |
| 2.3 | `Core/Loader/FFAssetLoader.h/cpp` | 中 | 同上 |
| 2.4 | `Core/Exporter/KTXExporter.h/cpp` | 低 | 从 `RHI::ITexture` 读取数据 |
| 2.5 | `Core/ReflectionProbeAsset.h` | 低 | 改用 RHI texture |

**依赖**: Phase 1.3 (TextureManager)

---

### Phase 3: Engine Rendering Pass 迁移

**目标**: 所有渲染 Pass 完全使用 RHI ICommandList

#### Phase 3.1: 简单 Pass (无复杂状态)

| 任务 | 文件 | 复杂度 | 说明 |
|------|------|--------|------|
| 3.1.1 | `PostProcessPass` | 低 | 已部分迁移，需移除 D3D11 调用 |
| 3.1.2 | `Skybox` | 低 | 简单 cubemap 渲染 |
| 3.1.3 | `GridPass` | ✅ 已完成 | 已使用 RHI |
| 3.1.4 | `DebugLinePass` | ✅ 已完成 | 已使用 RHI |

#### Phase 3.2: 中等复杂度 Pass

| 任务 | 文件 | 复杂度 | 说明 |
|------|------|--------|------|
| 3.2.1 | `ShadowPass` | 中 | 已部分迁移，需移除 D3D11 调用 |
| 3.2.2 | `SceneRenderer` (MainPass) | 高 | 核心渲染，大量状态设置 |
| 3.2.3 | `ForwardRenderPipeline` | 中 | 组合各 Pass |

#### Phase 3.3: 高级功能 Pass

| 任务 | 文件 | 复杂度 | 说明 |
|------|------|--------|------|
| 3.3.1 | `ClusteredLightingPass` | 高 | Compute shader, structured buffer |
| 3.3.2 | `IBLGenerator` | 高 | Cubemap 渲染, mip generation |
| 3.3.3 | `CubemapRenderer` | 中 | 6 面渲染 |
| 3.3.4 | `ReflectionProbeBaker` | 中 | 依赖 CubemapRenderer |
| 3.3.5 | `LightProbeBaker` | 中 | SH 计算 |
| 3.3.6 | `VolumetricLightmap` | 高 | 3D texture, compute |

**依赖**: Phase 1, Phase 2

---

### Phase 4: Manager 类迁移

| 任务 | 文件 | 复杂度 | 说明 |
|------|------|--------|------|
| 4.1 | `ReflectionProbeManager` | 中 | TextureCubeArray 管理 |
| 4.2 | `LightProbeManager` | 中 | Structured buffer 管理 |

**依赖**: Phase 3.3

---

### Phase 5: Editor 和杂项

| 任务 | 文件 | 复杂度 | 说明 |
|------|------|--------|------|
| 5.1 | `Editor/Panels_IrradianceDebug.cpp` | 低 | ImGui texture ID 转换 |
| 5.2 | `Core/Testing/Screenshot.cpp` | 低 | 从 RHI texture 读取像素 |
| 5.3 | `main.cpp` | 低 | 移除直接 D3D11 引用 |

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

## 推荐执行顺序

```
Phase 1.1 (DX11Context 移动)
    ↓
Phase 1.2-1.4 (Core 资源管理)
    ↓
Phase 2.1-2.3 (资源加载器)
    ↓
Phase 3.1 (简单 Pass)
    ↓
Phase 3.2 (核心渲染)
    ↓
Phase 3.3 + Phase 4 (高级功能)
    ↓
Phase 5 (清理)
```

---

## 验收标准

1. **编译检查**: `grep -r "#include <d3d11" --include="*.cpp" --include="*.h" | grep -v "RHI/DX11"` 返回空
2. **功能测试**: 所有现有测试通过
3. **运行时**: Editor 正常运行，渲染正确

---

## 预估工作量

| Phase | 预估时间 | 风险 |
|-------|---------|------|
| Phase 1 | 中 | 中 (核心基础设施) |
| Phase 2 | 中 | 低 |
| Phase 3.1-3.2 | 高 | 中 |
| Phase 3.3 | 高 | 高 (复杂渲染) |
| Phase 4-5 | 低 | 低 |

**总计**: 大型重构，建议分批次提交，每个 Phase 完成后验证

---

## 注意事项

1. **保持向后兼容**: 迁移期间保证功能可用
2. **增量提交**: 每个小任务完成后提交，便于回滚
3. **测试驱动**: 每次迁移后运行测试验证
4. **先接口后实现**: 如发现 RHI 接口不足，先扩展接口再迁移
