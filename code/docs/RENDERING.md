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

### Color Space & Texture Formats

| Texture Type | Format |
|--------------|--------|
| Albedo/Emissive | `UNORM_SRGB` |
| Normal/Metallic/Roughness/AO | `UNORM` |
| Intermediate RT (HDR) | `R16G16B16A16_FLOAT` |

### CSM Shadow

1-4 cascades, PCF 3×3 filtering, bounding sphere stabilization + texel snapping.

### IBL (Image-Based Lighting)

- Diffuse irradiance: 32×32 cubemap
- Specular pre-filtered: 128×128 cubemap, 7 mip levels

### Diffuse GI Mode

```cpp
// SceneLightSettings.h
enum class EDiffuseGIMode : int {
    VolumetricLightmap = 0,  // Per-Pixel GI (highest quality)
    GlobalIBL = 1,           // Skybox Irradiance (fallback)
    None = 2                 // Disabled
};
```

---

## Rendering Pipeline

### Architecture Overview

**设计理念**: 借鉴 Unreal Engine 的 RenderPipeline + ShowFlags 模式，实现职责分离和灵活的多场景渲染。

**核心组件**:
```
CRenderPipeline (抽象基类)
└── CForwardRenderPipeline (Forward渲染实现)
    ├── CShadowPass (阴影渲染，条件性)
    ├── CSceneRenderer (核心场景渲染: Opaque + Transparent + Skybox)
    ├── CPostProcessPass (HDR → LDR Tone Mapping)
    ├── CDebugLinePass (调试线框，条件性)
    └── CGridPass (编辑器网格，条件性)
```

### Rendering Order

**CForwardRenderPipeline 渲染流程**:
```
1. Shadow Pass (if showFlags.Shadows)
   └─ Render to Shadow Map (Texture2DArray, 4 cascades)

2. Scene Rendering → HDR RT (R16G16B16A16_FLOAT)
   ├─ Opaque Pass (depth write, depth test)
   ├─ Skybox (depth = 1.0)
   └─ Transparent Pass (depth test only, alpha blend)

3. Post-Processing → LDR RT (R8G8B8A8_UNORM_SRGB)
   └─ Tone Mapping (ACES/Reinhard/Uncharted2)

4. Debug Lines (if showFlags.DebugLines) → LDR RT
   └─ Physics debug, AABB, Gizmos (uses HDR depth buffer)

5. Grid (if showFlags.Grid) → LDR RT
   └─ Editor grid overlay (uses HDR depth buffer)
```

### ShowFlags System

**FShowFlags** 控制哪些功能被渲染（借鉴 UE4 设计）:

```cpp
struct FShowFlags {
    // Game Features
    bool Lighting = true;
    bool Shadows = true;
    bool IBL = true;
    bool Skybox = true;
    bool OpaqueObjects = true;
    bool TransparentObjects = true;
    bool PostProcessing = true;

    // Editor Tools
    bool Grid = false;
    bool DebugLines = false;
    bool Gizmos = false;

    // Debug Visualization
    bool Wireframe = false;
    bool ShowCascades = false;
    bool ShowClusters = false;
};
```

**Presets**:
- `FShowFlags::Editor()` - 完整编辑器功能（Grid + DebugLines + 所有游戏功能）
- `FShowFlags::Game()` - 纯游戏渲染（无编辑器工具）
- `FShowFlags::Preview()` - 材质预览（无阴影、无后处理）
- `FShowFlags::ReflectionProbe()` - Reflection Probe 烘焙（场景 + IBL，无编辑器工具）

### RenderContext Pattern

**统一的渲染参数传递**:

```cpp
struct RenderContext {
    CCamera& camera;                     // 相机（视图、投影矩阵）
    CScene& scene;                       // 场景数据
    ID3D11RenderTargetView* outputRTV;   // 输出目标（可选）
    ID3D11DepthStencilView* outputDSV;   // 深度缓冲（可选）
    unsigned int width, height;          // 分辨率
    float deltaTime;                     // 时间增量
    FShowFlags showFlags;                // 渲染特性控制
};
```

**用法示例**:

```cpp
// Editor viewport rendering
CRenderPipeline::RenderContext ctx{
    editorCamera,               // Camera
    CScene::Instance(),         // Scene
    nullptr,                    // outputRTV (pipeline uses internal offscreen)
    nullptr,                    // outputDSV
    viewportWidth,              // width
    viewportHeight,             // height
    deltaTime,                  // deltaTime
    FShowFlags::Editor()        // showFlags (full editor features)
};
g_pipeline.Render(ctx);

// Material preview rendering
CRenderPipeline::RenderContext previewCtx{
    previewCamera, previewScene, previewRTV, previewDSV,
    256, 256, 0.0f,
    FShowFlags::Preview()  // No shadows, no post-processing
};
g_pipeline.Render(previewCtx);
```

### CSceneRenderer - Core Scene Rendering

**职责**: 纯粹的场景内容渲染（不包含编辑器工具、后处理）

**特点**:
- 输出到 HDR RT (R16G16B16A16_FLOAT)
- 可被多处复用：编辑器视口、Reflection Probe、截图、Material Preview
- 独立于 Pipeline，可单独调用

**渲染流程**:
```cpp
void CSceneRenderer::Render(
    const CCamera& camera,
    CScene& scene,
    ID3D11RenderTargetView* hdrRTV,
    ID3D11DepthStencilView* dsv,
    UINT w, UINT h, float dt,
    const CShadowPass::Output* shadowData  // 可选阴影数据
) {
    // 1. Setup clustered lighting (tile-based light culling)
    m_clusteredLighting.Setup(camera, scene, w, h);

    // 2. Collect render items (frustum culling, sorting)
    std::vector<RenderItem> items = collectRenderItems(camera, scene);

    // 3. Render opaque objects (depth write + test)
    renderOpaquePass(items, camera, shadowData);

    // 4. Render skybox (depth = 1.0)
    if (scene.GetLightSettings().HasSkybox())
        m_skybox.Render(...);

    // 5. Render transparent objects (depth test only, alpha blend)
    renderTransparentPass(items, camera, shadowData);
}
```

### Frame Flow Example

**完整的编辑器渲染流程**:

```cpp
// 1. Update editor camera
CCamera& editorCamera = CScene::Instance().GetEditorCamera();
editorCamera.aspectRatio = (float)vpW / (float)vpH;
CEditorContext::Instance().Update(dt, editorCamera);

// 2. Clear debug line buffer
g_pipeline.GetDebugLinePass().BeginFrame();

// 3. Collect debug geometry
CDebugRenderSystem::Instance().CollectAndRender(
    CScene::Instance(),
    g_pipeline.GetDebugLinePass()
);

// 4. Render using pipeline
CRenderPipeline::RenderContext ctx{
    editorCamera, CScene::Instance(),
    nullptr, nullptr, vpW, vpH, dt,
    FShowFlags::Editor()
};
g_pipeline.Render(ctx);

// 5. Display result in ImGui viewport
Panels::DrawViewport(
    CScene::Instance(), editorCamera,
    g_pipeline.GetOffscreenSRV(),
    g_pipeline.GetOffscreenWidth(),
    g_pipeline.GetOffscreenHeight(),
    &g_pipeline
);
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

## Material System

### Overview

**位置**:
- `Core/MaterialAsset.h/cpp` - 材质资产定义与序列化
- `Core/MaterialManager.h/cpp` - 材质加载与缓存管理
- `Editor/Panels_MaterialEditor.cpp` - 材质编辑器 UI

**架构**: 资产文件系统（.ffasset）+ 反射系统 + 编辑器集成

### Material Asset Format

**文件**: `assets/materials/*.ffasset` (JSON)

```json
{
  "type": "material",
  "version": "1.0",
  "albedo": [1.0, 1.0, 1.0],
  "metallic": 0.0,
  "roughness": 0.5,
  "emissive": [0.0, 0.0, 0.0],
  "emissiveStrength": 1.0,
  "albedoTexture": "textures/albedo.png",
  "normalMap": "textures/normal.png",
  "metallicRoughnessMap": "textures/metallic_roughness.png"
}
```

**PBR 参数**:
- `albedo`: 基础颜色 (RGB, [0-1], sRGB 空间)
- `metallic`: 金属度 (0=电介质, 1=金属)
- `roughness`: 粗糙度 (0=镜面, 1=完全漫反射)
- `emissive`: 自发光颜色 (RGB, [0-1], sRGB 空间)
- `emissiveStrength`: 自发光强度乘数 (HDR, >1 会 bloom)

**纹理路径** (相对于 `E:/forfun/assets/`):
- `albedoTexture`: Albedo 贴图 (sRGB)
- `normalMap`: 法线贴图 (Linear, glTF 格式)
- `metallicRoughnessMap`: 金属度/粗糙度贴图 (Linear, G=Roughness, B=Metallic)

### Material Loading & Caching

**CMaterialManager** 单例负责加载和缓存：

```cpp
// 加载材质（自动缓存）
CMaterialAsset* mat = CMaterialManager::Instance().Load("materials/metal.ffasset");

// 获取默认材质（白色 Albedo，中等粗糙度）
CMaterialAsset* defaultMat = CMaterialManager::Instance().GetDefault();
```

**缓存机制**:
- 使用路径作为 key 进行缓存
- 避免重复加载同一材质
- 内存由 MaterialManager 管理（智能指针）

### Material Assignment

**MeshRenderer 组件**:
```cpp
struct SMeshRenderer : public IComponent {
    std::string path;           // Mesh path (.obj/.gltf/.glb)
    std::string materialPath;   // Material asset path (.ffasset)
    // ...
};
```

**在 MainPass 中的使用**:
```cpp
// Engine/Rendering/MainPass.cpp:396-400
CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
if (!meshRenderer->materialPath.empty()) {
    material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
}
```

### Shader Integration

**常量缓冲区** (`Shader/MainPass.ps.hlsl:35-41`):
```hlsl
cbuffer CB_Object : register(b1) {
    float4x4 gWorld;
    float3 gMatAlbedo; float gMatMetallic;
    float gMatRoughness;
    int gHasMetallicRoughnessTexture;  // 1 = use texture, 0 = use CB values
    float2 _padObj;
}
```

**纹理绑定** (`Engine/Rendering/MainPass.cpp:408-414`):
```cpp
// 加载材质纹理（如果路径为空则使用默认纹理）
ID3D11ShaderResourceView* albedoSRV = material->albedoTexture.empty() ?
    texMgr.GetDefaultWhite() : texMgr.Load(material->albedoTexture, /*srgb=*/true);
ID3D11ShaderResourceView* normalSRV = material->normalMap.empty() ?
    texMgr.GetDefaultNormal() : texMgr.Load(material->normalMap, /*srgb=*/false);
ID3D11ShaderResourceView* metallicRoughnessSRV = material->metallicRoughnessMap.empty() ?
    texMgr.GetDefaultWhite() : texMgr.Load(material->metallicRoughnessMap, /*srgb=*/false);
```

**像素着色器采样** (`Shader/MainPass.ps.hlsl:172-197`):
```hlsl
float3 albedoTex = gAlbedo.Sample(gSamp, i.uv).rgb;
float3 nTS = gNormal.Sample(gSamp, i.uv).xyz * 2.0 - 1.0;
nTS.y = -nTS.y;  // glTF Y-axis flip (OpenGL → DirectX)

// Material properties: texture or constant buffer values
float3 albedo = gMatAlbedo * albedoTex;
float metallic = gHasMetallicRoughnessTexture ? metallicTex : gMatMetallic;
float roughness = gHasMetallicRoughnessTexture ? roughnessTex : gMatRoughness;
```

### Material Editor

**位置**: `Editor/Panels_MaterialEditor.cpp`

**功能**:
- 实时编辑所有 PBR 参数
- 纹理路径浏览和加载
- 反射系统自动生成 UI（`VisitProperties`）
- 保存到 .ffasset 文件

**打开方式**:
1. Inspector 中点击 Material 字段的 Edit 按钮
2. 或通过 Window → Material Editor

**UI 控件**:
- Color3: Albedo, Emissive
- Slider: Metallic, Roughness, Emissive Strength
- FilePath: Albedo Texture, Normal Map, Metallic/Roughness Map

### Reflection System Integration

Material 使用反射系统实现自动序列化和 UI 生成：

```cpp
void CMaterialAsset::VisitProperties(IPropertyVisitor& visitor) {
    visitor.VisitFloat3("albedo", albedo);
    visitor.VisitFloat("metallic", metallic);
    visitor.VisitFloat("roughness", roughness);
    visitor.VisitFloat3("emissive", emissive);
    visitor.VisitFloat("emissiveStrength", emissiveStrength);

    const char* filter = "Image Files *.png;*.jpg;*.jpeg;*.tga;*.bmp All Files *.* ";
    visitor.VisitFilePath("albedoTexture", albedoTexture, filter);
    visitor.VisitFilePath("normalMap", normalMap, filter);
    visitor.VisitFilePath("metallicRoughnessMap", metallicRoughnessMap, filter);
}
```

**Visitor 实现**:
- `CJsonWriteVisitor`: 序列化到 JSON
- `CJsonReadVisitor`: 从 JSON 反序列化
- `CImGuiPropertyVisitor`: 自动生成 Inspector UI

### Default Textures

**TextureManager** 提供默认兜底纹理：

```cpp
// Core/TextureManager.cpp:80-100
m_defaultWhite  = MakeSolidSRV(255, 255, 255, 255, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);  // 白色 (sRGB)
m_defaultNormal = MakeSolidSRV(128, 128, 255, 255, DXGI_FORMAT_R8G8B8A8_UNORM);       // 法线 (0,0,1)
m_defaultBlack  = MakeSolidSRV(0,   0,   0,   255, DXGI_FORMAT_R8G8B8A8_UNORM);       // 黑色 (Linear)
```

**用途**:
- 当材质未指定纹理路径时使用
- 避免纹理缺失导致的渲染错误
- 默认白色纹理让材质参数完全生效（乘法中性元素）

### Best Practices

**材质命名**:
- 使用描述性名称：`metal_rusty.ffasset`, `wood_oak.ffasset`
- 按类型分组：`materials/metals/`, `materials/wood/`

**纹理路径**:
- 使用相对路径（相对于 `E:/forfun/assets/`）
- 示例：`"textures/metal/rusty_albedo.png"`

**参数调整**:
- Metallic: 0.0 (布料/木头/塑料) → 1.0 (金属)
- Roughness: 0.0 (镜面) → 1.0 (粗糙)
- Emissive Strength: >1 配合 Bloom 实现发光效果

**性能优化**:
- 共享材质：多个对象引用同一个 .ffasset
- 纹理复用：多个材质引用同一张纹理（TextureManager 自动缓存）

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


## Clustered Lighting & Point Lights

### Overview

**实现日期**: 2025-11-27 (Phase 2.1)

**架构**: Clustered Forward Shading（改进的 Forward+ 变种）
- **屏幕空间划分**: 32×32 像素 tiles
- **深度分层**: 16 个对数深度切片（Logarithmic Z）
- **内存布局**: Offset+Count 紧凑存储（Atomic operations）
- **光源剔除**: Sphere-AABB 相交测试（View Space）

**性能指标**:
- 支持 100+ 点光源 @ 1080p 60 FPS
- Cluster Grid 缓存（仅在投影参数变化时重建）
- Early exit 优化（背面剔除、距离剔除）

**Commit**: `679ae4f` - Implement Clustered Point Light System

---

### Point Light Component

**位置**: `Engine/Components/PointLight.h`

**组件定义**:
```cpp
struct SPointLight : public CComponent {
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};  // Linear RGB
    float intensity = 1.0f;                      // Luminous intensity (arbitrary units)
    float range = 10.0f;                         // Maximum light radius (for culling)

    const char* GetTypeName() const override { return "PointLight"; }
};
```

**参数说明**:
- `color`: 光源颜色（Linear RGB 空间）
- `intensity`: 光照强度（任意单位，通常 0.1-100）
- `range`: 光照半径，用于 culling（超出范围贡献 < 0.001）

**物理模型**:
- 距离衰减: `1 / distance²` (物理逆平方定律)
- 平滑过渡: UE4 风格衰减函数（见 `GetDistanceAttenuation`）
- BRDF: Cook-Torrance（与 Directional Light 相同公式）

**编辑器集成**:
- Inspector Panel: "Add Component" → "PointLight"
- 实时预览光照效果（通过 Viewport）
- 位置由 Transform 组件控制

---

### Clustered Shading Architecture

#### 1. Cluster Grid 划分

**配置** (`Engine/Rendering/ClusteredLightingPass.h:15-18`):
```cpp
namespace ClusteredConfig {
    constexpr uint32_t TILE_SIZE = 32;              // 32×32 像素 tiles
    constexpr uint32_t DEPTH_SLICES = 16;           // 16 个深度切片
    constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 100; // 每个 cluster 最多 100 个光源
}
```

**Cluster 总数**:
```
numClustersX = (screenWidth + 31) / 32
numClustersY = (screenHeight + 31) / 32
numClustersZ = 16
totalClusters = numClustersX × numClustersY × numClustersZ
```

**例**: 1920×1080 → 60×34×16 = **32,640 clusters**

#### 2. 对数深度分层

**为什么用对数分层？**
- 近距离场景细节多，需要更高精度
- 远距离可以粗略划分（透视投影的特性）

**公式** (`Shader/ClusteredLighting.compute.hlsl:52-56`):
```hlsl
float GetDepthFromSlice(uint sliceIdx) {
    float t = (float)sliceIdx / (float)DEPTH_SLICES;
    // Logarithmic interpolation: Z = near * (far/near)^t
    return g_nearZ * pow(g_farZ / g_nearZ, t);
}
```

**示例**（nearZ=0.1, farZ=100）:
```
Slice 0:  0.1m  (near plane)
Slice 4:  0.5m
Slice 8:  3.2m
Slice 12: 18.0m
Slice 16: 100.0m (far plane)
```

#### 3. 内存布局

**三大数据结构**:

| Buffer | 类型 | 大小 | 用途 |
|--------|------|------|------|
| **ClusterAABB** | `StructuredBuffer<ClusterAABB>` | 32,640 clusters × 32 bytes | 每个 cluster 的 View Space AABB |
| **ClusterData** | `StructuredBuffer<ClusterData>` | 32,640 clusters × 8 bytes | Offset + Count（光源索引列表起始位置） |
| **CompactLightList** | `StructuredBuffer<uint>` | Dynamic（最多 3.26M indices） | 紧凑存储的光源索引 |

**ClusterData 结构** (`Shader/ClusteredLighting.compute.hlsl:20-23`):
```hlsl
struct ClusterData {
    uint offset;  // Offset in compact light index list
    uint count;   // Number of lights in this cluster
};
```

**内存优化**:
- **Compact Storage**: 只存储有光源的 cluster 的索引
- **Atomic Allocation**: 使用 `InterlockedAdd` 动态分配 offset
- **内存节省**: 相比 Fixed Grid（32,640 × 100 × 4 bytes = 12.5 MB），Compact 方式只需 ~500 KB

---

### Compute Shader Pipeline

#### Stage 1: Build Cluster Grid

**Shader**: `Shader/ClusteredLighting.compute.hlsl:74-136` (`CSBuildClusterGrid`)

**输入**:
- `ClusterCB`: 投影矩阵逆、近远平面、屏幕尺寸

**输出**:
- `g_clusterAABBs`: 每个 cluster 的 View Space AABB

**算法**:
```
1. 计算 tile 的屏幕空间范围（像素坐标）
2. 转换到 NDC 空间（[-1, 1]）
3. 获取深度范围（对数分层）
4. Unproject 8 个角点到 View Space
5. 计算 AABB（取最小/最大坐标）
```

**优化**: Grid 仅在投影参数变化时重建（`m_clusterGridDirty` 标志）。

#### Stage 2: Cull Lights

**Shader**: `Shader/ClusteredLighting.compute.hlsl:167-224` (`CSCullLights`)

**算法**: Sphere-AABB 相交测试 + Atomic allocation

```hlsl
bool SphereIntersectsAABB(float3 sphereCenter, float sphereRadius,
                          float3 aabbMin, float3 aabbMax) {
    // 找到 AABB 上最接近球心的点
    float3 closestPoint = clamp(sphereCenter, aabbMin, aabbMax);
    // 判断距离是否 <= 半径
    float distanceSquared = dot(closestPoint - sphereCenter,
                               closestPoint - sphereCenter);
    return distanceSquared <= (sphereRadius * sphereRadius);
}
```

**调度参数** (`ClusteredLightingPass.cpp:295-299`):
```cpp
uint32_t dispatchX = (m_numClustersX + 7) / 8;  // 8×8 threads per group
uint32_t dispatchY = (m_numClustersY + 7) / 8;
uint32_t dispatchZ = ClusteredConfig::DEPTH_SLICES;  // Critical: all 16 slices
ctx->Dispatch(dispatchX, dispatchY, dispatchZ);
```

---

### Shader Integration (MainPass)

#### Point Light BRDF 计算

**位置**: `Shader/Common.hlsl:75-135` (`CalculatePointLightPBR`)

**完整公式**:
```hlsl
float3 CalculatePointLightPBR(PointLightInput light, ...) {
    // 1. 距离衰减（UE4 风格 - 物理正确 + 平滑过渡）
    float attenuation = GetDistanceAttenuation(unnormalizedL, invRadius);

    // 2. Early exit 优化
    if (NdotL <= 0.0) return 0.0;           // Backface (30% perf gain)
    if (attenuation < 0.001) return 0.0;    // Out of range

    // 3. Cook-Torrance BRDF（与 Directional Light 完全相同）
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);
    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // 4. 能量守恒
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // 5. 最终光照
    float3 radiance = light.color * light.intensity * attenuation;
    return (diffuse + specular) * radiance * NdotL;
}
```

**物理正确性**:
- ✅ 逆平方衰减（`1/distance²`）
- ✅ 能量守恒（`kS + kD ≤ 1.0`）
- ✅ 完整的 Cook-Torrance BRDF
- ✅ 与 Directional Light 相同的公式（复用 `Common.hlsl`）

**UE4 距离衰减函数** (`Common.hlsl:51-59`):
```hlsl
float GetDistanceAttenuation(float3 unnormalizedL, float invRadius) {
    float distanceSquared = dot(unnormalizedL, unnormalizedL);
    float attenuation = 1.0 / max(distanceSquared, 0.01*0.01);

    // Smooth falloff at edge (UE4 windowing function)
    float factor = distanceSquared * invRadius * invRadius;
    float smoothFactor = saturate(1.0 - factor * factor);
    return attenuation * (smoothFactor * smoothFactor);
}
```

#### Pixel Shader 使用

**数据绑定** (`Shader/ClusteredShading.hlsl:27-39`):
```hlsl
StructuredBuffer<ClusterData> g_clusterData : register(t10);
StructuredBuffer<uint> g_compactLightList : register(t11);
StructuredBuffer<GpuPointLight> g_pointLights : register(t12);

cbuffer ClusteredParams : register(b3) {
    float g_clusterNearZ;
    float g_clusterFarZ;
    uint g_clusterNumX;
    uint g_clusterNumY;
    uint g_clusterNumZ;
};
```

**使用示例** (`MainPass.ps.hlsl`):
```hlsl
float3 pointLightContrib = ApplyClusteredPointLights(
    i.position.xy,  // Screen position (pixels)
    viewZ,          // View-space depth
    worldPos, N, V, albedo, metallic, roughness
);

float3 finalColor = directionalLight + pointLightContrib + IBL + emissive;
```

---

### Performance Optimizations

#### 1. Cluster Grid 缓存

**问题**: 每帧重建 32,640 clusters 非常昂贵

**解决方案**: 仅在投影参数变化时重建
```cpp
bool projectionChanged = (
    fabs(m_cachedFovY - fovY) > 0.001f ||
    fabs(m_cachedNear - nearZ) > 0.001f ||
    fabs(m_cachedFar - farZ) > 0.001f
);
```

**收益**: 静态相机场景节省 **~0.1-0.5ms/帧**

#### 2. Early Exit 优化

**Backface + Distance Culling**:
```hlsl
if (NdotL <= 0.0) return 0.0;           // Backface
if (attenuation < 0.001) return 0.0;    // Out of range
```

**收益**: 约 **30-50%** 像素提前退出

#### 3. Compact Light List

**内存对比**:
- Fixed Grid: **12.5 MB** (32,640 × 100 × 4 bytes)
- Compact List: **~500 KB** (使用 `InterlockedAdd` 动态分配)

---

### Debug UI

**位置**: `Editor/Panels_SceneLightSettings.cpp`

**功能**:
1. **Light Count Heatmap**: 蓝色（0 光源）→ 红色（100+ 光源）
2. **Cluster AABB**: 渲染 cluster 包围盒线框
3. **None**: 关闭调试

**访问**: Window → Scene Light Settings → Clustered Lighting Debug

---

### Testing

**TestClusteredLighting** (`Tests/TestClusteredLighting.cpp`):
- 8 个彩色点光源（红、绿、蓝、黄、青、品红、白、橙）
- 15 个立方体（3×5 网格）
- 夜晚场景（无 Directional Light）

**预期效果**:
- 颜色混合正确（红 + 绿 = 黄）
- 平滑衰减到 range 边界
- 远处立方体接近黑色

**TestSimplePointLight** (`Tests/TestSimplePointLight.cpp`):
- 1 个强白光（intensity=50, range=20）
- 1 个立方体
- 预期：完全照亮 + PBR 高光清晰

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

## Reflection Probe System

### Overview

**实现日期**: 2025-12-01 (Phase 3.1)

**架构**: TextureCubeArray-based Reflection Probes
- **存储方式**: `TextureCubeArray` (所有 probe 打包到一个纹理数组)
- **索引选择**: Per-Object (每个物体选择最近的 probe，避免像素级接缝)
- **支持数量**: 最多 8 个 probes (1 global + 7 local)
- **烘焙流程**: Editor-time baking (Inspector "Bake Now" 按钮)

**设计理念**:
- **Per-Object Selection**: 不像 per-pixel selection 会产生接缝，per-object 在整个物体上使用同一个 probe
- **Global Fallback**: Index 0 始终是全局 IBL，物体不在任何 local probe 范围内时使用
- **Lazy Loading**: Probe 数据仅在场景加载或手动 bake 时加载

---

### Component & Asset

#### SReflectionProbe Component

**位置**: `Engine/Components/ReflectionProbe.h`

```cpp
struct SReflectionProbe : public IComponent {
    std::string assetPath;      // .ffasset 文件路径 (normalized relative)
    float radius = 10.0f;       // Probe 影响半径
    int resolution = 128;       // Cubemap 分辨率 (烘焙时使用)
    bool isDirty = true;        // 是否需要重新烘焙

    const char* GetTypeName() const override { return "ReflectionProbe"; }
};
```

**参数说明**:
- `assetPath`: 烘焙输出的 .ffasset 文件路径
- `radius`: 物体中心在此半径内才使用该 probe
- `resolution`: 烘焙时的 cubemap 分辨率 (128/256/512)
- `isDirty`: 标记是否需要重新烘焙（位置变化后设为 true）

#### Reflection Probe Asset (.ffasset)

**位置**: `assets/probes/{probe_name}.ffasset`

```json
{
  "type": "reflection_probe",
  "version": "1.0",
  "resolution": 128,
  "environment": "env.ktx2",
  "irradiance": "irradiance.ktx2",
  "prefiltered": "prefiltered.ktx2"
}
```

**目录结构**:
```
assets/probes/kitchen_probe/
├── kitchen_probe.ffasset    # Asset manifest
├── env.ktx2                 # Environment cubemap (128×128)
├── irradiance.ktx2          # Diffuse irradiance (32×32)
└── prefiltered.ktx2         # Specular prefiltered (128×128, 7 mips)
```

---

### Architecture

#### CReflectionProbeManager

**位置**: `Engine/Rendering/ReflectionProbeManager.h/cpp`

**职责**:
- 管理 `TextureCubeArray` 资源 (irradiance + prefiltered)
- 加载/卸载 probe 数据
- 提供 probe 选择算法
- 绑定到 shader

**核心数据结构**:

```cpp
class CReflectionProbeManager {
    // TextureCubeArray: 所有 probes 打包到一个纹理
    ComPtr<ID3D11Texture2D> m_irradianceArray;      // 32×32, 1 mip, 8 slices
    ComPtr<ID3D11Texture2D> m_prefilteredArray;     // 128×128, 7 mips, 8 slices

    // Constant Buffer: Probe 位置和半径
    struct CB_Probes {
        struct ProbeData {
            XMFLOAT3 position;
            float radius;
        } probes[MAX_PROBES];
        int probeCount;
    } m_probeData;

    // Public API
    void LoadProbesFromScene(CScene& scene, ...);
    int SelectProbeForPosition(const XMFLOAT3& worldPos) const;
    void Bind(ID3D11DeviceContext* context);
};
```

**Probe 索引约定**:
| Index | 用途 | 半径 |
|-------|------|------|
| 0 | Global IBL (skybox) | ∞ (1e10) |
| 1-7 | Local probes | 用户定义 |

#### CReflectionProbeBaker

**位置**: `Engine/Rendering/ReflectionProbeBaker.h/cpp`

**职责**:
- 从指定位置渲染 6 个 cubemap 面
- 生成 IBL maps (irradiance + prefiltered)
- 导出到 KTX2 + .ffasset

**烘焙流程**:

```cpp
bool CReflectionProbeBaker::BakeProbe(
    const XMFLOAT3& position,    // Probe 位置
    int resolution,              // Cubemap 分辨率
    CScene& scene,               // 要渲染的场景
    const std::string& outputAssetPath  // 输出 .ffasset 路径
) {
    // 1. 创建 Cubemap render target
    createCubemapRenderTarget(resolution);

    // 2. 渲染 6 个面
    for (int face = 0; face < 6; ++face) {
        SetupCameraForCubemapFace(camera, face, position);
        m_pipeline->Render(renderCtx);  // 使用 FShowFlags::ReflectionProbe()
    }

    // 3. 生成 IBL maps
    m_iblGenerator->GenerateIrradianceMap(envCubemap, 32);
    m_iblGenerator->GeneratePreFilteredMap(envCubemap, 128, 7);

    // 4. 导出 KTX2 文件
    CKTXExporter::ExportCubemapToKTX2(envCubemap, envPath, 0);
    CKTXExporter::ExportCubemapToKTX2(irradianceTexture, irrPath, 1);
    CKTXExporter::ExportCubemapToKTX2(prefilteredTexture, prefPath, 7);

    // 5. 生成 .ffasset
    createAssetFile(outputAssetPath, resolution);
}
```

---

### Shader Integration

#### Per-Object Probe Selection

**CPU 端** (`Engine/Rendering/MainPass.cpp`):

```cpp
// 在渲染每个物体前，选择最近的 probe
int probeIndex = m_probeManager.SelectProbeForPosition(objectWorldPos);

// 传递给 shader
cbObject.probeIndex = probeIndex;
ctx->UpdateSubresource(m_cbObject, 0, nullptr, &cbObject, 0, 0);
```

**选择算法**:

```cpp
int CReflectionProbeManager::SelectProbeForPosition(const XMFLOAT3& worldPos) const {
    int bestIndex = 0;  // Default: global IBL
    float bestDistSq = 1e20f;

    // 搜索 local probes (index 1+)
    for (int i = 1; i < m_probeCount; i++) {
        float dx = worldPos.x - m_probeData.probes[i].position.x;
        float dy = worldPos.y - m_probeData.probes[i].position.y;
        float dz = worldPos.z - m_probeData.probes[i].position.z;
        float distSq = dx*dx + dy*dy + dz*dz;
        float radiusSq = m_probeData.probes[i].radius * m_probeData.probes[i].radius;

        // 在半径内且更近
        if (distSq < radiusSq && distSq < bestDistSq) {
            bestDistSq = distSq;
            bestIndex = i;
        }
    }

    return bestIndex;
}
```

#### Shader 采样

**数据绑定** (`MainPass.ps.hlsl`):

```hlsl
// t3: Irradiance CubeArray
TextureCubeArray<float4> g_irradianceArray : register(t3);

// t4: Prefiltered CubeArray
TextureCubeArray<float4> g_prefilteredArray : register(t4);

// Per-object constant buffer
cbuffer CB_Object : register(b1) {
    // ... other data ...
    int gProbeIndex;  // 当前物体使用的 probe 索引
};
```

**采样代码**:

```hlsl
// Diffuse IBL
float4 irradianceCoord = float4(N, gProbeIndex);
float3 irradiance = g_irradianceArray.Sample(gSamp, irradianceCoord).rgb;

// Specular IBL
float mipLevel = roughness * 6.0;  // 7 mips (0-6)
float4 prefilteredCoord = float4(R, gProbeIndex);
float3 prefiltered = g_prefilteredArray.SampleLevel(gSamp, prefilteredCoord, mipLevel).rgb;
```

---

### Editor Integration

#### Inspector Panel

**位置**: `Editor/InspectorPanel.cpp`

**ReflectionProbe 组件 UI**:
- `assetPath`: 文件路径选择器
- `radius`: 拖动滑块
- `resolution`: 下拉选择 (128/256/512)
- **"Bake Now" 按钮**: 立即烘焙 probe

**烘焙触发代码**:

```cpp
if (ImGui::Button("Bake Now", ImVec2(-1, 0))) {
    static CReflectionProbeBaker baker;
    if (!baker.Initialize()) {
        CFFLog::Error("Failed to initialize baker");
    } else {
        bool success = baker.BakeProbe(
            transform->position,
            probeComp->resolution,
            scene,
            probeComp->assetPath
        );
        if (success) {
            probeComp->isDirty = false;
        }
    }
}
```

#### Scene Light Settings Panel

**Global IBL 配置**:
- Skybox asset path (.ffasset)
- 修改后自动调用 `CScene::ReloadEnvironment()`

---

### Scene Lifecycle

#### 初始化流程

```cpp
// 1. CScene::Initialize() - 仅创建 GPU 资源
m_probeManager.Initialize();  // 创建 TextureCubeArray

// 2. CScene::LoadFromFile() - 加载场景数据
CSceneSerializer::LoadScene(scenePath, *this);

// 3. CScene::ReloadProbesFromScene() - 加载 probe 数据
m_probeManager.LoadProbesFromScene(*this, globalIrrPath, globalPrefPath);
```

#### 运行时更新

```cpp
// 切换 skybox 后更新全局 IBL
CScene::ReloadEnvironment(newSkyboxAssetPath);

// 重新加载所有 probes（场景结构变化后）
CScene::ReloadProbesFromScene();
```

---

### Performance Considerations

**内存占用**:
| 资源 | 分辨率 | Mips | Probes | 大小 |
|------|--------|------|--------|------|
| Irradiance Array | 32×32 | 1 | 8 | ~300 KB |
| Prefiltered Array | 128×128 | 7 | 8 | ~16 MB |

**GPU 开销**:
- **Per-Object Selection**: 无额外 GPU 开销（CPU 选择，GPU 只采样一次）
- **TextureCubeArray**: 与单个 Cubemap 采样相同开销

**优化建议**:
- 对于大场景，考虑空间划分（Octree）加速 probe 查找
- 静态场景可预计算 object-probe 映射，避免每帧查询

---

### Known Limitations

1. **无 Probe 混合**: 当前每个物体只使用一个 probe，不支持多 probe 混合
   - **影响**: probe 边界可能有跳变
   - **未来计划**: 实现基于距离的 2-probe 混合

2. **无实时更新**: Probe 必须在编辑器中手动烘焙
   - **影响**: 动态场景反射不会更新
   - **未来计划**: 支持运行时 probe 更新（低分辨率、按需）

3. **固定最大数量**: 最多 8 个 probes
   - **影响**: 大场景可能不够用
   - **未来计划**: 可配置数量，或分区域加载

---

## GPU Debug Events (RHI)

### Overview

**位置**: `RHI/ICommandList.h`

GPU 调试事件用于在 RenderDoc/PIX 等 GPU 分析工具中标记渲染阶段。

### API

```cpp
// 接口
class ICommandList {
    virtual void BeginEvent(const wchar_t* name) = 0;
    virtual void EndEvent() = 0;
};

// RAII Wrapper
class RHI::CScopedDebugEvent {
    CScopedDebugEvent(ICommandList* cmdList, const wchar_t* name);
    ~CScopedDebugEvent();  // 自动调用 EndEvent()
};
```

### 使用示例

```cpp
// 在 ForwardRenderPipeline.cpp 中
{
    RHI::CScopedDebugEvent evt(cmdList, L"Shadow Pass");
    m_shadowPass.Render(...);
}  // 自动结束事件

{
    RHI::CScopedDebugEvent evt(cmdList, L"Scene Rendering");
    m_sceneRenderer.Render(...);
}
```

### RenderDoc 中的显示

在 RenderDoc 的 Event Browser 中会看到层级结构：
```
- forward pipeline
  - Shadow Pass
  - Scene Rendering
    - Opaque Pass
    - Skybox
    - Transparent Pass
  - Post-Processing
  - Debug Lines
  - Grid
- imgui pass
```

### DX11 实现

使用 `ID3DUserDefinedAnnotation` 接口：
- `CDX11CommandList::BeginEvent()` → `m_annotation->BeginEvent(name)`
- `CDX11CommandList::EndEvent()` → `m_annotation->EndEvent()`

---

## IBL Debug UI

**位置**: `Editor/Panels_IrradianceDebug.cpp`

**功能**: 三个 Tab 实时预览 IBL 纹理

**RHI 集成**: 使用 `RHI::ITexture::GetSRVSlice(arrayIndex, mipLevel)` 获取 cubemap 单个面的 SRV 用于 ImGui 显示。SRV 按需创建并缓存。

### Tab 1: Environment Map
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

## Transparency & Alpha Test

### Current Implementation (Phase 1.2)

**Alpha Modes**: 材质系统支持 3 种透明模式（glTF 2.0 标准）：
```cpp
enum class EAlphaMode {
    Opaque = 0,  // 不透明（默认）
    Mask = 1,    // Alpha Test：二值透明（硬边）
    Blend = 2    // Alpha Blend：连续透明（混合）
};
```

**Alpha Test (Mask) 实现**:
- **Shader**: `MainPass.ps.hlsl` 使用 `discard` 指令丢弃 alpha < cutoff 的像素
- **Material 参数**: `alphaMode` (enum) + `alphaCutoff` (0.0-1.0, 默认 0.5)
- **应用场景**: 草叶、树叶、栅栏等需要硬边透明的物体

```hlsl
// MainPass.ps.hlsl: Alpha Test
if (gAlphaMode == 1 && alpha < gAlphaCutoff) {
    discard;  // 完全透明，不写入像素
}
```

**测试**: `TestAlphaTest` 使用草模型验证 alpha test 功能
- 材质: `alphaMode=Mask`, `alphaCutoff=0.5`
- 模型: `pbr_models/grass_medium/grass_medium_01_1k.gltf`
- 预期: 草叶有清晰的轮廓边缘，无黑色方块

### Known Issues & Future Work

**当前未实现的优化**（参考 Unity/Unreal 做法）:

| 问题 | Unity/UE 方案 | 我们的状态 |
|------|---------------|-----------|
| **Early-Z 失效** | Depth prepass (UE) / 默认不做 (Unity) | ❌ 未实现 |
| **抗锯齿** | TAA/MSAA (Unity), TAA/TSR (UE) | ❌ 未实现 |
| **阴影质量** | Shadow pass 自动处理 alpha test | ❌ 未实现 |

**问题详解**:

1. **Early-Z Optimization 失效**
   - **原因**: `discard` 指令阻止 Early-Z 优化，所有像素都运行 pixel shader
   - **性能影响**: Alpha test 物体的填充率成本高
   - **Unity 方案**: 默认不做 depth prepass，依赖 TAA 抗锯齿
   - **Unreal 方案**: 使用 depth prepass 恢复 Early-Z（性能换画质）
   - **我们的计划**: Phase 2 实现 depth prepass（可选）

2. **锯齿（Aliasing）**
   - **原因**: Binary decision (alpha < cutoff) 创建硬边，1 像素差异导致视觉跳变
   - **Unity 方案**: TAA (Temporal Anti-Aliasing) 或 MSAA
   - **Unreal 方案**: TAA (默认) 或 TSR (UE5)
   - **我们的计划**: Phase 2 实现 TAA

3. **Shadow Pass 中的 Alpha Test**
   - **原因**: 阴影贴图需要在 depth pass 中应用 alpha test，否则透明区域仍然投影
   - **Unity/UE 方案**: Shadow pass 自动绑定材质的 alpha test 参数
   - **我们的状态**: Shadow pass 当前不支持 alpha test
   - **我们的计划**: Phase 2 在 `ShadowPass.ps.hlsl` 中添加 alpha test

4. **Mipmap-Alpha 冲突**
   - **原因**: Mipmap 平均 alpha 值改变覆盖率（远处草变透明/消失）
   - **Unity/UE 方案**: 不自动解决，依赖美术调整 alpha 通道（alpha-to-coverage workflow）
   - **我们的计划**: 不解决（美术工作流问题）

5. **性能开销**
   - **Texture sampling**: 需要采样 albedo.a 通道
   - **Branch divergence**: `discard` 导致 warp 内分支
   - **Depth writes**: 无法使用 Z-only prepass
   - **我们的计划**: Phase 3 提供 profiling 工具

**Alpha Blend (未实现)**:
- 需要 depth sorting（画家算法）
- 需要 blend states 配置
- 计划在 Phase 2 实现


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

## Volumetric Lightmap System

**详细文档**: [docs/VOLUMETRIC_LIGHTMAP.md](VOLUMETRIC_LIGHTMAP.md)

UE4/5 风格的自适应八叉树 Volumetric Lightmap，用于高质量 Per-Pixel 漫反射全局光照。

**核心特性**:
- 自适应八叉树 Brick 系统
- 两级 GPU 查找 (Indirection → Atlas)
- L2 球谐编码 (9 SH coefficients)
- GPU DXR 批量烘焙 (64 voxels per dispatch)
- Overlap Baking 消除接缝

---

**Last Updated**: 2025-12-25
