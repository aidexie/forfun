# Development Roadmap

**核心目标**:
1. 构建完善的自动化测试体系，让 AI 能够自主验证新功能的正确性
2. 摸清 3D 游戏引擎的各个技术方案（渲染、动画、光照、物理等）
3. 验证 AI 全流程开发的可行性

---

## 🐛 已知问题 (Known Issues)

### DX12 后端
1. **纹理初始数据上传未完成** - CreateTexture 的 initialData 需要通过 Upload Heap 复制
2. **Buffer 初始数据上传未完成** - 非 Upload Heap 的 Buffer 需要额外复制步骤
3. **资源状态跟踪警告** - 某些资源创建后未注册到 ResourceStateTracker

### Volumetric Lightmap
1. **Descriptor Heap Overflow** - 单帧内 bake 多个 brick 会超出限制
2. **Edge Discontinuity** - 边缘 probe 采样数量不足时方差较大
3. **Light Leaking** - 需要实现 Visibility/Occlusion 烘焙

### 其他
- **无 Probe 混合**: Reflection Probe 边界有跳变
- **无实时更新**: Probe 必须手动烘焙

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

---

## Phase 3: 渲染进阶 (预计 8-10周)

**目标**: 高级渲染特性、后处理和架构升级

**实现顺序**:
```
3.1 Lightmap ──────────────────────────────────────┐
                                                    │
3.2 Deferred (G-Buffer) ──► 3.3 后处理 (SSAO/SSR) │
                                                    │
3.4 Instancing ────────────────────────────────────┤
                                                    │
3.5 RDG ──► 3.6 Descriptor Set ──► 3.7 Vulkan ────┘
```

### 3.1 Lightmap 支持 - 3-4天

复用 DXR 烘焙基础设施，烘焙静态光照到 UV2 纹理空间。

**核心**: UV2 生成 (xatlas) + DXR Baking + Shader 采样

**验收标准**: TestLightmap 通过

### 3.2 Deferred 渲染 (Hybrid) - 1周

Hybrid Deferred: Forward+ 主渲染 + G-Buffer Pre-pass

**G-Buffer 布局**:
- RT0: Albedo.rgb + Metallic.a (R8G8B8A8_UNORM)
- RT1: Normal.xyz + Roughness.a (R16G16B16A16_FLOAT)
- RT2: Emissive.rgb + AO.a (R8G8B8A8_UNORM)

**验收标准**: TestDeferredGBuffer 通过

### 3.3 后处理栈 - 1-2周

依赖 3.2 G-Buffer

- **3.3.1 Bloom + ACES Tonemapping** - 4-5天
- **3.3.2 SSAO** - 2-3天 (需要 depth + normal)
- **3.3.3 SSR** - 3-4天 (需要 depth + normal + roughness)

**验收标准**: TestBloom, TestSSAO, TestSSR 通过

### 3.4 GPU Instancing - 2-3天

单次 Draw Call 渲染多个相同 Mesh 实例，Per-instance Transform + Material ID。

**验收标准**: TestInstancing 通过 (1000 立方体, 1 Draw Call)

### 3.5 Render Dependency Graph (RDG) - 1周

自动化资源屏障和 Pass 依赖管理。

**核心**: Pass 声明 → 依赖分析 → 自动屏障插入

**验收标准**: TestRDG 通过

### 3.6 Descriptor Set 抽象 - 1周

统一 DX12 Root Signature / Vulkan Descriptor Set 管理。

**验收标准**: TestDescriptorSet 通过

### 3.7 Vulkan 后端 - 2周

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

---

**Last Updated**: 2025-12-25
