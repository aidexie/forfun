# Development Roadmap

**核心目标**:
1. 构建完善的自动化测试体系，让 AI 能够自主验证新功能的正确性
2. 摸清 3D 游戏引擎的各个技术方案（渲染、动画、光照、物理等）
3. 验证 AI 全流程开发的可行性

---

## 🐛 已知问题 (Known Issues)

### Volumetric Lightmap
1. **Descriptor Heap Overflow** - 单帧内 bake 多个 brick 会超出限制
2. **Edge Discontinuity** - 边缘 probe 采样数量不足时方差较大
3. **Light Leaking** - 需要实现 Visibility/Occlusion 烘焙

---

## ✅ 已完成

### Phase 0: 基础设施
- **RHI 抽象层**: DX11/DX12 双后端，运行时切换
- **自动化测试框架**: 命令行驱动，帧回调，截图系统，断言系统
- **统一日志系统**: CFFLog，测试专用日志路径

### Phase 1: 渲染基础 ✅
- **PBR 渲染**: Cook-Torrance BRDF，Normal/Metallic-Roughness/Emissive/AO Map
- **CSM 阴影**: 1-4 级联，PCF 软阴影
- **IBL 系统**: Diffuse irradiance + Specular pre-filtered
- **Alpha 模式**: Opaque, Mask, Blend

### Phase 2: 光照系统 ✅
- **Clustered Forward+**: 100+ 动态光源 @ 60 FPS
- **Point/Spot Light**: 物理衰减，内外锥角
- **Reflection Probe**: TextureCubeArray，Box Projection
- **Volumetric Lightmap**: 自适应八叉树，GPU DXR 烘焙，Per-Pixel GI
  - 详细文档: `docs/VOLUMETRIC_LIGHTMAP.md`

### Phase 3.1: 2D Lightmap ✅
- **UV2 Generation**: xatlas 自动 UV 展开
- **Atlas Packing**: 多 Mesh 打包到单张贴图
- **GPU DXR Baking**: Path Tracing，多 Bounce GI
- **GPU Dilation**: 防止 UV 边缘黑边
- **Intel OIDN Denoising**: AI 降噪，98% 噪点消除
- **Runtime Sampling**: 着色器集成，Per-Object Scale/Offset
- 详细文档: `docs/LIGHTMAP.md`

---

## 🚧 Phase 3: 渲染进阶 (进行中)

**目标**: 高级渲染特性、后处理和架构升级

**实现顺序**:
```
3.1 Lightmap ✅ ─────────────────────────────────────┐
                                                     │
3.2 Deferred (G-Buffer) ──► 3.3 后处理 (SSAO/SSR)  │
                            │                        │
                            └──► 3.4 RTGI ──────────┤
                                                     │
3.5 Instancing ─────────────────────────────────────┤
                                                     │
3.6 RDG ──► 3.7 Descriptor Set ──► 3.8 Vulkan ─────┘
```

### 3.2 True Deferred Rendering ✅ COMPLETE

**技术文档**: `docs/DEFERRED_DOCUMENT.md`

True Deferred Pipeline 已完成，支持运行时切换 Forward/Deferred。

**已实现功能**:
- G-Buffer (5 RTs + Depth): WorldPos, Normal, Albedo, Emissive+MaterialID, Velocity
- Depth Pre-Pass: 消除 G-Buffer overdraw
- Deferred Lighting: 全屏 Quad，PBR + CSM + IBL
- Material System: Standard, Unlit (可扩展 Subsurface/Cloth/Hair)
- 透明物体: Forward 渲染
- 2D Lightmap: 预烘焙到 Emissive

**性能对比** (26 objects, 16 lights):
| Pipeline | FPS |
|----------|-----|
| Forward+ | 38.7 |
| Deferred | 27.9 |

Forward+ 更适合 <50 灯光场景；Deferred 更适合复杂场景和屏幕空间效果。

**验收测试**: TestGBuffer, TestMaterialTypes, TestDeferredPerf 通过

### 3.3 后处理栈

依赖 3.2 G-Buffer (velocity buffer ready in RT4)

- **3.3.1 Bloom + ACES Tonemapping** ✅ COMPLETE
  - HDR Bloom with Dual Kawase Blur (5 mip levels)
  - Soft threshold with knee for smooth falloff
  - ACES Filmic tonemapping
  - 详细文档: `docs/BLOOM.md`
- **3.3.2 SSAO** ✅ COMPLETE
  - Three algorithms: GTAO, HBAO, Crytek (runtime selectable)
  - Half-resolution compute with bilateral blur/upsample
  - Reversed-Z depth buffer support
  - Debug visualization modes
  - 详细文档: `docs/SSAO.md`
- **3.3.3 SSR** (需要 depth + normal + roughness)
- **3.3.4 TAA** (需要 velocity + history buffer)
  - Jittered projection matrix
  - Motion vector reprojection
  - Neighborhood clamping (3×3 or 5-tap)
  - Exponential history blend
- **3.3.5 DLSS** (需要 NVIDIA NGX SDK)
  - DLSS 3.x integration
  - Quality modes: Ultra Performance → Quality
  - Motion vectors + depth input
  - Optional: Frame Generation (DLSS 3)

**验收标准**: TestBloom ✅, TestSSAO ✅, TestSSR, TestTAA, TestDLSS 通过

### 3.4 RTGI (Real-Time Global Illumination) - 2-4周

依赖 3.2 G-Buffer，渐进式实现实时全局光照。

**阶段 A: SSGI** (屏幕空间 GI) - 3-4天
- 屏幕空间光线步进，采样击中点颜色
- 仅需 G-Buffer，无额外数据结构
- 限制：屏幕外物体无贡献

**阶段 B: DDGI** (Dynamic Diffuse GI) - 1-2周
- 实时更新 Light Probe 网格
- 复用 DXR 基础设施
- 每探针 64-256 rays/帧 + 时间滤波
- 存储: Irradiance (8×8 八面体) + Depth

**阶段 C: Lumen-like** (可选，研究性) - 2-3周
- Mesh SDF 预计算
- 软件光追 (Sphere Trace SDF)
- Radiance Cache 探针系统
- 混合：近距离屏幕追踪 + 远距离 SDF

**验收标准**: TestSSGI, TestDDGI 通过

### 3.5 GPU Instancing - 2-3天

单次 Draw Call 渲染多个相同 Mesh 实例，Per-instance Transform + Material ID。

**验收标准**: TestInstancing 通过 (1000 立方体, 1 Draw Call)

### 3.6 Render Dependency Graph (RDG) - 1周

自动化资源屏障和 Pass 依赖管理。

**核心**: Pass 声明 → 依赖分析 → 自动屏障插入

**验收标准**: TestRDG 通过

### 3.7 Descriptor Set 抽象 - 1周

统一 DX12 Root Signature / Vulkan Descriptor Set 管理。

**验收标准**: TestDescriptorSet 通过

### 3.8 Vulkan 后端 - 2周

添加 Vulkan 渲染后端，验证 RHI 抽象。

**核心组件**: VulkanContext, VulkanRenderContext, VulkanCommandList, VulkanSwapChain

**验收标准**: TestVulkanBasic 通过

---

## Phase 4: 动画系统 (预计 2-3周)

- **4.1 骨骼动画管线** - 1.5-2周: CSkeleton, CAnimationClip, 蒙皮顶点着色器
- **4.2 动画混合** - 3-4天 (可选): Blend(clipA, clipB, weight)

---

## Phase 5: 编辑器效率提升 (预计 1周)

- **5.1 Asset Browser** - 4-5天: 目录树 + 文件网格 + 拖放
- **5.2 Material Editor** - 2-3天: 实时预览 + 材质保存

---

## Phase 6: 物理系统 (3周，可选)

碰撞检测 + 刚体动力学

---

## Phase 7: 粒子系统 (2周，可选)

GPU 粒子 + Compute Shader

---

## References

- [Real Shading in Unreal Engine 4](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf)
- [Forward+: Bringing Deferred Lighting to the Next Level](https://takahiroharada.files.wordpress.com/2015/04/forward_plus.pdf)
- [Stupid Spherical Harmonics Tricks](https://www.ppsloan.org/publications/StupidSH36.pdf)
- [GTAO](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf)
- [Intel Open Image Denoise](https://www.openimagedenoise.org/)

---

**Last Updated**: 2026-01-12
