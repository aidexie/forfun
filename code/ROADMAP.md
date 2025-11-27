# Development Roadmap

**核心目标**:
1. 构建完善的自动化测试体系，让 AI 能够自主验证新功能的正确性
2. 摸清 3D 游戏引擎的各个技术方案（渲染、动画、光照、物理等）
3. 验证 AI 全流程开发的可行性

---

## 当前进度 (2025-11-25)

### ✅ 已完成

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

---

## Phase 1: 渲染基础完善 (当前阶段，预计 2-3周)

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

## Phase 2: 光照系统扩展 (预计 3-3.5周)

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

### 2.4 Light Probe (球谐光照) - 可选，3-4天

**优先级**: ⚠️ **低优先级，建议Phase 2先跳过**

**原因**:
- 实现复杂（SH编码/解码，数学密集）
- 视觉效果微妙（漫反射环境光）
- 调试困难（SH系数难以可视化）
- 当前全局IBL已满足大部分需求

**何时需要**:
- 大型开放世界（需要per-region环境光）
- 完整GI解决方案

**如果实现**:

**组件**:
```cpp
struct SLightProbe : public IComponent {
    float sh[9 * 3];  // 9个球谐系数 × RGB (L0-L2)
    XMFLOAT3 boxMin, boxMax;
    float blendDistance = 1.0f;
};
```

**核心技术**:
1. **SH Encoding** - 将cubemap投影到球谐基函数
2. **SH Decoding in Shader**
3. **Probe Blending**

**与 Reflection Probe 区别**:
- **Reflection Probe**: 镜面反射（高频），cubemap，用于金属
- **Light Probe**: 漫反射（低频），球谐，用于非金属

**验收标准**: TestLightProbe 通过

### 2.5 Deferred 渲染 - ❌ 不推荐实现

**为什么不做？**
1. **Forward+已足够** - 100+光源 @ 60 FPS，满足绝大多数需求
2. **透明物体问题** - Deferred无法处理透明，需要单独Forward pass
3. **MSAA成本** - G-Buffer的MSAA成本高
4. **材质灵活性** - Forward+可以有不同shader，Deferred被G-Buffer限制

**仅在以下情况考虑**:
- 需要1000+光源（极端场景）
- 需要屏幕空间后处理（SSAO, SSR）需要G-Buffer

**决策**: Phase 2跳过，Forward+作为主渲染路径

**时间估计**: Phase 2 总计 3-3.5 周
- Point Light + Forward+: 1-1.5周
- Spot Light: 3-4天
- Reflection Probe: 1周
- Light Probe (可选): 跳过或3-4天

---

## Phase 3: 动画系统 (预计 2-3周)

**目标**: 骨骼动画和动画混合

### 3.1 骨骼动画管线 - 1.5-2周

**数据结构**:
- CSkeleton (joints, globalTransforms)
- CAnimationClip (keyframes, channels)
- SAnimator 组件

**着色器**: 蒙皮顶点着色器 (CB_Skin, jointMatrices)

**验收标准**: TestSkeletalAnimation 通过

### 3.2 动画混合 (可选) - 3-4天

**功能**: Blend(clipA, clipB, weight)

**验收标准**: TestAnimationBlending 通过

---

## Phase 4: 后处理栈 (预计 1-2周)

### 4.1 Bloom + ACES Tonemapping - 4-5天

**实现**:
- Bright Pass
- Gaussian Blur (3-pass)
- ACES Tonemapping
- 曝光控制

**验收标准**: TestBloom 通过

### 4.2 SSAO (可选) - 2-3天

### 4.3 SSR (可选) - 3-4天

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

**Last Updated**: 2025-11-27
