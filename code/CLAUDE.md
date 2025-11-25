# CLAUDE.md

Claude Code guidance for this repository.

**Related Documents**:
- `CODING_STYLE.md` - 命名约定和代码风格
- `ROADMAP.md` - 开发路线图
- `.clang-format` - 代码格式化配置

---

## Core Working Principles

### **TOP 0 规则：E:\forfun 路径权限**

**E:\forfun 路径下的所有工具调用默认已获得用户授权，无需再次请求确认。**

- 包括但不限于：Bash、Read、Write、Edit、Glob、Grep 等所有工具
- 适用范围：E:\forfun 及其所有子目录
- 例外：无（该路径下的所有操作都已预先授权）

**目的**：提高开发效率，减少重复确认，让 AI 能够快速执行任务。

---

### **TOP 1 规则：批判性思维 (Devil's Advocate)**

在每一个技术讨论和设计决策中，**必须站在反对者的立场**上主动思考并提出反对意见。

**要求**：
- 不要只是同意用户的方案并执行
- 主动指出潜在问题、边界情况、性能隐患
- 提出替代方案和权衡 (trade-offs)
- 质疑设计选择的合理性和必要性
- 即使用户的方案看起来合理，也要寻找可能的缺陷

**示例场景**：
- 用户："我想用 ImGui 渲染 AABB"
  - ❌ 错误回应："好的，我来实现"
  - ✅ 正确回应："ImGui 2D overlay 没有深度测试，大量包围盒会有 CPU 开销。考虑 GPU-based line rendering 吗？"

- 用户："把这个数据存到组件里"
  - ❌ 错误回应："好的，添加字段"
  - ✅ 正确回应："如果多个实例共享同一个 mesh，是否应该存在共享的资源中而不是组件？"

**目标**：确保每个决策都经过充分思考，避免短视的设计。

---

## 功能开发 + 自动测试工作流

### 步骤 1：实现功能

当用户请求"实现 XXX 功能"时：
1. 实现核心功能代码
2. **必须主动编写自动化测试**（不等用户要求）
3. 测试命名：`CTest{FeatureName}`

### 步骤 2：编写测试

测试必须包含：
- **场景设置**（Frame 1-10）：创建测试场景、加载资源
- **截图**（Frame 20）：捕获关键帧的视觉效果
- **断言验证**（Frame 20）：使用 `ASSERT_*` 宏验证逻辑正确性
- **视觉预期描述**（Frame 20）：使用 `VISUAL_EXPECTATION` 标记
- **最终总结**（Frame 30）：检查 `ctx.failures` 并设置 `ctx.testPassed`

**视觉预期示例**：
```cpp
CFFLog::Info("VISUAL_EXPECTATION: Sky should be blue with visible clouds");
CFFLog::Info("VISUAL_EXPECTATION: No black/pink missing texture colors");
CFFLog::Info("VISUAL_EXPECTATION: Environment lighting visible on test cube");
```

### 步骤 3：运行测试

使用 Bash tool 运行：
```bash
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test CTestXXX
```

### 步骤 4：AI 自动分析

测试运行完成后，**必须自动执行**以下步骤：

1. **读取测试日志**：
   ```
   E:/forfun/debug/{TestName}/test.log
   ```
   - 检查断言状态（查找 "✓ ALL ASSERTIONS PASSED" 或 "✗ TEST FAILED"）
   - 提取 VISUAL_EXPECTATION 描述
   - 记录任何失败的断言

2. **读取截图**：
   ```
   E:/forfun/debug/{TestName}/screenshot_frame*.png
   ```
   - 使用 Read tool 查看截图（AI 的多模态能力）
   - 对比截图与 VISUAL_EXPECTATION 描述
   - 检查明显的渲染错误（黑屏、缺失纹理、错误颜色）

3. **读取运行时日志**（如有必要）：
   ```
   E:/forfun/debug/{TestName}/runtime.log
   ```
   - 如果测试失败，查找错误信息
   - 检查资源加载问题

### 步骤 5：生成测试分析报告

**报告格式**：
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
测试分析报告：{TestName}
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

✅ 断言状态：所有断言通过 (0 failures)
   或
✗ 断言状态：3 个断言失败
   - [TestName:Frame10] Object count: expected 1, got 2
   - [TestName:Frame20] Hit distance: expected 10.400, got 11.200

✅ 视觉验证：截图符合预期
   - ✓ Sky shows blue color with clouds
   - ✓ No missing textures
   - ✓ Environment lighting visible
   或
✗ 视觉验证：发现问题
   - ✗ Screenshot shows black screen (expected: blue sky)

📊 日志摘要：
   - Frame 1: Scene setup complete
   - Frame 10: All setup assertions passed
   - Frame 20: Raycast test passed

📸 截图：
   - screenshot_frame20.png (1116x803)
   - 显示：[简要描述截图内容]

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ 总结：功能实现正确，测试通过
   或
✗ 总结：测试失败，需要修复以下问题：
   1. [具体问题描述]
   2. [具体问题描述]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### 步骤 6：失败时的处理

如果测试失败：
1. 分析失败原因（从日志和截图）
2. 修复代码
3. 重新构建
4. 返回步骤 3（重新运行测试）
5. 重复直到测试通过

### 重要提醒

- ⚠️ **不要跳过测试**：每个新功能都必须有自动化测试
- ⚠️ **不要等待用户要求**：主动编写并运行测试
- ⚠️ **不要忘记分析**：测试运行后必须读取并分析结果
- ⚠️ **不要只看断言**：视觉验证同样重要

---

## Project Overview

中型游戏引擎+编辑器项目，目标类似 Unity/Unreal 但规模较小。

**当前状态**:
- ECS 架构
- Editor UI (Hierarchy, Inspector, Viewport, Scene Light Settings, IBL Debug, HDR Export)
- 3D 渲染 (OBJ/glTF)
- PBR (Cook-Torrance BRDF)
- CSM 阴影 (bounding sphere stabilization + texel snapping)
- IBL (diffuse irradiance + specular pre-filtered map)
- 场景序列化 + 组件自动注册
- Transform Gizmo (ImGuizmo: 平移/旋转/缩放, Local/World, Grid snapping)
- HDR Export 工具 (HDR → KTX2 + .ffasset)
- KTX2 资源加载 (.ffasset → env/irr/prefilter)
- 自动化测试框架 (命令行驱动，帧回调架构)

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

## Coordinate System

**DirectX 左手坐标系**:
- **+X**: Right, **+Y**: Up, **+Z**: Forward (into screen)

**UV Convention**:
- 原点: 左上角 (0,0)
- U: 左→右, V: 上→下

所有矩阵操作使用 `LH` 后缀函数 (`XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`)。

---

## Build

Windows DX11 + CMake + Ninja:

```bash
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun
./build/forfun.exe
```

**Paths**:
- Source: `E:/forfun/source/code`
- Third-party: `E:/forfun/thirdparty`
- Assets: `E:/forfun/assets`

**Dependencies**: imgui_docking, cgltf, nlohmann/json, DirectX 11, KTX-Software (libktx)

---

## Architecture

### Three-Layer Separation

1. **Core/** - 底层设备管理、资源加载
   - `DX11Context`: D3D11 device/context/swapchain 单例
   - `MeshResourceManager`: Mesh 加载/缓存
   - `GpuMeshResource`: GPU mesh RAII 封装

2. **Engine/** - ECS、场景、渲染
   - `World`: GameObject 容器
   - `GameObject`: 拥有 Components 的实体
   - `Component`: 组件基类
   - `Scene`: World + 编辑器选择状态
   - `Rendering/`: MainPass, ShadowPass, Skybox, IBLGenerator

3. **Editor/** - ImGui UI
   - `Panels.h`: Panel 接口
   - 每个 Panel 独立 `.cpp` 文件

### Component System

**内置组件**: Transform, MeshRenderer, Material, DirectionalLight

**添加新组件**:
```cpp
// Engine/Components/PointLight.h
#pragma once
#include "Component.h"
#include "ComponentRegistry.h"

struct PointLight : public Component {
    DirectX::XMFLOAT3 Color{1, 1, 1};
    float Intensity = 1.0f;

    const char* GetTypeName() const override { return "PointLight"; }
    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat3("Color", Color);
        visitor.VisitFloat("Intensity", Intensity);
    }
};

REGISTER_COMPONENT(PointLight)
```

然后添加到 `CMakeLists.txt` ENGINE_SOURCES。

### Reflection & Serialization

- `PropertyVisitor`: 反射接口
- `ImGuiPropertyVisitor`: Inspector UI
- `JsonWriteVisitor`/`JsonReadVisitor`: JSON 序列化
- 场景文件: `.scene` (JSON)

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

- 1-4 cascades
- Bounding sphere stabilization (消除旋转抖动)
- Texel snapping (消除移动抖动)
- PCF 3×3 软阴影

**DirectionalLight 参数**:
- `ShadowMapSizeIndex`: 1024/2048/4096
- `ShadowDistance`, `ShadowBias`, `ShadowNearPlaneOffset`
- `CascadeCount`, `CascadeSplitLambda`, `CascadeBlendRange`
- `DebugShowCascades`

---

## IBL System

### Components

1. **IBLGenerator** (`Engine/Rendering/IBLGenerator.h/cpp`)
2. **IrradianceConvolution.ps.hlsl**: Diffuse irradiance (uniform solid angle sampling)
3. **PreFilterEnvironmentMap.ps.hlsl**: Specular (GGX importance sampling, Split Sum)

### Key Implementation

**Uniform Solid Angle Sampling** (irradiance):
```hlsl
float cosTheta = 1.0 - v;  // Linear in cos(θ) → uniform solid angle
irradiance += color * cosTheta;
```

**GGX Importance Sampling** (pre-filtered):
```hlsl
float2 Xi = Hammersley(i, SAMPLE_COUNT);
float3 H = ImportanceSampleGGX(Xi, roughness);
float3 L = normalize(2.0 * dot(V, H) * H - V);
```

**Dynamic Sample Count**: 8K-65K samples based on roughness.

**Solid Angle Mip Selection**:
```hlsl
float saSample = 1.0 / (pdf * SAMPLE_COUNT);
float saTexel = 4.0 * PI / (6.0 * envResolution * envResolution);
float mipLevel = 0.5 * log2(saSample / saTexel);
```

### Debug UI

`Editor/Panels_IrradianceDebug.cpp` - 三个 Tab:
1. Irradiance Map (32×32)
2. Pre-Filtered Map (128×128, mip 0-6)
3. Environment Map (source, mip 0-9)

---

## Editor Panels

**Current**: Dockspace, Hierarchy, Inspector, Viewport, IrradianceDebug, HDRExport

**Adding new panel**:
1. 声明到 `Editor/Panels.h`
2. 实现 `Editor/Panels_PanelName.cpp`
3. 添加到 `CMakeLists.txt` EDITOR_SRC
4. 在 main loop `ImGui::NewFrame()` 后调用

---

## HDR Export Tool

**位置**: `Editor/Panels_HDRExport.cpp`

**功能**: 将 HDR 环境贴图导出为 IBL 资源包

**导出流程**: Window → HDR Export
1. 选择 HDR 源文件
2. 输入输出目录和资源名
3. 点击 Export

**输出文件**:
- `{name}_env.ktx2` - 环境立方体贴图 (512×512)
- `{name}_irr.ktx2` - 漫反射辐照度图 (32×32)
- `{name}_prefilter.ktx2` - 镜面预过滤图 (128×128, 7 mip)
- `{name}.ffasset` - JSON 描述符

**.ffasset 格式**:
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

**下一步**: 启动时缓存检测

---

## Transform Gizmo

Viewport 工具栏提供物体变换控制。

**操作模式**:
- **Translate (W)**: 平移物体
- **Rotate (E)**: 旋转物体
- **Scale (R)**: 缩放物体

**空间模式**:
- **World**: 世界坐标系
- **Local**: 局部坐标系

**Grid Snapping**:
- 勾选 "Snap" 启用网格对齐
- 平移: 可调步进值 (0.01-10m)，默认 1m
- 旋转: 可调角度 (1-90°)，默认 15°
- 缩放: 可调步进值 (0.01-2)，默认 0.5

---

## View Orientation Gizmo

Viewport 右上角的相机方向指示器 (自定义 ImGui DrawList 渲染)。

**特性**:
- X/Y/Z 轴正向: 亮色 + 箭头 + 标签
- 负向: 灰色细线
- 深度排序渲染

---

## Automated Testing Framework

**目标**: 让 AI 能够自主验证新功能的正确性。

### Test Framework Architecture

**位置**: `Core/Testing/`
- `TestCase.h` - 测试基类和上下文
- `TestRegistry.h` - 自动注册宏
- `Tests/` - 测试用例

**运行测试**:
```bash
./build/forfun.exe --test TestRayCast
```

### Writing Tests

**示例测试**:
```cpp
// Tests/TestRayCast.cpp
#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Engine/Scene.h"

class CTestRayCast : public ITestCase {
public:
    const char* GetName() const override { return "TestRayCast"; }

    void Setup(CTestContext& ctx) override {
        // Frame 1: 创建测试场景
        ctx.OnFrame(1, [&]() {
            auto& scene = CScene::Instance();
            // 清空场景
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }

            // 创建测试物体
            auto* cube = scene.GetWorld().Create("TestCube");
            auto* transform = cube->AddComponent<STransform>();
            transform->position = {0.0f, 0.0f, 5.0f};
            auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
            meshRenderer->path = "mesh/cube.obj";

            CFFLog::Info("Frame 1: Creating test scene");
        });

        // Frame 20: 执行射线投射测试
        ctx.OnFrame(20, [&]() {
            // 执行测试逻辑
            CFFLog::Info("Frame 20: Performing raycast test");

            // 验证结果
            ctx.testPassed = true;
        });

        // Frame 30: 结束测试
        ctx.OnFrame(30, [&]() {
            CFFLog::Info("Frame 30: Test finished");
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestRayCast)
```

### Frame Callback Pattern

**核心概念**: 测试在正常引擎循环中执行，通过帧回调调度操作。

**好处**:
- 真实环境测试（与运行时行为一致）
- 可以测试异步资源加载
- 可以观察渲染结果（配合截图）
- 自动退出（不阻塞 CI）

**主循环集成**:
```cpp
// main.cpp
if (activeTest) {
    testContext.ExecuteFrame(frameCount);
    if (testContext.IsFinished()) {
        PostQuitMessage(testContext.testPassed ? 0 : 1);
        break;
    }
}
```

### Test Context API

```cpp
class CTestContext {
public:
    int currentFrame = 0;
    bool testPassed = false;

    // 注册帧回调
    void OnFrame(int frameNumber, std::function<void()> callback);

    // 标记测试完成
    void Finish();
    bool IsFinished() const;

    // 执行当前帧回调（由主循环调用）
    void ExecuteFrame(int frame);
};
```

### Scene Light Settings

**位置**: `Engine/SceneLightSettings.h`, `Editor/Panels_SceneLightSettings.cpp`

**功能**: 场景级别配置，独立于 GameObject 系统

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

### Next Steps (Phase 0 - 最高优先级)

详见 `ROADMAP.md` Phase 0。

**立即需要**:
1. **截图 API** - `CScreenshot::Capture(path)` / `CaptureTest(testName, frame)`
2. **状态查询** - `CScene::GenerateReport()`
3. **测试断言** - `CTestContext::Assert()` / `AssertEqual()`

**目标**: 让 AI 能够"看到"测试结果（截图），验证逻辑状态（报告），自动判断 pass/fail（断言）。

---

**Last Updated**: 2025-11-24
