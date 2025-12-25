# Development Roadmap

**核心目标**:
1. 构建完善的自动化测试体系，让 AI 能够自主验证新功能的正确性
2. 摸清 3D 游戏引擎的各个技术方案（渲染、动画、光照、物理等）
3. 验证 AI 全流程开发的可行性

---

## 当前进度 (2025-12-25)

### ✅ 已完成

#### RHI 抽象层 + DX12 后端 (Phase 0.5) ⭐ NEW
- **RHI 抽象层**: 渲染硬件接口，支持多后端切换
  - `RHI/IRHIContext.h` - 上下文接口（Device, SwapChain, CommandQueue）
  - `RHI/IRHIRenderContext.h` - 渲染上下文接口（资源创建, PSO, CommandList）
  - `RHI/RHIManager.h` - 运行时后端管理和切换
  - `RHIFactory.h` - 后端工厂模式创建
- **DX12 后端实现**:
  - `DX12Context` - Device, SwapChain, CommandQueue, Fence 同步
  - `DX12RenderContext` - 资源创建, Root Signature, PSO Builder
  - `DX12CommandList` - 命令列表封装, 资源绑定
  - `DX12DescriptorHeap` - 描述符堆管理（CBV/SRV/UAV, Sampler, RTV, DSV）
  - `DX12UploadManager` - 上传堆管理（动态内存分配）
  - `DX12ResourceStateTracker` - 资源状态跟踪和屏障管理
  - `DX12PipelineState` - PSO Builder + Cache
- **DX12 Debug 基础设施**:
  - `DX12_CHECK` 宏 - 包装所有 D3D12 API 调用，输出文件名/行号
  - `DX12Debug.cpp` - InfoQueue 错误消息检索
  - Debug Layer 集成
- **Root Signature 配置**:
  - 7 个 CBV (b0-b6): PerFrame, PerObject, Material, ClusteredParams, Probes, LightProbe, VolumetricLightmap
  - 25 个 SRV (t0-t24): 材质纹理 + VolumetricLightmap 纹理
  - 8 个 UAV (u0-u7), 8 个 Sampler (s0-s7)
- **配置系统**:
  - `render.json` - 运行时后端选择（DX11/DX12）
  - `RenderConfig` - 配置加载和应用
- **ImGui DX12 支持**: 完整的 DX12 ImGui 后端集成

#### 自动化测试基础设施 (Phase 0)
- **测试框架**: 命令行驱动 (`--test TestName`)，帧回调架构，自动退出
  - `Core/Testing/TestCase.h` - 测试基类和上下文
  - `Core/Testing/TestRegistry.h` - 自动注册宏
  - `Tests/TestRayCast.cpp` - 示例测试用例
- **统一日志系统**: CFFLog 替代所有 console 输出，支持测试专用日志路径
  - 测试模式：E:/forfun/debug/{TestName}/runtime.log (独立)
  - 正常模式：E:/forfun/debug/logs/runtime.log (全局)
- **截图系统**: `Core/Testing/Screenshot.h` - PNG 截图保存，AI 可通过 Read tool 查看
  - 输出路径：E:/forfun/debug/{TestName}/screenshot_frame{N}.png
- **状态查询系统**: `CScene::GenerateReport()` 和 `CRenderStats` - AI 可读取场景逻辑状态
- **断言系统**: `ASSERT_*` 宏 + 类型化断言方法 - fail-fast，详细错误信息，Vector3 支持
- **测试工作流文档**: CLAUDE.md 完整的 6 步测试流程（实现→编写→运行→分析→报告→修复）
- **文档重组**: docs/RENDERING.md + docs/EDITOR.md 详细参考文档

#### 渲染和编辑器功能
- **PBR 渲染**: Cook-Torrance BRDF，物理正确的能量守恒
- **CSM 阴影**: 1-4 级联，bounding sphere stabilization + texel snapping，PCF 软阴影
- **IBL 系统**: Diffuse irradiance (32×32) + Specular pre-filtered (128×128, 7 mip)
  - GGX importance sampling, solid angle mip selection
- **场景光照设置**: Scene Light Settings 面板，支持天空盒配置和即时应用
- **IBL Debug 窗口**: 可视化 Irradiance/PreFilter/Environment 贴图
- **Transform Gizmo**: 平移/旋转/缩放，Local/World 切换，Grid snapping
- **HDR Export Tool**: HDR → KTX2 资源导出 (env/irr/prefilter)
- **鼠标拾取**: CPU 射线投射选择物体（Ray-AABB 相交测试）
- **地面参考网格**: Shader-based 无限网格，Unity 风格，双层级（1m+10m）
- **Debug 渲染系统**: GPU 几何着色器渲染 AABB 线框，深度测试
- **KTX2 集成**: libktx 库，跨平台纹理格式
- **.ffasset 格式**: JSON 资源描述符

#### Phase 1: 渲染基础完善 ✅
- **PBR 材质完善**: Normal Map, Metallic-Roughness Map, Emissive, AO
- **Alpha 模式**: Opaque, Mask (Alpha Test), Blend (Alpha Blend)
- **材质编辑器**: Inspector 内嵌材质属性编辑

#### Phase 2: 光照系统扩展 ✅
- **Clustered Forward+ 渲染**: 支持大量动态光源，Compute Shader 光源剔除
  - 3D Cluster Grid (16x16 tile, 16 depth slices)
  - 异步光源 Culling
- **Point Light**: 物理衰减 (1/d²)，范围软衰减
- **Spot Light**: 内外锥角，方向衰减
- **Reflection Probe 系统**:
  - TextureCubeArray 统一管理 (最多 8 个 Probe)
  - Per-object Probe 选择 (CPU 侧，基于物体中心位置)
  - 编辑器 Bake 工具 (KTX2 输出)
  - 默认 Fallback IBL (纯色，防止空 IBL)
  - 全局/局部 Probe 分离管理
- **Light Probe 系统**:
  - L2 球谐 (SH9) 编码/解码
  - CPU 烘焙 (Cubemap → SH 投影)
  - Per-object 采样 (基于位置)

#### Phase 2.5: Volumetric Lightmap ✅ (2025-12-09)
- **核心架构**:
  - 自适应八叉树 Brick 系统（基于场景几何密度）
  - 两级 GPU 查找：World Position → Indirection Texture → Brick Atlas
  - L1 球谐编码 (SH9, 9 coefficients × RGB)
  - 硬件 Trilinear 插值实现 Per-Pixel 采样
- **烘焙系统**:
  - Overlap Baking 消除 Brick 边缘接缝（边缘体素采样相同世界位置）
  - 自动派生参数（maxLevel, indirectionResolution, brickAtlasSize）
  - 详细进度日志 + ETA 估算
- **Diffuse GI 模式**:
  - `EDiffuseGIMode` 枚举：VolumetricLightmap / GlobalIBL / None
  - 场景级别设置（CB_Frame b0），独立于 VL 系统
  - 编辑器 UI 支持模式切换
- **调试功能**:
  - Octree Brick 线框可视化（颜色编码不同层级）
  - Show Octree Debug 开关

#### 架构改进 ✅
- **Hybrid Render Pipeline**: 统一的渲染管线架构，支持 Shadow/Main/Post-Processing 多阶段
- **FFPath 路径管理**: 统一的路径规范化和资源定位
- **IBLGenerator 移除**: BRDF LUT 迁移到 ReflectionProbeManager，简化架构

---

## Phase 1: 渲染基础完善 ✅ 已完成

**目标**: 补全 PBR 材质系统，验证现有渲染功能的正确性

### 1.1 PBR 输入补全

**缺失的 PBR 标准输入**:
```cpp
struct SMaterial {
    // 已有
    XMFLOAT3 albedo;
    float metallic;
    float roughness;
    std::string albedoTexture;

    // 需要添加
    std::string normalMap;              // 法线贴图（PBR 标配）
    std::string metallicRoughnessMap;   // 打包纹理 (G=Roughness, B=Metallic)
    std::string aoMap;                  // 环境光遮蔽
    XMFLOAT3 emissive;                 // 自发光颜色
    std::string emissiveMap;           // 自发光纹理
    float emissiveStrength;            // 发光强度（HDR）
};
```

**实现任务**:
1. **Emissive** - 自发光材质（最简单，优先）
2. **Normal Mapping** - 切线空间法线贴图
3. **AO Map** - 环境光遮蔽

### 1.2 Transparency 支持

**Alpha 模式**: Opaque, Mask, Blend

### 1.3 渲染系统测试套件

- TestPBRMaterials
- TestCSMShadows
- TestIBL
- TestTransparency

---

## Phase 2: 光照系统扩展 ✅ 已完成

**目标**: 构建完整的动态光照系统，支持多光源和局部 IBL

**实现策略**: 避免重复实现，Point Light直接与Forward+一起开发

### 2.1 Point Light + Forward+ 渲染 (合并实现) - 1-1.5周

**为什么合并？**
- 避免先实现简单Forward，再重构为Forward+的重复工作
- 从一开始就设计正确的Light Buffer和数据结构
- 一次性支持100+光源，不受性能限制

**组件**:
```cpp
struct SPointLight : public IComponent {
    XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    bool castShadows = false;  // Phase 2暂不实现，Phase 3添加
};
```

**Forward+ 核心算法**:
1. **Tile划分** (16×16像素)
   - Screen space划分为tile grid
   - 每个tile独立剔除光源

2. **Compute Shader Light Culling**
   - 输入：所有光源的AABB/Sphere
   - 输出：每个tile的light index list
   ```hlsl
   // LightCulling.compute.hlsl
   RWStructuredBuffer<uint> tileData;  // [tileIdx * maxLightsPerTile]

   [numthreads(16, 16, 1)]
   void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
       uint tileIdx = GetTileIndex(dispatchThreadID.xy);
       Frustum tileFrustum = ComputeTileFrustum(tileIdx);

       uint lightCount = 0;
       for (uint i = 0; i < numLights; i++) {
           if (SphereIntersectsFrustum(lights[i], tileFrustum)) {
               tileData[tileIdx * MAX_LIGHTS + lightCount++] = i;
           }
       }
   }
   ```

3. **Pixel Shader只处理可见光源**
   ```hlsl
   // MainPass.ps.hlsl
   float4 PSMain(PSInput input) : SV_Target {
       uint tileIdx = GetTileIndex(input.position.xy);
       uint lightStart = tileIdx * MAX_LIGHTS_PER_TILE;

       float3 lighting = 0;
       for (uint i = 0; i < tileData[lightStart]; i++) {
           uint lightIdx = tileData[lightStart + i + 1];
           lighting += CalculatePointLight(lights[lightIdx], ...);
       }
       return float4(lighting, 1.0);
   }
   ```

**着色器实现细节**:
- 物理衰减：`1 / (distance²)`
- 范围平滑过渡：`smoothstep(range - 0.5, range, distance)`
- Cook-Torrance BRDF 复用（与DirectionalLight相同公式）

**验收标准**:
- TestPointLights - 16个点光源基础测试
- TestForwardPlus - 100个点光源 @ 1080p 60 FPS

**测试场景**: 夜晚城市（路灯、霓虹灯）

### 2.2 Spot Light - 3-4天

**组件**:
```cpp
struct SSpotLight : public IComponent {
    XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerConeAngle = 15.0f;  // 全亮区域角度
    float outerConeAngle = 30.0f;  // 边缘衰减角度
    bool castShadows = false;
};
```

**着色器实现**:
- 复用Forward+架构（Light Buffer增加direction字段）
- 锥形衰减：
  ```hlsl
  float spotAttenuation = dot(normalize(lightDir), spotDirection);
  float spotFactor = smoothstep(cos(outerAngle), cos(innerAngle), spotAttenuation);
  ```
- Cookie纹理（可选）- 投影纹理实现图案

**验收标准**: TestSpotLight 通过，混合Point + Spot光源渲染

**测试场景**: 舞台灯光 / 手电筒

### 2.3 Reflection Probe (局部 IBL) - 1周

**组件**:
```cpp
struct SReflectionProbe : public IComponent {
    XMFLOAT3 boxMin{-5, -5, -5};
    XMFLOAT3 boxMax{5, 5, 5};
    int resolution = 128;
    bool isBoxProjection = true;
    std::string bakedCubemapPath;  // .ffasset格式
};
```

**核心技术**:
1. **Cubemap Baking（编辑器工具）**
   - 在Probe位置渲染6个方向（+X, -X, +Y, -Y, +Z, -Z）
   - 保存为KTX2格式（复用现有HDR Export工具）
   - 生成mipmap chain（specular pre-filtering）

2. **Box Projection（Shader中修正反射方向）**
   ```hlsl
   // 将反射方向从无限远修正到box边界
   float3 BoxProjection(float3 reflectDir, float3 worldPos,
                        float3 boxMin, float3 boxMax, float3 probePos) {
       float3 rbmax = (boxMax - worldPos) / reflectDir;
       float3 rbmin = (boxMin - worldPos) / reflectDir;
       float3 rbminmax = max(rbmax, rbmin);
       float intersectDist = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
       float3 intersectPos = worldPos + reflectDir * intersectDist;
       return intersectPos - probePos;
   }
   ```

3. **多Probe混合**（可选，Phase 2暂不实现）
   - 基于距离权重混合
   - Phase 3再添加

**验收标准**: TestReflectionProbe 通过
- VISUAL_EXPECTATION: 金属球反射室内红墙和蓝墙，而非天空盒

**测试场景**: 室内走廊，金属门把手

### 2.4 Light Probe (球谐光照) - ✅ 已完成 (升级为 Volumetric Lightmap + DXR)

**原方案**: 单点 Light Probe 采样
**实际方案**: Volumetric Lightmap + GPU DXR 烘焙（更优解决方案）

**升级内容**:
- **Volumetric Lightmap**: 自适应八叉树 Brick 系统，Per-Pixel 采样（比 Per-Object 更精细）
- **GPU DXR 烘焙**: CDXRCubemapBaker 实现，64 voxels 批量 dispatch
- **多 Bounce GI**: GPU Path Tracing 支持多次反弹
- **L2 SH 编码**: 9 系数 × RGB，硬件 Trilinear 插值

**详细文档**: `docs/VOLUMETRIC_LIGHTMAP.md`

**验收标准**: TestDXRBakeVisualize 通过

**时间估计**: Phase 2 总计 3-4 周
- Point Light + Forward+: 1-1.5周
- Spot Light: 3-4天
- Reflection Probe: 1周
- Light Probe (Volumetric Lightmap): ✅ 已完成

---

## Phase 3: 渲染进阶 (预计 8-10周)

**目标**: 高级渲染特性、后处理和架构升级

**实现顺序** (基于依赖关系):
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

**目标**: 烘焙静态光照到纹理，提升静态场景性能和视觉质量

**核心技术**:

1. **UV2 生成（Lightmap UV）**
   - 独立的 UV 通道（不重叠，均匀分布）
   - 工具：xatlas 库自动生成

   ```cpp
   struct Vertex {
       XMFLOAT3 position;
       XMFLOAT3 normal;
       XMFLOAT2 uv;        // 原始 UV（用于 Albedo 等纹理）
       XMFLOAT2 lightmapUV; // Lightmap UV（用于烘焙光照）
   };
   ```

2. **Lightmap Baking（编辑器工具）**
   - 复用 DXR 烘焙基础设施（CDXRCubemapBaker）
   - 输出：Lightmap 纹理（HDR 格式，R16G16B16A16_FLOAT）

3. **Shader 集成**
   ```hlsl
   Texture2D gLightmap : register(t5);

   float4 PSMain(PSInput input) : SV_Target {
       float3 bakedLighting = gLightmap.Sample(gSampler, input.lightmapUV).rgb;
       float3 dynamicLighting = CalculateDynamicLights(...);
       float3 finalColor = albedo * (bakedLighting + dynamicLighting);
       return float4(finalColor, 1.0);
   }
   ```

**验收标准**: TestLightmap 通过

### 3.2 Deferred 渲染 (Hybrid) - 1周

**目标**: 添加 G-Buffer 支持 SSAO 和 SSR 等屏幕空间效果

**架构**: Hybrid Deferred (Forward+ 主渲染 + G-Buffer Pre-pass)
- 保留 Forward+ 用于透明物体和主光照
- 添加 G-Buffer 用于屏幕空间效果

**G-Buffer 布局**:
```
RT0: Albedo.rgb + Metallic.a      (R8G8B8A8_UNORM)
RT1: Normal.xyz + Roughness.a     (R16G16B16A16_FLOAT)
RT2: Emissive.rgb + AO.a          (R8G8B8A8_UNORM)
Depth: D24S8 或 D32F
```

**实现步骤**:
1. 创建 G-Buffer 纹理和 RTV
2. 实现 G-Buffer Pass shader
3. 修改 MainPass 读取 G-Buffer depth/normal
4. 透明物体仍走 Forward+ 路径

**验收标准**: TestDeferredGBuffer 通过

### 3.3 后处理栈 - 1-2周

**目标**: 现代后处理效果（依赖 3.2 G-Buffer）

#### 3.3.1 Bloom + ACES Tonemapping - 4-5天

**实现**:
- Bright Pass
- Gaussian Blur (3-pass)
- ACES Tonemapping
- 曝光控制

**验收标准**: TestBloom 通过

#### 3.3.2 SSAO - 2-3天

**依赖**: G-Buffer (depth + normal)

#### 3.3.3 SSR - 3-4天

**依赖**: G-Buffer (depth + normal + roughness)

### 3.4 GPU Instancing - 2-3天

**目标**: 减少 Draw Call，提升大量物体渲染性能

**核心技术**:
- 单次 Draw Call 渲染多个相同 Mesh 的实例
- 使用 `DrawIndexedInstanced()`
- Per-instance 数据：Transform Matrix, Material ID

```hlsl
struct InstanceData {
    float4x4 worldMatrix;
    uint materialID;
};
StructuredBuffer<InstanceData> gInstanceData : register(t10);

VSOutput main(VSInput input, uint instanceID : SV_InstanceID) {
    InstanceData inst = gInstanceData[instanceID];
    float4 worldPos = mul(float4(input.position, 1.0), inst.worldMatrix);
    // ...
}
```

**验收标准**: TestInstancing 通过
- 场景：1000 个相同的立方体
- Draw Call 从 1000 降低到 1

### 3.5 Render Dependency Graph (RDG) - 1周

**目标**: 自动化资源屏障和渲染 Pass 依赖管理

**核心设计**:
```cpp
class CRenderGraph {
    RGTextureHandle CreateTexture(const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(const RGBufferDesc& desc);

    void AddPass(const char* name,
                 std::function<void(RGPassBuilder&)> setup,
                 std::function<void(ICommandList*)> execute);

    void Compile();   // 分析依赖，生成屏障
    void Execute();   // 执行所有 Pass
};
```

**实现任务**:
1. Pass 声明和依赖跟踪
2. 资源生命周期分析
3. 自动屏障插入 (DX12: ResourceBarrier, Vulkan: Pipeline Barrier)
4. 资源别名 (Aliasing) 优化

**验收标准**: TestRDG 通过

### 3.6 Descriptor Set 抽象 - 1周

**目标**: 统一 DX12 Root Signature / Vulkan Descriptor Set 管理

**核心设计**:
```cpp
struct SDescriptorSetLayout {
    std::vector<SDescriptorBinding> bindings;
};

class IDescriptorSet {
    virtual void SetConstantBuffer(uint32_t binding, IBuffer* buffer) = 0;
    virtual void SetTexture(uint32_t binding, ITexture* texture) = 0;
    virtual void SetSampler(uint32_t binding, ISampler* sampler) = 0;
};
```

**验收标准**: TestDescriptorSet 通过

### 3.7 Vulkan 后端 - 2周

**目标**: 添加 Vulkan 渲染后端，验证 RHI 抽象

**核心组件**:
- `VulkanContext` - Instance, Device, Queue
- `VulkanRenderContext` - Pipeline, Descriptor Pool
- `VulkanCommandList` - Command Buffer
- `VulkanSwapChain` - Surface, Present

**验收标准**: TestVulkanBasic 通过

---

## Phase 4: 动画系统 (预计 2-3周)

**目标**: 骨骼动画和动画混合

### 4.1 骨骼动画管线 - 1.5-2周

**数据结构**:
- CSkeleton (joints, globalTransforms)
- CAnimationClip (keyframes, channels)
- SAnimator 组件

**着色器**: 蒙皮顶点着色器 (CB_Skin, jointMatrices)

**验收标准**: TestSkeletalAnimation 通过

### 4.2 动画混合 (可选) - 3-4天

**功能**: Blend(clipA, clipB, weight)

**验收标准**: TestAnimationBlending 通过

---

## Phase 5: 编辑器效率提升 (预计 1周)

### 5.1 Asset Browser - 4-5天

**功能**: 目录树 + 文件网格 + 拖放

### 5.2 Material Editor - 2-3天

**功能**: 实时预览 + 材质保存

---

## Phase 6: 物理系统 (3周，可选)

碰撞检测 + 刚体动力学

---

## Phase 7: 粒子系统 (2周，可选)

GPU 粒子 + Compute Shader

---

## Technical Recommendations

### Rendering
- 推荐 Forward+ (透明物体友好)
- 仅 1000+ 光源时考虑 Deferred

### Animation
- 手动实现 (cgltf)

### Post-Processing Priority
- Must-Have: Bloom + ACES
- High Value: SSAO
- Optional: SSR

---

## References

### Lighting
- [Real Shading in Unreal Engine 4](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf)
- [Forward+: Bringing Deferred Lighting to the Next Level](https://takahiroharada.files.wordpress.com/2015/04/forward_plus.pdf)
- [AMD Forward+ Rendering](https://gpuopen.com/learn/lighting/forward-plus/)
- [Parallax-corrected Cubemap](https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/)
- [Stupid Spherical Harmonics Tricks](https://www.ppsloan.org/publications/StupidSH36.pdf)

### Reflection Probes
- [Unity Reflection Probes](https://docs.unity3d.com/Manual/ReflectionProbes.html)
- [UE4 Reflection Environment](https://docs.unrealengine.com/4.27/en-US/BuildingWorlds/LightingAndShadows/ReflectionEnvironment/)

### Animation
- [glTF 2.0 Skins](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#skins)
- GPU Gems 1 - Chapter 4: Skinning

### Post-Processing
- [ACES](https://github.com/ampas/aces-dev)
- Next Generation Post Processing in Call of Duty (Jimenez, SIGGRAPH 2014)
- [GTAO](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf)

### Testing
- [Snapshot Testing - Jest](https://jestjs.io/docs/snapshot-testing)
- [Visual Regression Testing](https://www.browserstack.com/guide/visual-regression-testing)

---

**Last Updated**: 2025-12-25
