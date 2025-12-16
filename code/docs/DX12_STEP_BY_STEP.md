# DX12 逐步迁移路线图

## 目标

将项目从 DX11 迁移到 DX12，采用**渐进式方法**：一步步将原有功能添加回来，每一步都测试并理解 DX12 的工作原理。

## 方法论

**不要一次性迁移所有功能**，而是：
1. 从最简单的 DX12 程序开始（清屏）
2. 逐步添加功能，每步验证
3. 遇到问题时，理解 DX12 的底层原理再修复
4. 保持 DX11 代码路径完整，随时可以切换对比

---

## 进度追踪

### Phase 1: 最小可运行 ✅ 完成

**目标**：DX12 能正常清屏，无报错

**完成的工作**：
- [x] DX12 Device/SwapChain/CommandQueue 创建
- [x] Command Allocator 和 Command List 管理
- [x] 基本帧循环：Reset → Record → Close → Execute → Present
- [x] Backbuffer 状态转换：PRESENT ↔ RENDER_TARGET
- [x] Fence 同步（修复了 fence value 保存顺序 bug）
- [x] Triple buffering 正确同步

**学到的 DX12 知识**：
- Command List 需要先 Reset 才能录制命令
- ExecuteCommandLists 是异步的，立即返回
- Signal 也是入队操作，GPU 按顺序执行
- Command Allocator Reset 前必须等 GPU 执行完

---

### Phase 2: ImGui 集成 ✅ 完成

**目标**：ImGui 能正常渲染

**完成的工作**：
- [x] ImGui DX12 backend 初始化（使用新版 InitInfo API）
- [x] 提供 CommandQueue 给 ImGui（用于字体纹理上传）
- [x] 设置 Descriptor Heap（ImGui 需要 SRV heap）
- [x] ImGui Panel 显示（Dockspace, Hierarchy, Inspector 等）

**学到的 DX12 知识**：
- ImGui DX12 需要 CommandQueue 来上传字体纹理
- 渲染前必须调用 SetDescriptorHeaps
- Viewport 和 ScissorRect 必须显式设置

---

### Phase 3: Scene 初始化 ✅ 完成

**目标**：开启 CScene::Initialize()，让 ReflectionProbeManager 等正常工作

**完成的工作**：
- [x] CScene::Instance().Initialize() 正常工作
- [x] ReflectionProbeManager 初始化（创建 cube arrays）
- [x] LightProbeManager 初始化
- [x] BRDF LUT 加载（KTX2）
- [x] 纹理上传到 DEFAULT heap（通过 UploadManager）

---

### Phase 4: ForwardRenderPipeline 初始化 ✅ 完成

**目标**：开启 g_pipeline.Initialize()

**完成的工作**：
- [x] ForwardRenderPipeline 初始化
- [x] ShadowPass PSO 创建（depth-only，无 RT）
- [x] SceneRenderer PSO 创建（Opaque + Transparent）
- [x] PostProcess PSO 创建
- [x] Skybox PSO 创建
- [x] ClusteredLightingPass 初始化

**学到的 DX12 知识**：
- PSO 需要显式指定 renderTargetFormats 和 depthStencilFormat
- Depth-only pass 使用空的 renderTargetFormats
- 如果没有 pixel shader 且没有指定 RT 格式，不添加默认 RT

---

### Phase 5: 场景加载 ✅ 完成

**目标**：加载 .scene 文件

**完成的工作**：
- [x] .scene 文件加载
- [x] Mesh 加载到 DEFAULT heap
- [x] Material 加载
- [x] Texture 加载（PNG/JPG/KTX2）
- [x] Skybox 环境贴图加载

**注意**：GenerateMips 在 DX12 未实现（需要 compute shader）

---

### Phase 6: 3D 渲染 ✅ 完成

**目标**：ForwardRenderPipeline.Render() 正常工作

**完成的工作**：
- [x] Viewport 显示（HDR → LDR → Backbuffer）
- [x] PostProcess Pass（Tone mapping）
- [x] Skybox 渲染
- [x] Opaque Pass 渲染（基础物体）
- [x] Shadow Pass 渲染（CSM）
- [x] IBL 绑定（Irradiance + Prefiltered + BRDF LUT）

**待完成**：
- [ ] Transparent Pass 渲染
- [ ] Clustered Lighting
- [ ] Debug Lines

**学到的 DX12 知识**：
- SRV 的 MipLevels=0 在 DX12 无效，需要用 -1 表示"所有 mip"
- SetConstantBuffer 必须在 SetPipelineState 之后调用（root signature 需要先绑定）
- 解决方案：延迟绑定，在 Draw 之前统一绑定所有 pending resources

**DX12 资源绑定架构**：
```cpp
// Root Signature Layout:
// Param 0-6: Root CBV (b0-b6) - 直接绑定 GPU virtual address
// Param 7: SRV Descriptor Table (t0-t24)
// Param 8: UAV Descriptor Table (u0-u7)
// Param 9: Sampler Descriptor Table (s0-s7)

// 延迟绑定模式：
SetConstantBuffer(slot, buffer)  → m_pendingCBVs[slot] = address
SetShaderResource(slot, texture) → m_pendingSRVs[slot] = gpuHandle
SetSampler(slot, sampler)        → m_pendingSamplers[slot] = gpuHandle

// Draw 之前统一绑定：
BindPendingResources() {
    for (slot : CBVs) SetGraphicsRootConstantBufferView(slot, address)
    SetGraphicsRootDescriptorTable(7, srvTable)
    SetGraphicsRootDescriptorTable(9, samplerTable)
}
```

---

### Phase 7: Viewport 显示 ✅ 完成

**目标**：DrawViewport 正常工作，显示 3D 场景

**完成的工作**：
- [x] Offscreen RT 作为 ImGui 纹理显示
- [x] HDR RT → LDR RT（PostProcess）
- [x] LDR RT 显示在 ImGui Viewport

---

### Phase 8: Descriptor Table 重构 ✅ 完成

**目标**：修复 SRV/Sampler Descriptor Table 绑定问题，实现正确的纹理和采样器绑定

**背景问题**：
当前代码将散落在 heap 中的 SRV 直接绑定为 descriptor table：
```cpp
// 当前错误实现：
m_pendingSRVs[slot] = texture->GetOrCreateSRV().gpuHandle;  // 散落的！
SetGraphicsRootDescriptorTable(7, m_pendingSRVs[0]);  // GPU 期望连续内存
```

GPU 期望 `basePtr + slot * descriptorSize` 是连续的，但实际每个 SRV 分配在 heap 的随机位置。

**完成的工作**：

1. **SRV/UAV 返回类型重构** ✅
   - `CDX12Texture::GetOrCreateSRV()` → 返回 `SDescriptorHandle`（含 CPU + GPU handle）
   - `CDX12Texture::GetOrCreateSRVSlice()` → 返回 `SDescriptorHandle`
   - `CDX12Texture::GetOrCreateUAV()` → 返回 `SDescriptorHandle`
   - `CDX12Buffer::GetSRV()` → 返回 `SDescriptorHandle`
   - `CDX12Buffer::GetUAV()` → 返回 `SDescriptorHandle`
   - 消除了多处冗余的 index → GPU handle 重新计算

2. **纹理槽位重组** ✅
   - MainPass.ps.hlsl 纹理从 `[0,1,6,7]` 重组为 `[0,1,2,3]`
   - 便于未来 descriptor table 连续绑定

3. **SRV Staging Ring 实现** ✅
   - CPU-only heap 存储持久 SRV（copy source）
   - GPU shader-visible staging ring 用于 per-draw 连续绑定
   - `SetShaderResource` 存储 CPU handle
   - `BindPendingResources` 时 copy 到 staging 并绑定

4. **Sampler Staging Ring 实现** ✅
   - CPU-only heap 存储持久 sampler（copy source）
   - GPU shader-visible staging ring 用于 per-draw 连续绑定
   - `SetSampler` 存储 CPU handle
   - `BindPendingResources` 时 copy 到 staging 并绑定

---

### Phase 9: 完整编辑器 🔜 待开始

**目标**：所有编辑器功能正常

**待完成**：
- [ ] Transform Gizmo
- [ ] 对象选择
- [ ] 相机控制
- [ ] IBL Baking
- [ ] Reflection Probe Baking

---

## Descriptor Table 设计文档

### 问题分析

**D3D12 Descriptor Table 工作原理**：
```
Shader 期望:              GPU Heap 实际情况:
┌──────────────┐         ┌──────────────┐
│ t0 @ offset 0│         │ sampler      │ index 0
├──────────────┤         ├──────────────┤
│ t1 @ offset 1│         │ shadowmap    │ index 1
├──────────────┤         ├──────────────┤
│ t2 @ offset 2│         │ irradiance   │ index 2
├──────────────┤         ├──────────────┤
│ t3 @ offset 3│         │ albedo tex   │ index 47  ← 不连续！
└──────────────┘         └──────────────┘

SetGraphicsRootDescriptorTable(base) → GPU 计算: base + slot * size
如果 base 是 index 0，则 t3 会读取 index 3，而不是 index 47！
```

### 业界方案对比

| 引擎 | 方案 | 特点 |
|------|------|------|
| **UE5** | FD3D12DescriptorCache + CopyDescriptors | CPU heap 暂存 → 绑定时 copy 到 GPU heap |
| **NVRHI** | 不可变 BindingSet | 创建时预分配连续 descriptor block，绑定时无需 copy |
| **Diligent** | CPUDescriptorHeap + DynamicSuballocationsManager | 5 类协作，静态/动态分开管理 |

### 选定方案：Per-Draw Descriptor Staging Ring

与 `CDX12DynamicBufferRing`（用于 CBV 数据）相同的模式：

```
┌─────────────────────────────────────────────────────────────┐
│              GPU Shader-Visible Heap                        │
├─────────────────────────────────────────────────────────────┤
│ [Persistent Descriptors]      │  [Per-Frame Staging Ring]  │
│  - Free-list 分配              │  Frame 0: 1024 slots       │
│  - 每个纹理的 SRV 在此          │  Frame 1: 1024 slots       │
│  - 无序、散落                   │  Frame 2: 1024 slots       │
└───────────────────────────────┴────────────────────────────┘
```

**绑定流程**：
```cpp
// 1. 从 staging ring 分配连续 N 个 descriptor
SDescriptorHandle stagingBase = m_stagingRing.AllocateContiguous(4);

// 2. Copy 散落的 descriptor 到连续区域
device->CopyDescriptorsSimple(1, stagingBase.cpuHandle[0], albedoSRV.cpuHandle, type);
device->CopyDescriptorsSimple(1, stagingBase.cpuHandle[1], normalSRV.cpuHandle, type);
device->CopyDescriptorsSimple(1, stagingBase.cpuHandle[2], shadowSRV.cpuHandle, type);
device->CopyDescriptorsSimple(1, stagingBase.cpuHandle[3], iblSRV.cpuHandle, type);

// 3. 绑定连续的 staging 区域
SetGraphicsRootDescriptorTable(7, stagingBase.gpuHandle);
```

**CDX12DescriptorStagingRing 实现**：
```cpp
class CDX12DescriptorStagingRing {
public:
    // 泛化支持 CBV_SRV_UAV 和 SAMPLER 两种 heap type
    bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                    uint32_t descriptorsPerFrame, uint32_t frameCount, const char* debugName);
    void BeginFrame(uint32_t frameIndex);

    // 分配 N 个连续 descriptor，返回第一个的 handle
    SDescriptorHandle AllocateContiguous(uint32_t count);

    // 获取 heap（用于 SetDescriptorHeaps）
    ID3D12DescriptorHeap* GetHeap() const;

private:
    CDX12DescriptorHeap m_heap;  // 自己的 shader-visible heap
    uint32_t m_descriptorsPerFrame;
    uint32_t m_currentFrame;
    uint32_t m_currentOffset;
};
```

**绑定流程（BindPendingResources）**：
```cpp
// SRV 绑定
if (m_srvDirty) {
    // 1. 分配连续 staging 区域
    SDescriptorHandle staging = stagingRing.AllocateContiguous(maxBoundSlot);

    // 2. Copy 散落的 SRV 到连续区域
    for (uint32_t i = 0; i < maxBoundSlot; ++i) {
        device->CopyDescriptorsSimple(1, staging.cpuHandle[i],
                                      m_pendingSRVCpuHandles[i], type);
    }

    // 3. 绑定连续区域
    SetGraphicsRootDescriptorTable(7, staging.gpuHandle);
}

// Sampler 绑定（同样模式）
if (m_samplerDirty) {
    SDescriptorHandle staging = samplerStagingRing.AllocateContiguous(maxBoundSlot);
    for (uint32_t i = 0; i < maxBoundSlot; ++i) {
        device->CopyDescriptorsSimple(1, staging.cpuHandle[i],
                                      m_pendingSamplerCpuHandles[i], type);
    }
    SetGraphicsRootDescriptorTable(9, staging.gpuHandle);
}
```

### 关键点

1. **CopyDescriptorsSimple 是 CPU 操作**
   - 源和目标都必须有有效的 CPU handle
   - 源 handle 来自 `SDescriptorHandle.cpuHandle`（创建 SRV 时保存的）
   - 目标 handle 来自 staging ring 的连续区域

2. **Per-Frame 隔离**
   - 每帧使用独立的 staging 区域
   - 无需等待 GPU，因为 Frame N 的区域在 Frame N+3 才会被重用
   - 与 `CDX12DynamicBufferRing` 相同的生命周期管理

3. **线性分配，无碎片**
   - BeginFrame 重置 offset 到帧区域起始
   - 每次 draw 前分配，永不释放单个 allocation
   - 整个帧结束后整体重用

---

## 已知问题 & 解决方案

### 1. Texture Initial Data Upload ✅ 已解决
**问题**：KTX 纹理数据需要通过 Upload Heap 上传到 Default Heap
**解决方案**：UploadManager 实现纹理上传

### 2. Buffer Initial Data Upload ✅ 已解决
**问题**：Default Heap Buffer 的初始数据上传未实现
**解决方案**：通过 Upload Buffer copy 到 Default Buffer

### 3. SRV MipLevels = 0 ✅ 已解决
**问题**：DX12 SRV 创建时 MipLevels=0 无效，导致 device removal
**解决方案**：使用 `UINT(-1)` 表示"从 MostDetailedMip 开始的所有 mip"

### 4. Root Signature Not Set ✅ 已解决
**问题**：SetConstantBuffer 在 SetPipelineState 之前调用，导致 root signature 未设置错误
**解决方案**：延迟绑定模式，在 Draw 之前通过 BindPendingResources() 统一绑定

### 5. GenerateMips ⚠️ 未实现
**问题**：DX12 没有内置的 GenerateMips，需要 compute shader
**状态**：警告但不阻塞，纹理只有 mip 0
**解决方案**：实现 compute shader 版本的 mipmap 生成

### 6. Shadow Pass ✅ 已解决
**问题**：Shadow Pass 有多个问题（constant buffer 同步、scissor rect）
**解决方案**：
- 使用 `SetConstantBufferData` 从 dynamic ring buffer 分配（解决 CB 同步）
- 添加 `SetScissorRect` 调用（DX12 强制要求）

### 7. Descriptor Table 绑定 ✅ 已解决
**问题**：SRV/Sampler descriptor table 绑定时，各 descriptor 分散在 heap 不同位置，但 GPU 期望连续内存
**解决方案**：
- `CDX12DescriptorStagingRing` - Per-frame linear allocator
- SRV 和 Sampler 各自有独立的 staging ring
- 绑定时 copy 到连续 staging 区域

### 8. Shadow Jitter (Camera Movement) ⚠️ 待解决
**问题**：DX12 下快速移动相机时阴影抖动，DX11 正常
**原因**：`CShadowPass::m_output` 是单实例，多帧共享。Frame N+1 更新 `lightSpaceVPs` 时，Frame N 的 GPU 工作可能还在读取
**解决方案**：
- 选项 1：Per-frame output buffers（推荐）
- 选项 2：在 frame 开始时 copy output 到 per-frame constant buffer
- 选项 3：直接将 shadow matrices 存入 dynamic ring buffer

---

## DX12 核心概念笔记

### Command List 生命周期
```
Reset(allocator) → 录制命令 → Close() → ExecuteCommandLists() → GPU 执行
                                                    ↓
                              等 GPU 完成后才能再次 Reset 同一个 allocator
```

### Fence 同步
```cpp
// 提交工作后
uint64_t fenceValue = Signal(fence, value);  // 入队 signal 命令
frameFenceValues[frameIndex] = fenceValue;   // 记录

// 下一帧使用同一个 allocator 前
WaitForFenceValue(frameFenceValues[frameIndex]);  // 等待 GPU 完成
allocator->Reset();  // 现在安全了
```

### Resource Barrier
```cpp
// 状态转换必须在使用资源之前完成
barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
cmdList->ResourceBarrier(1, &barrier);
// 现在可以作为 render target 使用
```

### Heap 类型
| Heap Type | 位置 | CPU 访问 | GPU 访问 | 用途 |
|-----------|------|---------|---------|------|
| DEFAULT | VRAM | ❌ | 最快 | 渲染资源 |
| UPLOAD | RAM | 可写 | 可读(慢) | 上传数据 |
| READBACK | RAM | 可读 | 可写(慢) | 回读数据 |

---

## 文件变更记录

### main.cpp
- DX12 分支使用独立的渲染循环，不依赖 RHI 抽象层
- 直接调用 DX12 API，便于理解和调试

### DX12Context.cpp
- 修复 MoveToNextFrame 中 fence value 保存顺序

### DX12RenderContext.cpp
- 添加 DRED 诊断支持
- BeginFrame 添加 backbuffer 状态转换
- Backbuffer wrapper 状态同步
- PSO 创建支持空 RT 格式（depth-only pass）

### DX12CommandList.cpp
- 实现延迟资源绑定（pending CBV/SRV/Sampler）
- BindPendingResources() 在 Draw 之前统一绑定
- Reset() 时重置 pending bindings 和 topology

### DX12Texture.cpp
- 修复 SRV MipLevels=0 问题，使用 UINT(-1) 表示所有 mip

### SceneRenderer.cpp
- DX12 模式下跳过高级功能（probes, clustered lighting, transparent）
- 保留基础 Opaque Pass 和 Skybox 渲染

### ForwardRenderPipeline.cpp
- 统一使用 SceneRenderer 渲染
- DX12 模式下禁用 Shadow Pass

### Skybox.cpp / PostProcessPass.cpp / ShadowPass.cpp
- 为 PSO 添加显式 renderTargetFormats 和 depthStencilFormat

---

*Last Updated: 2025-12-17*
