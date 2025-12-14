# CLAUDE.md

Claude Code guidance for this repository.

**Related Documents**:
- `CODING_STYLE.md` - 命名约定和代码风格
- `ROADMAP.md` - 开发路线图
- `.clang-format` - 代码格式化配置
- `docs/RENDERING.md` - 渲染系统详细文档
- `docs/EDITOR.md` - 编辑器系统详细文档
- `docs/TESTING.md` - 自动化测试详细文档 ⭐

---
## CRITICAL: File Editing on Windows

### ⚠️ MANDATORY: Always Use Backslashes on Windows for File Paths

**When using Edit or MultiEdit tools on Windows, you MUST use backslashes (`\`) in file paths, NOT forward slashes (`/`).**

#### ❌ WRONG - Will cause errors:
```
Edit(file_path: "D:/repos/project/file.tsx", ...)
MultiEdit(file_path: "D:/repos/project/file.tsx", ...)
```

#### ✅ CORRECT - Always works:
```
Edit(file_path: "D:\repos\project\file.tsx", ...)
MultiEdit(file_path: "D:\repos\project\file.tsx", ...)
```

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

## Automated Testing (Quick Reference)

**详细文档**: 参见 **`docs/TESTING.md`** 获取完整测试指南

### 核心原则

**每个新功能都必须主动编写自动化测试**（不等用户要求）

### 快速工作流

1. **实现功能** → 2. **写测试**（`CTestFeatureName`）→ 3. **运行测试** → 4. **AI自动分析** → 5. **生成报告**

```bash
# 运行测试
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestXXX
```

### AI 必须自动执行

测试运行后：
1. 读取 `E:/forfun/debug/{TestName}/test.log` - 检查断言
2. 读取 `E:/forfun/debug/{TestName}/screenshot_frame20.png` - 视觉验证
3. 读取 `E:/forfun/debug/{TestName}/runtime.log` - 错误日志（如需要）
4. 生成测试分析报告


## Project Overview

中型游戏引擎+编辑器项目，目标类似 Unity/Unreal 但规模较小。

**当前状态**:
- ECS 架构 (Component-based GameObject system)
- Editor UI (Hierarchy, Inspector, Viewport, Scene Light Settings, IBL Debug, HDR Export)
- 3D 渲染 (OBJ/glTF loader)
- PBR (Cook-Torrance BRDF, physically-based)
- CSM 阴影 (bounding sphere stabilization + texel snapping)
- IBL (diffuse irradiance + specular pre-filtered map)
- Reflection Probes (TextureCubeArray, per-object selection, editor baking)
- Clustered Lighting (Point/Spot lights, 100+ lights @ 60 FPS)
- 场景序列化 + 组件自动注册
- Transform Gizmo (ImGuizmo: Translate/Rotate/Scale, Local/World, Grid snapping)
- HDR Export 工具 (HDR → KTX2 + .ffasset)
- KTX2 资源加载 (.ffasset → env/irr/prefilter)
- 自动化测试框架 (命令行驱动，帧回调架构)
- FFPath 统一路径管理 (自动规范化、绝对/相对路径转换)

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

## Path Management (FFPath)

### 设计原则

- **外部输入**: 灵活（绝对路径、相对路径、`/` 或 `\` 分隔符都可以）
- **内部存储**: 统一使用标准化相对路径（`folder/file.ext`，无前导 `/`，使用 `/` 分隔符）
- **文件操作**: 使用 `FFPath::GetAbsolutePath()` 转换为绝对路径

### API 使用

```cpp
#include "Core/PathManager.h"  // FFPath namespace

// 初始化（main.cpp 中调用一次）
FFPath::Initialize("E:/forfun");

// 主要 API（自动处理任意输入格式）
std::string fullPath = FFPath::GetAbsolutePath("mat/wood.ffasset");
// → "E:/forfun/assets/mat/wood.ffasset"

std::string fullPath2 = FFPath::GetAbsolutePath("E:/forfun/assets/mat/wood.ffasset");
// → "E:/forfun/assets/mat/wood.ffasset" (已经是绝对路径，直接返回)

std::string normalized = FFPath::Normalize("E:\\forfun\\assets\\mat\\wood.ffasset");
// → "mat/wood.ffasset"

// 目录访问
FFPath::GetAssetsDir();   // "E:/forfun/assets"
FFPath::GetDebugDir();    // "E:/forfun/debug"
FFPath::GetSourceDir();   // "E:/forfun/source/code"
```

### 强制规则

1. **存储路径时**: 始终使用 `FFPath::Normalize()` 标准化
   ```cpp
   settings.skyboxAssetPath = FFPath::Normalize(userSelectedPath);
   ```

2. **读取文件时**: 始终使用 `FFPath::GetAbsolutePath()`
   ```cpp
   std::ifstream file(FFPath::GetAbsolutePath(settings.skyboxAssetPath));
   ```

3. **禁止硬编码路径**: 除了 `FFPath::Initialize()` 中的项目根路径，其他地方不允许出现 `E:/forfun`

4. **Shader 路径**: 使用 `FFPath::GetSourceDir()` + 相对路径
   ```cpp
   std::wstring shaderPath = std::wstring(FFPath::GetSourceDir().begin(), FFPath::GetSourceDir().end())
                           + L"/Shader/MyShader.hlsl";
   ```

### 路径格式示例

| 输入 | `Normalize()` 结果 | `GetAbsolutePath()` 结果 |
|------|-------------------|-------------------------|
| `"mat/wood.ffasset"` | `"mat/wood.ffasset"` | `"E:/forfun/assets/mat/wood.ffasset"` |
| `"E:/forfun/assets/mat/wood.ffasset"` | `"mat/wood.ffasset"` | `"E:/forfun/assets/mat/wood.ffasset"` |
| `"mat\\wood.ffasset"` | `"mat/wood.ffasset"` | `"E:/forfun/assets/mat/wood.ffasset"` |
| `"./mat/wood.ffasset"` | `"mat/wood.ffasset"` | `"E:/forfun/assets/mat/wood.ffasset"` |

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

### **CRITICAL: Reflection System Naming Rules**

**问题历史 (2025-11-25)**:
初次实现 Material Editor 时，在 `CMaterialAsset::VisitProperties()` 中使用了友好的 UI 显示名称（如 "Albedo Texture"、"Emissive Strength"），导致 JSON 序列化字段名与现有 .ffasset 文件格式不匹配（应该是 "albedoTexture"、"emissiveStrength"），所有材质加载失败。

**强制规则**:

1. **VisitProperties() 中的 name 参数必须使用精确的成员变量名**
   ```cpp
   // ✓ CORRECT - 使用变量名
   visitor.VisitFloat3("albedo", albedo);
   visitor.VisitFilePath("albedoTexture", albedoTexture, filter);

   // ✗ WRONG - 使用 UI 显示名会导致序列化失败
   visitor.VisitFloat3("Albedo", albedo);
   visitor.VisitFilePath("Albedo Texture", albedoTexture, filter);
   ```

2. **JSON 序列化使用反射系统时，字段名 = VisitXXX 的第一个参数**
   - `CJsonWriteVisitor` 和 `CJsonReadVisitor` 直接使用 name 参数作为 JSON key
   - 如果 name 参数与成员变量名不匹配，会导致现有资源文件无法加载

3. **UI 层仍然可以友好显示**
   - ImGui 会将 "albedoTexture" 显示为 "albedoTexture"
   - 如需更友好的显示名，应在 ImGuiPropertyVisitor 中处理转换，而不是改变反射层的 name

4. **验证清单**（添加新的反射属性时）:
   - [ ] VisitXXX 的第一个参数是否与成员变量名完全一致？
   - [ ] JSON 文件中的字段名是否匹配？
   - [ ] 是否会导致现有资源文件加载失败？

**修复措施**:
所有使用反射系统的类（Components、Assets）都应遵循此规则。如发现违反，应立即修正。

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

- `docs/TESTING.md` - 自动化测试完整文档 ⭐
  - Test Framework Architecture (Frame Callback Pattern)
  - Test Naming Convention (GetName() vs Class Name)
  - Assertion Macros (ASSERT_*, VISUAL_EXPECTATION)
  - Development Workflow (Write → Run → AI Analysis → Report)
  - Best Practices (Sphere vs Cube, Material Setup, Light Intensity)
  - Common Issues & Solutions
  - AI Auto-Analysis Requirements

---

**Last Updated**: 2025-12-02
