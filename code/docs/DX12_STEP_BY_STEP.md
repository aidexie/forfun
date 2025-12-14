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

**当前状态**：
```cpp
// main.cpp DX12 循环
1. Reset command allocator/list
2. Barrier: PRESENT → RENDER_TARGET
3. OMSetRenderTargets + Clear
4. RSSetViewports + RSSetScissorRects
5. ImGui NewFrame
6. ImGui Panels (Dockspace, Hierarchy, Inspector, etc.)
7. ImGui Render + RenderDrawData
8. Barrier: RENDER_TARGET → PRESENT
9. Close + Execute
10. Present
11. MoveToNextFrame
```

---

### Phase 3: Scene 初始化 ⏳ 待开始

**目标**：开启 CScene::Initialize()，让 ReflectionProbeManager 等正常工作

**待完成**：
- [ ] 取消 CScene::Instance().Initialize() 的注释
- [ ] 修复初始化时使用 Command List 的问题（可能需要临时 BeginFrame/EndFrame）
- [ ] 测试 ReflectionProbeManager 初始化
- [ ] 测试 LightProbeManager 初始化

**潜在问题**：
- 初始化代码可能在主循环之前使用 Command List
- 纹理上传需要 Upload Heap → Default Heap 的 copy

---

### Phase 4: ForwardRenderPipeline 初始化 ⏳ 待开始

**目标**：开启 g_pipeline.Initialize()

**待完成**：
- [ ] 取消 g_pipeline.Initialize() 的注释
- [ ] ShadowPass 初始化
- [ ] SceneRenderer 初始化
- [ ] PostProcess 初始化
- [ ] DebugLinePass 初始化
- [ ] GridPass 初始化

**潜在问题**：
- PSO 创建（需要 Root Signature）
- Shader 编译
- 资源创建

---

### Phase 5: 场景加载 ⏳ 待开始

**目标**：加载 .scene 文件

**待完成**：
- [ ] 取消场景加载代码的注释
- [ ] 测试 Mesh 加载
- [ ] 测试 Material 加载
- [ ] 测试 Texture 加载（KTX2）

**潜在问题**：
- 纹理初始数据上传（已知问题：Upload Heap copy 未完全实现）
- Buffer 初始数据上传

---

### Phase 6: 3D 渲染 ⏳ 待开始

**目标**：ForwardRenderPipeline.Render() 正常工作

**待完成**：
- [ ] 开启 g_pipeline.Render() 调用
- [ ] Shadow Pass 渲染
- [ ] Scene 渲染（PBR）
- [ ] Skybox 渲染
- [ ] Post Process
- [ ] Debug Lines

**潜在问题**：
- Resource State 管理
- Root Signature 绑定
- Descriptor Table 绑定

---

### Phase 7: Viewport 显示 ⏳ 待开始

**目标**：DrawViewport 正常工作，显示 3D 场景

**待完成**：
- [ ] 开启 Panels::DrawViewport() 调用
- [ ] Offscreen RT 作为 ImGui 纹理显示
- [ ] Gizmo 交互

---

### Phase 8: 完整编辑器 ⏳ 待开始

**目标**：所有编辑器功能正常

**待完成**：
- [ ] Transform Gizmo
- [ ] 对象选择
- [ ] 相机控制
- [ ] IBL Baking
- [ ] Reflection Probe Baking

---

## 已知问题 & 解决方案

### 1. Texture Initial Data Upload
**问题**：KTX 纹理数据需要通过 Upload Heap 上传到 Default Heap
**状态**：警告但不阻塞
**解决方案**：实现 DX12UploadManager 的纹理上传

### 2. Buffer Initial Data Upload
**问题**：Default Heap Buffer 的初始数据上传未实现
**状态**：警告但不阻塞
**解决方案**：通过 Upload Buffer copy 到 Default Buffer

### 3. Resource State Tracking
**问题**：部分资源未注册到 StateTracker
**状态**：警告
**解决方案**：确保所有资源创建时注册状态

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

---

*Last Updated: 2025-12-13*
