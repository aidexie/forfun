# Development Roadmap

优先级可根据项目需求调整。

---

## 当前进度 (2025-11-23)

### ✅ 已完成
- **Transform Gizmo**: 平移/旋转/缩放，Local/World 切换，Grid snapping
- **HDR Export Tool**: HDR → KTX2 资源导出 (env/irr/prefilter)
- **KTX2 集成**: libktx 库，跨平台纹理格式
- **.ffasset 格式**: JSON 资源描述符
- **FFAsset Loader**: 加载 .ffasset 和 KTX2 纹理
- **鼠标拾取**: CPU射线投射选择物体（Ray-AABB相交测试）
- **地面参考网格**: Shader-based无限网格，Unity风格，双层级（1m+10m）
- **Debug渲染系统**: GPU几何着色器渲染AABB线框，深度测试

### 🔜 下一步
- **启动时缓存检测**: 检查 KTX2 是否比 HDR 源文件新
- **Asset Browser**: 浏览资源文件夹

---

## Phase 1: Editor Core Functionality

**目标**: 可用的场景编辑器

### 1.1 Transform Gizmo ✅
- ~~集成 ImGuizmo~~
- ~~平移/旋转/缩放模式~~
- ~~Local/World 空间切换~~
- ~~Grid snapping~~
- 多选支持 (待实现)

### 1.2 Viewport Interaction
- ~~鼠标拾取（射线投射选择物体）~~ ✅
- ~~地面参考网格~~ ✅
- ~~AABB Debug可视化~~ ✅
- 灯光范围/探针边界可视化 (待实现)

### 1.3 Asset Browser Panel
- 浏览 `E:/forfun/assets` 目录
- 拖放模型/纹理到场景
- 缩略图预览
- 文件类型过滤

**验收标准**: 5分钟内创建10+物体场景，无需手动输入坐标。

---

## Phase 2: Lighting System Extension

**目标**: 支持多种动态光源

### 2.1 Point Light
```cpp
struct SPointLight : public Component {
    XMFLOAT3 color{1,1,1};
    float intensity = 1.0f;
    float range = 10.0f;
    bool cast_shadows = false;
};
```
- Forward 渲染 (8-16灯光)
- 物理衰减 (inverse square law)

### 2.2 Spot Light
- 内/外锥角、范围
- 单张阴影贴图 (1024×1024)
- Cookie 纹理 (可选)

### 2.3 Reflection Probe
```cpp
struct SReflectionProbe : public Component {
    XMFLOAT3 box_min{-5,-5,-5}, box_max{5,5,5};
    int resolution = 256;
    bool is_box_projection = true;
    std::string baked_path;
};
```
- 编辑器 Bake 按钮
- Box Projection 修正
- 运行时采样替换全局 IBL

### 2.4 Light Probe (可选)
- 局部漫反射 IBL
- 球谐系数 (9 coefficients)

**验收标准**: 室内场景反射显示周围几何体而非天空。

---

## Phase 3: Animation + Advanced Rendering

可并行开发。

### 3A. Skeletal Animation

**数据结构**:
```cpp
struct SJoint {
    std::string name;
    int parent_index;
    XMFLOAT4X4 inverse_bind_matrix;
    XMFLOAT4X4 local_transform;
};

struct SSkeleton {
    std::vector<SJoint> joints;
    std::vector<XMFLOAT4X4> global_transforms;
};

struct SAnimationClip {
    std::string name;
    float duration;
    std::vector<Channel> channels;
};
```

**实现步骤**:
1. 扩展 `CGltfLoader` 解析 glTF skins/animations
2. Skin/Animator 组件
3. 蒙皮着色器 (CB_Skin, jointIndices, jointWeights)
4. 播放控制 + 动画混合

**验收标准**: 角色模型播放行走动画 60FPS。

### 3B. Forward+ Rendering

**Light Culling Compute Shader**:
```hlsl
StructuredBuffer<PointLight> gAllLights : register(t8);
RWStructuredBuffer<uint> gLightIndexList : register(u0);
RWStructuredBuffer<uint2> gTileLightIndices : register(u1);
```

**算法**:
1. 屏幕划分 16×16 tiles
2. 每 tile 构建视锥体
3. 测试光源球与视锥体相交
4. 写入可见光源索引

**验收标准**: 100+ 动态点光源 60FPS (1080p)。

---

## Phase 4: Post-Processing Stack

### 4.1 基础效果
- **Bloom**: 亮度提取 → 高斯模糊 → 叠加
- **Tonemapping**: ACES Filmic (推荐)
- **Color Grading**: 曝光、对比度、饱和度

### 4.2 高级效果
- **SSAO**: Horizon-based 或 GTAO
- **SSR**: 屏幕空间反射 (可选)
- **TAA**: 时域抗锯齿

### 4.3 Post-Process Volume
```cpp
struct SPostProcessVolume : public Component {
    XMFLOAT3 box_min, box_max;
    float priority;
    bool is_global;
    float bloom_intensity;
    float exposure;
};
```

**验收标准**: Bloom + ACES 达到接近 Unity/UE 视觉质量。

---

## Phase 5: Material System Enhancement

### 5.1 Material Asset
```json
{
  "name": "Gold",
  "albedo_color": [1.0, 0.86, 0.57],
  "albedo_texture": "textures/gold_albedo.png",
  "metallic": 1.0,
  "roughness": 0.3,
  "normal_map": "textures/gold_normal.png"
}
```
- 热重载

### 5.2 Material Editor Panel
- 实时预览球/立方体
- 纹理拖放
- 颜色选择器
- 预设保存/加载

### 5.3 扩展 PBR 输入
- Emissive Map
- Height Map (视差遮蔽)
- Detail Maps
- Clear Coat
- Anisotropy

**验收标准**: 编辑器内创建和预览材质，无需重启。

---

## Technical Recommendations

### Animation
推荐手动实现 (cgltf)，可选 Ozz-Animation。

### Rendering
推荐 Forward+ (透明物体友好，MSAA简单)。仅 1000+ 光源时考虑 Deferred。

### Post-Processing Priority
- Must-Have: Bloom + Tonemapping (ACES)
- High Value: SSAO, TAA
- Optional: SSR, LUT, DoF

---

## References

### Reflection Probes
- [Unity Reflection Probes](https://docs.unity3d.com/Manual/ReflectionProbes.html)
- "Local Image-based Lighting With Parallax-corrected Cubemap" (Lagarde & Zanuttini)

### Forward+
- "Forward+: Bringing Deferred Lighting to the Next Level" (Harada et al., Eurographics 2012)
- [AMD Forward+ Rendering](https://gpuopen.com/learn/lighting/forward-plus/)

### Animation
- [glTF 2.0 Skins](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#skins)
- "GPU Gems 1 - Chapter 4: Skinning"

### Post-Processing
- [ACES](https://github.com/ampas/aces-dev)
- "Next Generation Post Processing in Call of Duty: Advanced Warfare" (Jimenez, SIGGRAPH 2014)
