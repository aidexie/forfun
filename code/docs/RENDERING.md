# Rendering System Documentation

Complete reference for graphics rendering, shadow system, IBL, and pipeline architecture.

---

## Graphics Rendering Standards

### **CRITICAL: Physics-Based Rendering**

所有图形特性必须**物理正确**。这是最高优先级要求。

**禁止的非物理 Hack**:
- 让阴影影响 ambient/IBL（阴影仅影响直接光）
- 无物理依据的乘数（如 `color *= 1.5` 为了"更好看"）
- Clamp 应该是 HDR 的值

**允许的物理参数**:
- 曝光控制、Tone mapping、Bloom
- 用户可调强度滑块（如 `gIblIntensity`）
- 预计算顶点色 AO

### Energy Conservation

```hlsl
// BRDF 能量守恒: kS + kD ≤ 1.0
float3 kS = F;
float3 kD = (1.0 - kS) * (1.0 - metallic);

// 直接光: Lambert = albedo/π
float3 diffuse = kD * albedo / PI;

// IBL: 必须除以 π 以匹配直接光
float3 diffuseIBL = irradiance * albedo;
float3 ambient = kD_IBL * (diffuseIBL / PI) + specularIBL;
```

**References**: pbrt, UE4 Real Shading (Karis), Disney BRDF (Burley)

---

## Rendering Pipeline

### Frame Flow

```cpp
gMainPass.UpdateCamera(vpWidth, vpHeight, dt);
if (dirLight) {
    gShadowPass.Render(gScene, dirLight, gMainPass.GetCameraViewMatrix(), gMainPass.GetCameraProjMatrix());
}
gMainPass.Render(gScene, vpWidth, vpHeight, dt, &gShadowPass.GetOutput());
DrawViewport(gMainPass.GetOffscreenSRV(), ...);
```

### Color Space

```
Albedo (UNORM_SRGB) → GPU converts to Linear
    ↓
HDR Linear (R16G16B16A16_FLOAT)
    ↓
PostProcess (Tone Mapping)
    ↓
Final (R8G8B8A8_UNORM_SRGB) → GPU applies Gamma
```

**规则**:
- Albedo/Emissive: `UNORM_SRGB`
- Normal/Metallic/Roughness/AO: `UNORM`
- Intermediate RT: `R16G16B16A16_FLOAT`
- Final: `R8G8B8A8_UNORM_SRGB`

---

## Shadow System (CSM)

### Overview

- 1-4 cascades
- Bounding sphere stabilization (消除旋转抖动)
- Texel snapping (消除移动抖动)
- PCF 3×3 软阴影

### DirectionalLight Parameters

**位置**: `Engine/Components/DirectionalLight.h`

**参数**:
- `ShadowMapSizeIndex`: 1024/2048/4096
- `ShadowDistance`: 最大阴影距离
- `ShadowBias`: 深度偏移（防止 shadow acne）
- `ShadowNearPlaneOffset`: 近平面偏移
- `CascadeCount`: 级联数量 (1-4)
- `CascadeSplitLambda`: 级联分割参数 (0-1)
- `CascadeBlendRange`: 级联混合范围
- `DebugShowCascades`: 调试显示级联颜色

### Implementation Details

**Bounding Sphere Stabilization**:
```cpp
// 使用包围球而不是紧密 AABB
// 好处: 相机旋转时光照矩阵不变，阴影稳定
XMFLOAT3 center = cascade_frustum_center;
float radius = cascade_frustum_max_distance;
XMVECTOR lightPos = center - lightDir * radius;
XMMatrixOrthographicLH(radius * 2, radius * 2, 0.0f, radius * 2);
```

**Texel Snapping**:
```cpp
// 光照空间坐标对齐到纹素网格
// 好处: 相机平移时纹素采样位置不变
float texelSize = shadowMapSize / (radius * 2.0f);
XMVECTOR shadowOrigin = XMVectorRound(shadowOrigin * texelSize) / texelSize;
```

**PCF 3×3 Soft Shadows**:
```hlsl
float shadow = 0.0;
for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
        float2 offset = float2(x, y) * texelSize;
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, depth);
    }
}
shadow /= 9.0;
```

---

## IBL System

### Components

**位置**:
- `Engine/Rendering/IBLGenerator.h/cpp` - IBL 生成器
- `Shaders/IrradianceConvolution.ps.hlsl` - 漫反射辐照度
- `Shaders/PreFilterEnvironmentMap.ps.hlsl` - 镜面预过滤

### Diffuse Irradiance (Uniform Solid Angle Sampling)

**核心算法**:
```hlsl
float cosTheta = 1.0 - v;  // Linear in cos(θ) → uniform solid angle
irradiance += color * cosTheta;
```

**为什么这样采样**:
- 球面积分在 cos(θ) 上均匀分布，不是在 θ 上
- `cosTheta = 1.0 - v` 确保采样密度与贡献度匹配
- 避免极点过度采样/欠采样

**参数**:
- 分辨率: 32×32
- 采样数: 每像素 ~1024 (θ, φ 均匀网格)

### Specular Pre-Filtered Map (GGX Importance Sampling)

**核心算法**:
```hlsl
// 1. Hammersley 序列生成低差异采样
float2 Xi = Hammersley(i, SAMPLE_COUNT);

// 2. GGX 重要性采样: 根据 roughness 生成 half vector
float3 H = ImportanceSampleGGX(Xi, roughness);

// 3. 计算反射方向
float3 L = normalize(2.0 * dot(V, H) * H - V);
```

**动态采样数**:
```cpp
// 根据 roughness 动态调整采样数
// 粗糙表面需要更多采样以覆盖更宽的镜面波瓣
int sampleCount = 8192 + (int)(roughness * 56832);  // 8K-65K
```

**Solid Angle Mip Selection**:
```hlsl
// 根据采样分布和纹理分辨率选择正确的 mip level
float saSample = 1.0 / (pdf * SAMPLE_COUNT);  // 每个采样覆盖的立体角
float saTexel = 4.0 * PI / (6.0 * envResolution * envResolution);  // 每个纹素的立体角
float mipLevel = 0.5 * log2(saSample / saTexel);  // mip level 选择
```

**为什么需要 mip selection**:
- 避免欠采样（aliasing）
- 模拟预积分的模糊效果
- 匹配 Split Sum Approximation 的第一项

**参数**:
- 分辨率: 128×128
- Mip levels: 7 (128 → 64 → 32 → 16 → 8 → 4 → 2)
- 采样数: 8K-65K (roughness-dependent)

### Split Sum Approximation

IBL 镜面项的完整积分:
```
Lo = ∫ Li(l) * f(l,v) * (n·l) dl
```

分解为两项（Split Sum）:
```
Lo ≈ (∫ Li(l) * (n·l) dl) * (∫ f(l,v) dl)
     ↑ Pre-filtered env map      ↑ BRDF LUT (2D texture)
```

**Pre-filtered map**: 预积分环境光照（本系统已实现）
**BRDF LUT**: 预积分 BRDF 响应（通常是 2D 纹理，未来实现）

---

## IBL Debug UI

**位置**: `Editor/Panels_IrradianceDebug.cpp`

**功能**: 三个 Tab 实时预览 IBL 纹理

### Tab 1: Irradiance Map
- 显示: diffuse irradiance (32×32)
- 用途: 验证漫反射环境光是否正确
- 特性: 模糊的颜色球，表示整体环境色调

### Tab 2: Pre-Filtered Map
- 显示: specular pre-filtered map (128×128)
- 用途: 验证镜面预过滤是否正确
- 控制: Mip level slider (0-6)
  - Mip 0: 清晰反射
  - Mip 6: 完全模糊（高粗糙度）

### Tab 3: Environment Map
- 显示: 原始环境贴图
- 用途: 对比源数据
- 控制: Mip level slider (0-9)

**使用方法**: Window → Irradiance Debug

---

## Scene Light Settings

**位置**:
- `Engine/SceneLightSettings.h` - 数据结构
- `Editor/Panels_SceneLightSettings.cpp` - UI 面板

**功能**: 场景级别光照配置，独立于 GameObject 系统

**当前支持**:
- Skybox 资源路径（.ffasset）
- 即时应用（修改后立即重新加载）

**访问**:
```cpp
auto& settings = CScene::Instance().GetLightSettings();
settings.skyboxAssetPath = "skybox/test.ffasset";
CScene::Instance().Initialize(settings.skyboxAssetPath);  // 应用
```

**序列化**: 保存到 `.scene` 文件的 `lightSettings` 节点

**UI**: Window → Scene Light Settings

---

## KTX2 资源加载

### .ffasset 格式

**位置**: assets/skybox/*.ffasset

**格式**:
```json
{
  "type": "skybox",
  "version": "1.0",
  "source": "source.hdr",
  "data": {
    "env": "name_env.ktx2",
    "irr": "name_irr.ktx2",
    "prefilter": "name_prefilter.ktx2"
  }
}
```

**字段说明**:
- `type`: 资源类型（当前仅 "skybox"）
- `version`: 格式版本
- `source`: 源 HDR 文件名（用于溯源）
- `data.env`: 环境立方体贴图（512×512）
- `data.irr`: 漫反射辐照度图（32×32）
- `data.prefilter`: 镜面预过滤图（128×128, 7 mip）

### 加载流程

**位置**: `Core/Loader/KTXLoader.h/cpp`

```cpp
// 1. 解析 .ffasset
auto metadata = ParseFFAsset(assetPath);

// 2. 加载三个 KTX2 文件
ID3D11ShaderResourceView* envSRV = LoadKTX(metadata.envPath);
ID3D11ShaderResourceView* irrSRV = LoadKTX(metadata.irrPath);
ID3D11ShaderResourceView* prefilterSRV = LoadKTX(metadata.prefilterPath);

// 3. 传递给 Skybox/MainPass
skybox.SetTexture(envSRV);
mainPass.SetIBLTextures(irrSRV, prefilterSRV);
```

**注意**:
- KTX2 文件使用 BC6H (HDR) 压缩
- 所有纹理为立方体贴图（6 个面）
- Pre-filtered map 包含完整 mip chain

---

## 后续计划

### Phase 1: 基础完善
- [ ] BRDF LUT 生成与采样（完成 Split Sum 第二项）
- [ ] 点光源 + 聚光灯阴影
- [ ] 阴影级联过渡优化（消除硬边界）

### Phase 2: 高级特性
- [ ] Screen-space reflections (SSR)
- [ ] Temporal anti-aliasing (TAA)
- [ ] Bloom + Auto-exposure

### Phase 3: 性能优化
- [ ] GPU-driven rendering
- [ ] Frustum culling + Occlusion culling
- [ ] Instanced rendering

---

**Last Updated**: 2025-11-25
