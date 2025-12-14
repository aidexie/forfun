# DX12 3D Rendering 渐进式开启路线图

## 当前状态
- ✅ Phase 1-5: 基础 RHI + ImGui + 场景加载完成
- ✅ 纹理上传（KTX cubemap）
- ✅ 资源状态跟踪修复
- ⏳ 3D 渲染管线

## 开启步骤

### Phase 6.1: Offscreen RT 创建 + 清屏
**目标**: 验证 RenderTarget 和 DepthStencil 创建正常

- [ ] 在 DX12 循环中调用 `ensureOffscreen()`
- [ ] 使用 RHI CommandList 清屏（不是原生 D3D12）
- [ ] 验证 HDR RT 和 Depth Buffer 创建成功

**测试点**: 无崩溃，日志显示 RT 创建成功

---

### Phase 6.2: 最简 3D 渲染 - Skybox Only
**目标**: 验证 PSO 创建 + 基本绘制调用

- [ ] 调用 `CSkybox::Render()`
- [ ] 需要: Shader 编译 (VS/PS)
- [ ] 需要: PSO 创建 (Root Signature + Input Layout)
- [ ] 需要: SetRenderTargets, SetViewport, SetScissor
- [ ] 需要: Draw call

**测试点**: 看到 skybox 渲染（彩色天空盒）

---

### Phase 6.3: Scene Renderer - Opaque Objects
**目标**: 渲染不透明物体

- [ ] 调用 `CSceneRenderer::RenderOpaqueObjects()`
- [ ] 需要: Constant Buffer 更新 (PerFrame, PerObject, Material)
- [ ] 需要: Texture 绑定 (Albedo, Normal, etc.)
- [ ] 需要: Sampler 绑定

**测试点**: 看到场景中的 mesh

---

### Phase 6.4: Shadow Pass
**目标**: 阴影贴图生成

- [ ] 调用 `CShadowPass::Render()`
- [ ] 需要: Depth-only 渲染
- [ ] 需要: Shadow map SRV 绑定到 main pass

**测试点**: 场景有阴影

---

### Phase 6.5: IBL + Reflection Probes
**目标**: 环境光照

- [ ] Irradiance map 采样
- [ ] Prefiltered specular 采样
- [ ] BRDF LUT 采样
- [ ] Reflection probe 选择

**测试点**: 金属/高光材质有正确反射

---

### Phase 6.6: Clustered Lighting
**目标**: 点光源/聚光灯

- [ ] Light buffer 创建
- [ ] Cluster assignment compute pass
- [ ] Light culling

**测试点**: 场景有动态光源照明

---

### Phase 6.7: Post-Processing + Grid + Debug Lines
**目标**: 完整渲染管线

- [ ] Tone mapping (HDR -> LDR)
- [ ] Grid 渲染
- [ ] Debug line 渲染

**测试点**: 编辑器网格可见

---

### Phase 6.8: Viewport 显示
**目标**: 将渲染结果显示到 ImGui Viewport

- [ ] Offscreen SRV 绑定到 ImGui
- [ ] 需要: ImGui_ImplDX12 纹理描述符

**测试点**: Viewport 窗口显示 3D 场景

---

## 调试技巧

1. **每步验证**: 一次只开启一个功能
2. **检查 VS 输出**: D3D12 ERROR 消息
3. **RenderDoc**: PIX 事件标记 (`CScopedDebugEvent`)
4. **资源状态**: 确保 barrier 正确

## 已知需要修复的问题

1. `[DX12RenderContext] Initial data for default heap buffer not implemented`
   - Buffer 初始数据上传（类似纹理上传）

2. ResourceStateTracker 对部分资源的跟踪
   - 手动 barrier 后状态同步
