# CLAUDE.md

Claude Code guidance for this repository.

**Related Documents**:
- `CODING_STYLE.md` - 命名约定和代码风格
- `ROADMAP.md` - 开发路线图
- `.clang-format` - 代码格式化配置
- `docs/RENDERING.md` - 渲染系统详细文档
- `docs/EDITOR.md` - 编辑器系统详细文档

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

**要求**:
- 不要只是同意用户的方案并执行
- 主动指出潜在问题、边界情况、性能隐患
- 提出替代方案和权衡 (trade-offs)
- 质疑设计选择的合理性和必要性
- 即使用户的方案看起来合理，也要寻找可能的缺陷

**示例场景**:
- 用户："我想用 ImGui 渲染 AABB"
  - ❌ 错误回应："好的，我来实现"
  - ✅ 正确回应："ImGui 2D overlay 没有深度测试，大量包围盒会有 CPU 开销。考虑 GPU-based line rendering 吗？"

- 用户："把这个数据存到组件里"
  - ❌ 错误回应:"好的，添加字段"
  - ✅ 正确回应:"如果多个实例共享同一个 mesh，是否应该存在共享的资源中而不是组件？"

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
- ECS 架构 (Component-based GameObject system)
- Editor UI (Hierarchy, Inspector, Viewport, Scene Light Settings, IBL Debug, HDR Export)
- 3D 渲染 (OBJ/glTF loader)
- PBR (Cook-Torrance BRDF, physically-based)
- CSM 阴影 (bounding sphere stabilization + texel snapping)
- IBL (diffuse irradiance + specular pre-filtered map)
- 场景序列化 + 组件自动注册
- Transform Gizmo (ImGuizmo: Translate/Rotate/Scale, Local/World, Grid snapping)
- HDR Export 工具 (HDR → KTX2 + .ffasset)
- KTX2 资源加载 (.ffasset → env/irr/prefilter)
- 自动化测试框架 (命令行驱动，帧回调架构)

---

## Coordinate System

**DirectX 左手坐标系**:
- **+X**: Right, **+Y**: Up, **+Z**: Forward (into screen)

**UV Convention**:
- 原点: 左上角 (0,0)
- U: 左→右, V: 上→下

所有矩阵操作使用 `LH` 后缀函数 (`XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`)。

---

## Build & Run

Windows DX11 + CMake + Ninja:

```bash
# Build
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun

# Run editor
./build/Debug/forfun.exe

# Run test
./build/Debug/forfun.exe --test TestRayCast
```

**Paths**:
- Source: `E:/forfun/source/code`
- Third-party: `E:/forfun/thirdparty`
- Assets: `E:/forfun/assets`
- Debug output: `E:/forfun/debug/{TestName}/`

**Dependencies**: imgui_docking, cgltf, nlohmann/json, DirectX 11, KTX-Software (libktx)

---

## Architecture

### Three-Layer Separation

1. **Core/** - 底层设备管理、资源加载
   - `DX11Context`: D3D11 device/context/swapchain 单例
   - `MeshResourceManager`: Mesh 加载/缓存
   - `GpuMeshResource`: GPU mesh RAII 封装
   - `Testing/`: 测试框架（TestCase, TestRegistry, Screenshot, Assertions）

2. **Engine/** - ECS、场景、渲染
   - `World`: GameObject 容器
   - `GameObject`: 拥有 Components 的实体
   - `Component`: 组件基类
   - `Scene`: World + 编辑器选择状态 + SceneLightSettings
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

struct SPointLight : public IComponent {
    DirectX::XMFLOAT3 Color{1, 1, 1};
    float Intensity = 1.0f;

    const char* GetTypeName() const override { return "PointLight"; }
    void VisitProperties(IPropertyVisitor& visitor) override {
        visitor.VisitFloat3("Color", Color);
        visitor.VisitFloat("Intensity", Intensity);
    }
};

REGISTER_COMPONENT(SPointLight)
```

然后添加到 `CMakeLists.txt` ENGINE_SOURCES。

**Reflection & Serialization**:
- `IPropertyVisitor`: 反射接口
- `CImGuiPropertyVisitor`: Inspector UI 自动生成
- `CJsonWriteVisitor`/`CJsonReadVisitor`: JSON 序列化
- 场景文件: `.scene` (JSON)

---

## Graphics Rendering (Quick Reference)

### Physics-Based Rendering Rule

**所有图形特性必须物理正确**。禁止非物理 hack（无依据的乘数、让阴影影响 IBL 等）。

详见 **`docs/RENDERING.md`** 获取：
- Energy Conservation 公式
- Shadow System (CSM) 实现细节
- IBL System (GGX importance sampling, solid angle mip selection)
- Rendering Pipeline 架构
- Color Space 规则

### 快速参考

**Color Space**:
- Albedo/Emissive: `UNORM_SRGB`
- Normal/Metallic/Roughness/AO: `UNORM`
- Intermediate RT: `R16G16B16A16_FLOAT`

**CSM Shadow**: 1-4 cascades, bounding sphere stabilization, texel snapping, PCF 3×3

**IBL**: Diffuse (32×32) + Specular pre-filtered (128×128, 7 mip)

---

## Editor System (Quick Reference)

详见 **`docs/EDITOR.md`** 获取：
- Panel 系统架构
- Transform Gizmo (W/E/R 快捷键, World/Local, Grid snapping)
- View Orientation Gizmo (相机方向指示器)
- HDR Export Tool (HDR → KTX2 + .ffasset 工作流)
- Irradiance Debug Panel (IBL 纹理预览)

### 快速参考

**添加新 Panel**:
1. 声明到 `Editor/Panels.h`
2. 实现 `Editor/Panels_PanelName.cpp`
3. 添加到 `CMakeLists.txt` EDITOR_SRC
4. 在 main loop 中调用

**当前 Panels**: Dockspace, Hierarchy, Inspector, Viewport, Scene Light Settings, Irradiance Debug, HDR Export

---

## Automated Testing

### Test Framework

**位置**: `Core/Testing/`
- `TestCase.h` - `ITestCase` 接口, `CTestContext` API
- `TestRegistry.h` - `REGISTER_TEST()` 宏
- `Screenshot.h` - `CScreenshot::CaptureTest()`
- `Tests/` - 测试用例

**运行测试**:
```bash
./build/Debug/forfun.exe --test TestRayCast
```

**输出**:
```
E:/forfun/debug/{TestName}/
  ├── runtime.log       (Frame-by-frame execution log)
  ├── test.log          (Test session log with assertions)
  └── screenshot_frame20.png
```

### Frame Callback Pattern

**核心概念**: 测试在正常引擎循环中执行，通过帧回调调度操作。

```cpp
class CTestMyFeature : public ITestCase {
public:
    const char* GetName() const override { return "TestMyFeature"; }

    void Setup(CTestContext& ctx) override {
        ctx.OnFrame(1, [&]() {
            // 创建测试场景
        });

        ctx.OnFrame(20, [&]() {
            // 执行测试 + 截图 + 断言
            CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);
            ASSERT_EQUAL(ctx, actual, expected, "Description");
        });

        ctx.OnFrame(30, [&]() {
            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestMyFeature)
```

### Assertion Macros

```cpp
ASSERT(ctx, condition, "Description");
ASSERT_EQUAL(ctx, actual, expected, "Description");
ASSERT_NOT_NULL(ctx, pointer, "Description");
ASSERT_IN_RANGE(ctx, value, min, max, "Description");
ASSERT_VEC3_EQUAL(ctx, actual, expected, epsilon, "Description");
```

**好处**: 测试失败不会崩溃，记录到 `ctx.failures`，最后统一判断 pass/fail。

---

## Documentation Index

### Core Documents (Root)
- `CLAUDE.md` (本文件) - AI 工作指南 + 快速参考
- `CODING_STYLE.md` - 命名约定（匈牙利命名法：C/S/I/E 前缀）
- `ROADMAP.md` - 开发路线图（Phase 0-3）
- `.clang-format` - 代码格式化配置

### Detailed References (docs/)
- `docs/RENDERING.md` - 渲染系统完整文档
  - Graphics Rendering Standards (Physics-based, Energy Conservation)
  - Rendering Pipeline (Frame Flow, Color Space)
  - Shadow System (CSM implementation details)
  - IBL System (Diffuse/Specular algorithms, Debug UI)
  - Scene Light Settings
  - KTX2 Asset Loading

- `docs/EDITOR.md` - 编辑器系统完整文档
  - Editor Architecture (Panel system)
  - Hierarchy/Inspector/Viewport Panels
  - Transform Gizmo (操作模式, Grid Snapping)
  - View Orientation Gizmo
  - Scene Light Settings Panel
  - Irradiance Debug Panel
  - HDR Export Tool (完整导出流程)
  - File Dialog Utilities

---

**Last Updated**: 2025-11-25
