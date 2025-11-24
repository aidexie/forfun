# Development Roadmap

**核心目标**: 构建完善的自动化测试体系，让 AI 能够自主验证新功能的正确性。

---

## 当前进度 (2025-11-24)

### ✅ 已完成

#### 自动化测试基础设施
- **测试框架**: 命令行驱动 (`--test TestName`)，帧回调架构，自动退出
  - `Core/Testing/TestCase.h` - 测试基类和上下文
  - `Core/Testing/TestRegistry.h` - 自动注册宏
  - `Tests/TestRayCast.cpp` - 示例测试用例
- **统一日志系统**: CFFLog 替代所有 console 输出，支持自定义控制台重定向
- **文件结构重组**: Core/Loader 和 Core/Exporter 分离，便于维护

#### 渲染和编辑器功能
- **场景光照设置**: Scene Light Settings 面板，支持天空盒配置和即时应用
- **IBL Debug 窗口**: 可视化 Irradiance/PreFilter/Environment 贴图，支持关闭和 Window 菜单打开
- **Transform Gizmo**: 平移/旋转/缩放，Local/World 切换，Grid snapping
- **HDR Export Tool**: HDR → KTX2 资源导出 (env/irr/prefilter)
- **鼠标拾取**: CPU 射线投射选择物体（Ray-AABB 相交测试）
- **地面参考网格**: Shader-based 无限网格，Unity 风格，双层级（1m+10m）
- **Debug 渲染系统**: GPU 几何着色器渲染 AABB 线框，深度测试
- **KTX2 集成**: libktx 库，跨平台纹理格式
- **.ffasset 格式**: JSON 资源描述符

#### Bug 修复
- **OFN_NOCHANGEDIR**: 修复文件对话框改变工作目录的严重 bug

---

## Phase 0: AI 自主测试基础设施 (最高优先级)

**目标**: 让 AI 能够"看到"、验证和报告测试结果，减少人工验证工作量。

**核心问题**: AI 无法直接观察 EXE 运行结果（视觉盲区、交互盲区、状态盲区）

### 0.1 视觉验证系统 ✅ (部分完成，待实现)

#### 截图 API
```cpp
// Core/Testing/Screenshot.h
class CScreenshot {
public:
    // 从 MainPass 离屏 RT 读取并保存为 PNG
    static bool Capture(const std::string& path);

    // 保存到测试目录: test_screenshots/{testname}_{frame}.png
    static bool CaptureTest(const std::string& testName, int frame);
};
```

**需求**:
- API 驱动（不依赖快捷键）
- 保存 tonemapped LDR 图像（与 Viewport 显示一致）
- 自动创建目录
- 日志输出保存路径
- 使用 stb_image_write（避免额外依赖）

**验收标准**:
```cpp
ctx.OnFrame(20, [&]() {
    scene.LoadSkybox("skybox/test.ffasset");
    CScreenshot::CaptureTest("TestSkybox", 20);
    // 输出: test_screenshots/TestSkybox_frame20.png
});
```

---

### 0.2 状态查询系统 (待实现)

#### Scene Report
```cpp
// Engine/Scene.h
class CScene {
public:
    std::string GenerateReport() const;
    // 输出:
    // - GameObject 数量和名称列表
    // - 当前选中对象索引
    // - 天空盒资源路径
    // - 光源信息
};
```

#### Rendering Statistics
```cpp
// Engine/Rendering/RenderStats.h
struct CRenderStats {
    int drawCalls = 0;
    int triangles = 0;
    float frameTimeMs = 0.0f;

    void Reset();
    std::string ToString() const;
};

// MainPass 中累加统计
CRenderStats::Instance().drawCalls++;
```

**验收标准**:
```cpp
ctx.OnFrame(30, [&]() {
    std::string report = CScene::Instance().GenerateReport();
    CFFLog::Info("Scene State:\n%s", report.c_str());
    // 输出:
    // GameObjects: 3
    // - [0] TestCube (selected)
    // - [1] Ground
    // - [2] DirectionalLight
    // Skybox: skybox/test.ffasset
});
```

---

### 0.3 测试断言系统 (待实现)

#### Assert API
```cpp
// Core/Testing/TestCase.h
class CTestContext {
public:
    void Assert(bool condition, const char* message);
    void AssertEqual(float a, float b, float epsilon = 0.01f, const char* msg = "");
    void AssertEqual(int a, int b, const char* msg = "");

    // 失败时记录错误，继续运行（汇总所有错误）
    std::vector<std::string> failures;
};
```

**验收标准**:
```cpp
ctx.OnFrame(50, [&]() {
    auto& scene = CScene::Instance();
    ctx.AssertEqual(scene.GetWorld().Count(), 3, "Expected 3 objects");
    ctx.AssertEqual(scene.GetSelectedIndex(), 0, "Expected object 0 selected");

    if (ctx.failures.empty()) {
        ctx.testPassed = true;
        CFFLog::Info("✓ All assertions passed");
    } else {
        ctx.testPassed = false;
        CFFLog::Error("✗ %d assertions failed", ctx.failures.size());
    }
    ctx.Finish();
});
```

---

### 0.4 测试快照系统 (待实现，低优先级)

#### Snapshot Definition
```cpp
// Core/Testing/TestSnapshot.h
struct CTestSnapshot {
    std::string scenePath;         // .scene 文件
    XMFLOAT3 cameraPos;
    XMFLOAT3 cameraTarget;
    std::string referenceImage;    // 基线截图
    std::string description;

    static CTestSnapshot Create(const std::string& name);
    static CTestSnapshot Load(const std::string& path);
    void Save(const std::string& path) const;
};
```

**用途**: 视觉回归测试（保存"已知正确"的场景+截图，测试时对比）

**工作流**:
1. 手动验证场景渲染正确
2. 创建快照: `CTestSnapshot::Create("gold_material_test")`
3. 测试时加载快照并对比截图（需要 Visual Diff 工具）

---

### 0.5 输入模拟系统 (待实现，中优先级)

#### Input Simulation
```cpp
// Core/Testing/InputSimulator.h
class CInputSimulator {
public:
    static void SimulateKey(int vkCode);
    static void SimulateMouseClick(int x, int y);
    static void SimulateMouseMove(int x, int y);
};
```

**用途**: 测试交互功能（鼠标拾取、快捷键响应）

**验收标准**:
```cpp
ctx.OnFrame(10, [&]() {
    // 模拟点击 Viewport 中心
    CInputSimulator::SimulateMouseClick(400, 300);
});

ctx.OnFrame(15, [&]() {
    // 验证物体被选中
    ctx.AssertEqual(scene.GetSelectedIndex(), 1, "Should select object at raycast hit");
});
```

---

### 0.6 性能 Profiling (待实现，中优先级)

#### Performance Report
```cpp
// Core/Profiler.h
struct CProfileReport {
    float avgFPS;
    float frameTimeMs;
    int drawCalls;
    int triangles;
};

class CProfiler {
public:
    static void BeginFrame();
    static void EndFrame();
    static CProfileReport GetReport();
};
```

**用途**: 验证性能回归（新功能不应显著降低 FPS）

---

## Phase 1: Editor Core Functionality

**目标**: 可用的场景编辑器

### 1.1 Transform Gizmo ✅
- ~~集成 ImGuizmo~~
- ~~平移/旋转/缩放模式~~
- ~~Local/World 空间切换~~
- ~~Grid snapping~~
- 多选支持 (待实现)

### 1.2 Viewport Interaction ✅
- ~~鼠标拾取（射线投射选择物体）~~
- ~~地面参考网格~~
- ~~AABB Debug 可视化~~
- 灯光范围/探针边界可视化 (待实现)

### 1.3 Asset Browser Panel (待实现)
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
    bool castShadows = false;
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
    XMFLOAT3 boxMin{-5,-5,-5}, boxMax{5,5,5};
    int resolution = 256;
    bool isBoxProjection = true;
    std::string bakedPath;
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
    int parentIndex;
    XMFLOAT4X4 inverseBindMatrix;
    XMFLOAT4X4 localTransform;
};

struct SSkeleton {
    std::vector<SJoint> joints;
    std::vector<XMFLOAT4X4> globalTransforms;
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
    XMFLOAT3 boxMin, boxMax;
    float priority;
    bool isGlobal;
    float bloomIntensity;
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
  "albedoColor": [1.0, 0.86, 0.57],
  "albedoTexture": "textures/gold_albedo.png",
  "metallic": 1.0,
  "roughness": 0.3,
  "normalMap": "textures/gold_normal.png"
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

### Testing Infrastructure
- **截图**: stb_image_write（PNG 8-bit）
- **Visual Diff**: Python + OpenCV 或 ImageMagick（外部工具）
- **日志**: 结构化输出，便于自动解析

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

### Testing
- [Snapshot Testing - Jest](https://jestjs.io/docs/snapshot-testing)
- [Visual Regression Testing](https://www.browserstack.com/guide/visual-regression-testing)

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
