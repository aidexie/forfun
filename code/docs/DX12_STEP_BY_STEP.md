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

### Phase 6: 3D 渲染 ✅ 部分完成

**目标**：ForwardRenderPipeline.Render() 正常工作

**完成的工作**：
- [x] Viewport 显示（HDR → LDR → Backbuffer）
- [x] PostProcess Pass（Tone mapping）
- [x] Skybox 渲染
- [x] Opaque Pass 渲染（基础物体）

**待完成**：
- [ ] Shadow Pass 渲染（需要调试）
- [ ] Transparent Pass 渲染
- [ ] Probe/IBL 绑定
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

### Phase 8: 完整编辑器 ⏳ 进行中

**目标**：所有编辑器功能正常

**待完成**：
- [ ] Transform Gizmo
- [ ] 对象选择
- [ ] 相机控制
- [ ] IBL Baking
- [ ] Reflection Probe Baking

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

### 6. Shadow Pass ⚠️ 需要调试
**问题**：Shadow Pass 有多个问题，暂时禁用
**状态**：DX12 模式下跳过
**解决方案**：需要单独调试 depth-only rendering

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

*Last Updated: 2025-12-14*
