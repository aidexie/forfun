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

## Phase 2: 光照系统扩展 (预计 4-5周)

**目标**: 构建完整的动态光照系统，支持多光源和局部 IBL

### 2.1 Point Light (Forward 渲染) - 1周

**组件**:
```cpp
struct SPointLight : public IComponent {
    XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    bool castShadows = false;
};
```

**着色器实现**:
- 物理衰减：`1 / (distance²)`
- 范围平滑过渡：`smoothstep(range)`
- Cook-Torrance BRDF 复用
- 限制：16 个点光源

**验收标准**: TestPointLights 通过

### 2.2 Spot Light - 3-4天

**组件**:
```cpp
struct SSpotLight : public IComponent {
    XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerConeAngle = 15.0f;
    float outerConeAngle = 30.0f;
    bool castShadows = false;
};
```

**着色器实现**:
- 锥形衰减
- Cookie 纹理（可选）

**验收标准**: TestSpotLight 通过

### 2.3 Reflection Probe (局部 IBL) - 1周

**组件**:
```cpp
struct SReflectionProbe : public IComponent {
    XMFLOAT3 boxMin{-5, -5, -5};
    XMFLOAT3 boxMax{5, 5, 5};
    int resolution = 128;
    bool isBoxProjection = true;
    std::string bakedCubemapPath;
};
```

**核心技术**:
1. **Baking** - 编辑器中渲染 6 个面
2. **Box Projection** - 修正反射方向
3. **Blending** - 多 Probe 混合

**验收标准**: TestReflectionProbe 通过，室内金属球反射显示墙壁而非天空

### 2.4 Light Probe (球谐光照) - 3-4天

**组件**:
```cpp
struct SLightProbe : public IComponent {
    float sh[9 * 3];  // 9 个球谐系数 × RGB (L0-L2)
    XMFLOAT3 boxMin, boxMax;
    float blendDistance = 1.0f;
};
```

**核心技术**:
1. **SH Encoding** - 将 cubemap 投影到球谐基函数
```cpp
// Band 0-2 (9 coefficients)
for each pixel (direction, color):
    for l = 0 to 2:
        for m = -l to l:
            sh[idx] += color * SH_basis(l, m, direction) * solidAngle
```

2. **SH Decoding in Shader**
```hlsl
float3 EvaluateSH(float3 normal, float sh[27]) {
    // L0 (DC)
    float3 result = sh[0] * 0.282095;
    // L1 (linear)
    result += sh[1] * 0.488603 * normal.y;
    result += sh[2] * 0.488603 * normal.z;
    result += sh[3] * 0.488603 * normal.x;
    // L2 (quadratic) - 5 more terms
    return result;
}
```

3. **Probe Blending** - 基于位置混合

**与 Reflection Probe 区别**:
- **Reflection Probe**: 镜面反射（高频），cubemap，用于金属
- **Light Probe**: 漫反射（低频），球谐，用于非金属

**验收标准**: TestLightProbe 通过，漫反射物体受局部环境光影响

### 2.5 Forward+ 渲染 (Tiled Light Culling) - 1-1.5周

**目标**: 100+ 光源 @ 60 FPS

**核心算法**:
1. Tile 划分 (16×16 像素)
2. Compute Shader Light Culling
3. Pixel Shader 只处理可见光源

**验收标准**: TestForwardPlus 通过，100 光源 @ 1080p 60 FPS

### 2.6 Deferred 渲染 (可选) - 1周

**G-Buffer 布局**:
- RT0: Albedo + AO
- RT1: Normal + Roughness
- RT2: Metallic
- Depth

**决策**: Forward+ 和 Deferred 暂定都做，到时候可能只做一个

**时间估计**: Phase 2 总计 4-5 周

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

**Last Updated**: 2025-11-25
