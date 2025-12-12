# DX12 Backend Implementation Roadmap

## Overview

本文档描述了为 RHI 抽象层添加 DX12 后端的实现计划。目标是在不修改上层渲染代码的前提下，实现完整的 DX12 支持。

**开始日期**: 2025-12-12
**预估工期**: 20-29 个工作日

---

## Goals

### Primary Goals
1. **完整的 DX12 后端实现** - 实现 `IRenderContext` 和 `ICommandList` 的所有接口
2. **上层代码零修改** - 现有的渲染 Pass（MainPass、ShadowPass、ClusteredLighting 等）无需改动
3. **运行时后端切换** - 支持启动时选择 DX11 或 DX12
4. **性能可接受** - 不引入明显的 CPU 开销

### Non-Goals (Current Phase)
- 多线程命令录制
- 异步 Compute Queue
- 光线追踪支持
- Mesh Shader 支持

---

## Design Decisions

| 决策点 | 选择 | 理由 |
|--------|------|------|
| Root Signature | **固定布局** | 简单、PSO 缓存效率高、够用 |
| 描述符堆策略 | **静态分配 + Free List** | 简单、高效、无需每帧重建 |
| 资源状态跟踪 | **隐式自动跟踪** | 上层代码无需修改 |
| 多线程支持 | **不支持** | 当前不需要，架构预留扩展 |
| Debug Layer | **Debug 构建启用** | 开发阶段必须 |
| N-Buffering | **3 帧** | 平衡延迟和资源占用 |

---

## Architecture Overview

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

## Root Signature Layout

固定的 Root Signature 布局，覆盖所有渲染场景：

```
┌─────────────────────────────────────────────────────────────┐
│                    Graphics Root Signature                   │
├──────────────┬──────────────────────────────────────────────┤
│ Parameter 0  │ Root CBV (b0) - PerFrame Constants           │
│ Parameter 1  │ Root CBV (b1) - PerObject Constants          │
│ Parameter 2  │ Root CBV (b2) - Material Constants           │
│ Parameter 3  │ Descriptor Table: SRV t0-t15 (Textures)      │
│ Parameter 4  │ Descriptor Table: UAV u0-u7 (RW Resources)   │
│ Parameter 5  │ Descriptor Table: Sampler s0-s7              │
└──────────────┴──────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    Compute Root Signature                    │
├──────────────┬──────────────────────────────────────────────┤
│ Parameter 0  │ Root CBV (b0) - Compute Constants            │
│ Parameter 1  │ Descriptor Table: SRV t0-t15 (Input)         │
│ Parameter 2  │ Descriptor Table: UAV u0-u7 (Output)         │
│ Parameter 3  │ Descriptor Table: Sampler s0-s7              │
└──────────────┴──────────────────────────────────────────────┘
```

---

## Descriptor Heap Configuration

| Heap Type | Visibility | Size | Usage |
|-----------|------------|------|-------|
| CBV_SRV_UAV | GPU-visible | 4096 | Textures, Buffers, UAVs |
| SAMPLER | GPU-visible | 256 | Sampler states |
| RTV | CPU-only | 128 | Render targets |
| DSV | CPU-only | 32 | Depth stencil |

---

## Frame Synchronization Model

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
│   2. Reset CommandAllocator[N % 3]                          │
│   3. Reset CommandList                                       │
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

---

## Implementation Phases

### Phase 0: Preparation (1-2 days)
**Goal**: 创建 DX12 后端的基础目录结构，不影响现有代码

**Tasks**:
- [ ] 创建 `RHI/DX12/` 目录
- [ ] 添加 DX12 头文件和库依赖到 CMakeLists.txt
- [ ] 创建 `DX12Common.h`（DX12 特有的类型、宏、辅助函数）
- [ ] 在 `RHIFactory.cpp` 中添加 `EBackend::DX12` 分支（返回 nullptr 占位）
- [ ] 验证 DX11 后端仍然正常工作

**Files to Create**:
```
RHI/DX12/
├── DX12Common.h        - ComPtr, HRESULTToString, debug macros
└── (placeholder)
```

**Commit Message**: `RHI: Phase 0 - Add DX12 backend directory structure`

---

### Phase 1: Core Infrastructure (3-4 days)
**Goal**: 实现 DX12 设备初始化、交换链、帧同步

**Tasks**:
- [ ] 实现 `CDX12Context` 单例
  - [ ] 创建 ID3D12Device
  - [ ] 创建 ID3D12CommandQueue (Direct)
  - [ ] 创建 IDXGISwapChain3
  - [ ] 创建 ID3D12Fence 和同步事件
  - [ ] 创建 N 个 ID3D12CommandAllocator
- [ ] 实现帧同步
  - [ ] `WaitForGPU()` - 等待所有 GPU 工作完成
  - [ ] `MoveToNextFrame()` - 推进帧索引
  - [ ] Fence 信号和等待逻辑
- [ ] 实现 `OnResize()`
  - [ ] 等待 GPU 空闲
  - [ ] 释放旧的 backbuffer
  - [ ] ResizeBuffers
  - [ ] 重新获取 backbuffer
- [ ] 启用 Debug Layer（Debug 构建）

**Files to Create**:
```
RHI/DX12/
├── DX12Common.h
├── DX12Context.h
└── DX12Context.cpp
```

**Commit Message**: `RHI: Phase 1 - Implement DX12 device, swapchain and frame sync`

---

### Phase 2: Descriptor Heap Management (2-3 days)
**Goal**: 实现描述符堆的创建和分配管理

**Tasks**:
- [ ] 实现 `CDX12DescriptorHeap` 基础类
  - [ ] 创建指定类型和大小的堆
  - [ ] 计算 descriptor 大小（GetDescriptorHandleIncrementSize）
  - [ ] CPU/GPU handle 计算
- [ ] 实现 Free List 分配器
  - [ ] `Allocate()` - 分配一个 descriptor
  - [ ] `Free()` - 释放一个 descriptor（加入 free list）
  - [ ] 处理堆满的情况（错误日志）
- [ ] 实现 `CDX12DescriptorHeapManager`
  - [ ] 管理 4 种类型的堆（CBV_SRV_UAV, SAMPLER, RTV, DSV）
  - [ ] 提供全局访问点

**Files to Create**:
```
RHI/DX12/
├── DX12DescriptorHeap.h
├── DX12DescriptorHeap.cpp
└── (update DX12Context to use descriptor heaps)
```

**Commit Message**: `RHI: Phase 2 - Implement DX12 descriptor heap management`

---

### Phase 3: Resource Creation (3-4 days)
**Goal**: 实现 Buffer 和 Texture 的创建

**Tasks**:
- [ ] 实现 `CDX12Buffer`
  - [ ] 创建 ID3D12Resource（Committed Resource）
  - [ ] 支持 Constant/Vertex/Index/Structured buffer
  - [ ] 实现 Map/Unmap（对于 Upload 堆资源）
  - [ ] 创建 CBV/SRV/UAV 描述符（按需）
  - [ ] 跟踪资源状态
- [ ] 实现 `CDX12Texture`
  - [ ] 创建 2D/3D/Cube/Array 纹理
  - [ ] 创建 SRV/RTV/DSV/UAV 描述符（按需）
  - [ ] 支持 mip/slice 特定的 view
  - [ ] 跟踪资源状态
- [ ] 实现 `CDX12UploadManager`
  - [ ] 管理临时上传缓冲区
  - [ ] 支持纹理和缓冲区的初始数据上传
  - [ ] 基于 Fence 的延迟释放
- [ ] 实现 `CDX12Sampler`
  - [ ] 创建采样器描述符

**Files to Create**:
```
RHI/DX12/
├── DX12Resources.h      - CDX12Buffer, CDX12Texture, CDX12Sampler declarations
├── DX12Buffer.cpp       - Buffer implementation
├── DX12Texture.cpp      - Texture implementation
└── DX12UploadManager.h/cpp - Upload buffer management
```

**Commit Message**: `RHI: Phase 3 - Implement DX12 buffer and texture resources`

---

### Phase 4: Resource State Tracking (3-4 days)
**Goal**: 实现自动资源状态跟踪和 Barrier 插入

**Tasks**:
- [ ] 实现 `CDX12ResourceStateTracker`
  - [ ] 维护每个资源的当前状态
  - [ ] `TransitionResource()` - 记录状态转换需求
  - [ ] `FlushBarriers()` - 批量提交所有待执行的 barrier
  - [ ] 处理 subresource 级别的状态
- [ ] 实现 UAV Barrier
  - [ ] `UAVBarrier()` - 同一资源连续 UAV 读写时需要
- [ ] 集成到资源类
  - [ ] CDX12Buffer 和 CDX12Texture 存储当前状态
  - [ ] 绑定操作时自动请求状态转换

**Files to Create**:
```
RHI/DX12/
├── DX12ResourceStateTracker.h
└── DX12ResourceStateTracker.cpp
```

**Commit Message**: `RHI: Phase 4 - Implement DX12 automatic resource state tracking`

---

### Phase 5: Command List and Render Context (3-4 days)
**Goal**: 实现完整的命令录制和执行流程

**Tasks**:
- [ ] 实现 `CDX12CommandList`
  - [ ] Render Target Operations
    - [ ] SetRenderTargets
    - [ ] SetRenderTargetSlice
    - [ ] SetDepthStencilOnly
    - [ ] ClearRenderTarget
    - [ ] ClearDepthStencil
    - [ ] ClearDepthStencilSlice
  - [ ] Pipeline State
    - [ ] SetPipelineState
    - [ ] SetPrimitiveTopology
    - [ ] SetViewport
    - [ ] SetScissorRect
  - [ ] Resource Binding
    - [ ] SetVertexBuffer
    - [ ] SetIndexBuffer
    - [ ] SetConstantBuffer
    - [ ] SetShaderResource
    - [ ] SetShaderResourceBuffer
    - [ ] SetSampler
    - [ ] SetUnorderedAccess
    - [ ] SetUnorderedAccessTexture
    - [ ] ClearUnorderedAccessViewUint
  - [ ] Draw Commands
    - [ ] Draw
    - [ ] DrawIndexed
    - [ ] DrawInstanced
    - [ ] DrawIndexedInstanced
  - [ ] Compute
    - [ ] Dispatch
  - [ ] Copy Operations
    - [ ] CopyTexture
    - [ ] CopyTextureToSlice
    - [ ] CopyTextureSubresource
  - [ ] Other
    - [ ] GenerateMips
    - [ ] UnbindRenderTargets
    - [ ] UnbindShaderResources
    - [ ] BeginEvent / EndEvent
- [ ] 实现 `CDX12RenderContext`
  - [ ] Initialize / Shutdown
  - [ ] BeginFrame / EndFrame / Present
  - [ ] GetCommandList
  - [ ] CreateBuffer / CreateTexture / CreateSampler / CreateShader / CreatePipelineState
  - [ ] GetBackbuffer / GetDepthStencil
  - [ ] OnResize
  - [ ] GetNativeDevice / GetNativeContext

**Files to Create**:
```
RHI/DX12/
├── DX12CommandList.h
├── DX12CommandList.cpp
├── DX12RenderContext.h
└── DX12RenderContext.cpp
```

**Commit Message**: `RHI: Phase 5 - Implement DX12 command list and render context`

---

### Phase 6: Pipeline State and Root Signature (2-3 days)
**Goal**: 实现 PSO 创建和 Root Signature 管理

**Tasks**:
- [ ] 实现 `CDX12RootSignatureManager`
  - [ ] 创建默认 Graphics Root Signature
  - [ ] 创建默认 Compute Root Signature
  - [ ] 缓存 Root Signature
- [ ] 实现 `CDX12PipelineState`
  - [ ] 存储 ID3D12PipelineState
  - [ ] 关联 Root Signature
- [ ] 实现 `CDX12PSOCache`
  - [ ] Graphics PSO 缓存
  - [ ] Compute PSO 缓存
  - [ ] 基于 PipelineStateDesc 哈希
- [ ] 更新 RenderContext
  - [ ] CreatePipelineState 使用缓存
  - [ ] CreateComputePipelineState 使用缓存

**Files to Create**:
```
RHI/DX12/
├── DX12RootSignature.h
├── DX12RootSignature.cpp
├── DX12PipelineState.h
└── DX12PipelineState.cpp
```

**Commit Message**: `RHI: Phase 6 - Implement DX12 PSO and root signature management`

---

### Phase 7: Shader Compilation (1-2 days)
**Goal**: 实现 DX12 shader 编译

**Tasks**:
- [ ] 集成 DXC 编译器（或使用 D3DCompile with SM 5.1）
- [ ] 实现 `CDX12Shader`
  - [ ] 存储编译后的字节码
  - [ ] 支持 VS/PS/CS/GS/HS/DS
- [ ] 更新 `CompileShaderFromFile`
  - [ ] 支持 DX12 shader model (5.1 或 6.0)
- [ ] 验证 shader 与 Root Signature 兼容性

**Files to Create**:
```
RHI/DX12/
├── DX12Shader.h
└── DX12ShaderCompiler.cpp
```

**Commit Message**: `RHI: Phase 7 - Implement DX12 shader compilation`

---

### Phase 8: Integration and Testing (2-3 days)
**Goal**: 集成测试，确保 DX12 后端正确工作

**Tasks**:
- [ ] 更新 `RHIFactory` 返回 DX12 实现
- [ ] 更新 `main.cpp` 支持命令行选择后端
- [ ] 基础渲染测试
  - [ ] 清屏颜色
  - [ ] 简单三角形
  - [ ] 纹理采样
- [ ] 完整功能测试
  - [ ] 运行 TestSimplePointLight
  - [ ] 运行 TestClusteredLighting
  - [ ] 运行 TestReflectionProbe
  - [ ] 运行 TestSpotLight
- [ ] 性能验证
  - [ ] 对比 DX11 帧率
  - [ ] 检查内存泄漏
- [ ] 稳定性测试
  - [ ] 长时间运行
  - [ ] 窗口 resize
  - [ ] 反复切换场景

**Commit Message**: `RHI: Phase 8 - DX12 backend integration and testing complete`

---

## File Structure (Final)

```
RHI/
├── DX11/                          (existing)
│   ├── DX11Buffer.cpp
│   ├── DX11CommandList.cpp
│   ├── DX11CommandList.h
│   ├── DX11Context.cpp
│   ├── DX11Context.h
│   ├── DX11RenderContext.cpp
│   ├── DX11RenderContext.h
│   ├── DX12Resources.h
│   ├── DX11ShaderCompiler.cpp
│   ├── DX11Texture.cpp
│   └── DX11Utils.h
│
├── DX12/                          (new)
│   ├── DX12Common.h               - Phase 0
│   ├── DX12Context.h              - Phase 1
│   ├── DX12Context.cpp            - Phase 1
│   ├── DX12DescriptorHeap.h       - Phase 2
│   ├── DX12DescriptorHeap.cpp     - Phase 2
│   ├── DX12Resources.h            - Phase 3
│   ├── DX12Buffer.cpp             - Phase 3
│   ├── DX12Texture.cpp            - Phase 3
│   ├── DX12UploadManager.h        - Phase 3
│   ├── DX12UploadManager.cpp      - Phase 3
│   ├── DX12ResourceStateTracker.h - Phase 4
│   ├── DX12ResourceStateTracker.cpp - Phase 4
│   ├── DX12CommandList.h          - Phase 5
│   ├── DX12CommandList.cpp        - Phase 5
│   ├── DX12RenderContext.h        - Phase 5
│   ├── DX12RenderContext.cpp      - Phase 5
│   ├── DX12RootSignature.h        - Phase 6
│   ├── DX12RootSignature.cpp      - Phase 6
│   ├── DX12PipelineState.h        - Phase 6
│   ├── DX12PipelineState.cpp      - Phase 6
│   ├── DX12Shader.h               - Phase 7
│   └── DX12ShaderCompiler.cpp     - Phase 7
│
├── ICommandList.h                 (existing, no changes)
├── IRenderContext.h               (existing, no changes)
├── RHICommon.h                    (existing, no changes)
├── RHIDescriptors.h               (existing, no changes)
├── RHIFactory.cpp                 (update for DX12)
├── RHIFactory.h                   (existing)
├── RHIHelpers.cpp                 (existing)
├── RHIHelpers.h                   (existing)
├── RHIManager.cpp                 (existing)
├── RHIManager.h                   (existing)
├── RHIPointers.h                  (existing)
├── RHIResources.h                 (existing, no changes)
└── ShaderCompiler.h               (existing)
```

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| 资源状态跟踪遗漏 | Medium | High | Debug Layer 会报错，逐步测试 |
| 描述符堆溢出 | Low | Medium | 监控分配，日志警告 |
| PSO 创建失败 | Medium | High | 详细错误日志，shader 验证 |
| 帧同步死锁 | Low | High | 简单的 N-buffering 模型 |
| 性能低于预期 | Medium | Medium | Profile 和优化 |

---

## Success Criteria

1. **功能完整**: 所有现有测试用例通过
2. **稳定运行**: 连续运行 10 分钟无崩溃
3. **无内存泄漏**: Debug 构建无泄漏警告
4. **性能可接受**: 帧率不低于 DX11 的 80%
5. **代码质量**: 无 Debug Layer 错误/警告

---

## References

- [NVRHI - NVIDIA Rendering Hardware Interface](https://github.com/NVIDIA-RTX/NVRHI)
- [Diligent Engine](https://github.com/DiligentGraphics/DiligentEngine)
- [The Forge Cross-Platform Framework](https://github.com/ConfettiFX/The-Forge)
- [Microsoft DirectX-Specs](https://microsoft.github.io/DirectX-Specs/)
- [D3D12 Programming Guide](https://docs.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide)

---

## Changelog

| Date | Phase | Description |
|------|-------|-------------|
| 2025-12-12 | - | Roadmap document created |
| | | |
