# CLAUDE.md

Claude Code guidance for this repository.

**Related Documents**:
- `CODING_STYLE.md` - 命名约定和代码风格
- `ROADMAP.md` - 开发路线图
- `.clang-format` - 代码格式化配置
- `docs/RENDERING.md` - 渲染系统详细文档
- `docs/EDITOR.md` - 编辑器系统详细文档
- `docs/TESTING.md` - 自动化测试详细文档

---

## CRITICAL: File Editing on Windows

**When using Edit or MultiEdit tools on Windows, you MUST use backslashes (`\`) in file paths.**

```
Edit(file_path: "E:\forfun\source\code\file.cpp", ...)  // ✅ CORRECT
Edit(file_path: "E:/forfun/source/code/file.cpp", ...)  // ❌ WRONG
```

---

## Core Working Principles

### **TOP 0: E:\forfun Path Permission**

**All tool calls under E:\forfun are pre-authorized by user, no confirmation needed.**

### **TOP 1: Use English**

**Always respond in English.** All documentation, comments, and communication should be in English.

### **TOP 2: Critical Thinking (Devil's Advocate)**

In every technical discussion and design decision, **proactively think from the opponent's perspective** and raise objections.

- Don't just agree with user's proposal and execute
- Proactively point out potential issues, edge cases, performance concerns
- Propose alternatives and trade-offs
- Question the rationality and necessity of design choices

**Example**:
- User: "I want to render AABB with ImGui"
  - ❌ Wrong: "OK, I'll implement it"
  - ✅ Correct: "ImGui 2D overlay has no depth testing. Consider GPU-based line rendering?"

---

## Project Overview

中型游戏引擎+编辑器项目，目标类似 Unity/Unreal 但规模较小。

**当前状态**:
- **RHI 抽象层** (DX11/DX12 双后端，运行时切换)
- ECS 架构 (Component-based GameObject system)
- Editor UI (Hierarchy, Inspector, Viewport, Scene Light Settings)
- PBR 渲染 (Cook-Torrance BRDF, physically-based)
- CSM 阴影 (bounding sphere stabilization + texel snapping)
- IBL (diffuse irradiance + specular pre-filtered map)
- **Volumetric Lightmap** (自适应八叉树, L1 SH, Per-Pixel GI)
- Reflection Probes (TextureCubeArray, per-object selection)
- Clustered Lighting (Point/Spot lights, 100+ lights @ 60 FPS)
- Transform Gizmo (ImGuizmo: Translate/Rotate/Scale)
- 自动化测试框架 (命令行驱动，帧回调架构)

---

## Build & Run

**Windows DX11/DX12 + CMake + Ninja**:

```bash
# Build
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun

# Run editor
./build/Debug/forfun.exe

# Run test
./build/Debug/forfun.exe --test TestRayCast
```

**Log Output**:
- Editor mode: `debug/logs/runtime.log`
- Test mode: `debug/logs/{TestCaseName}/runtime.log`

### Backend Selection

**配置文件**: `render.json` (项目根目录)
```json
{
  "renderBackend": "DX11"
}
```
可选值: `"DX11"` (默认, 稳定) | `"DX12"` (开发中)

**Paths**:
- Source: `E:/forfun/source/code`
- Third-party: `E:/forfun/thirdparty`
- Assets: `E:/forfun/assets`
- Debug output: `E:/forfun/debug/{TestName}/`

**Dependencies**: imgui_docking, cgltf, nlohmann/json, DirectX 11/12, KTX-Software

---

## Automated Testing

**详细文档**: `docs/TESTING.md`

### 核心原则

**每个新功能都必须主动编写自动化测试**（不等用户要求）

### 快速工作流

```bash
# 运行测试
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestXXX
```

**测试运行后 AI 必须自动**:
1. 读取 `E:/forfun/debug/{TestName}/test.log` - 检查断言
2. 读取 `E:/forfun/debug/{TestName}/screenshot_frame20.png` - 视觉验证
3. 生成测试分析报告

---

## Architecture

### RHI Abstraction Layer (NEW)

**位置**: `RHI/`

**设计理念**: 渲染硬件接口，支持多后端切换

```
RHI/
├── IRHIContext.h        # Device, SwapChain, CommandQueue 接口
├── IRHIRenderContext.h  # 资源创建, PSO, CommandList 接口
├── RHIManager.h         # 运行时后端管理
├── DX11/                # DX11 后端实现
└── DX12/                # DX12 后端实现
    ├── DX12Context.cpp          # Device, Fence 同步
    ├── DX12RenderContext.cpp    # Root Signature, PSO Builder
    ├── DX12CommandList.cpp      # 命令列表, 资源绑定
    ├── DX12DescriptorHeap.cpp   # 描述符堆管理
    └── DX12UploadManager.cpp    # 上传堆管理
```

**DX12 调试**: `DX12_CHECK` 宏包装所有 D3D12 API 调用，输出文件名/行号

### Three-Layer Separation

1. **Core/** - 底层设备管理、资源加载、RHI
   - `RHI/`: 渲染硬件抽象层
   - `MeshResourceManager`: Mesh 加载/缓存
   - `Testing/`: 测试框架

2. **Engine/** - ECS、场景、渲染
   - `World`: GameObject 容器
   - `Scene`: World + SceneLightSettings
   - `Rendering/`: MainPass, ShadowPass, Skybox, VolumetricLightmap

3. **Editor/** - ImGui UI
   - `Panels.h`: Panel 接口
   - 每个 Panel 独立 `.cpp` 文件

### Component System

**内置组件**: Transform, MeshRenderer, Material, DirectionalLight, PointLight, SpotLight, ReflectionProbe

**添加新组件**:
```cpp
// Engine/Components/PointLight.h
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

### **CRITICAL: Reflection System Naming Rules**

**VisitProperties() 中的 name 参数必须使用精确的成员变量名**

```cpp
// ✅ CORRECT - 使用变量名
visitor.VisitFloat3("albedo", albedo);

// ❌ WRONG - 会导致 JSON 序列化失败
visitor.VisitFloat3("Albedo", albedo);
```

---

## Coordinate System

**DirectX 左手坐标系**: +X Right, +Y Up, +Z Forward (into screen)

**UV Convention**: 原点左上角 (0,0), U 左→右, V 上→下

所有矩阵操作使用 `LH` 后缀函数 (`XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`)。

---

## Path Management (FFPath)

**设计原则**:
- **外部输入**: 灵活（绝对/相对路径，`/` 或 `\` 都可以）
- **内部存储**: 统一标准化相对路径 (`folder/file.ext`)
- **文件操作**: 使用 `FFPath::GetAbsolutePath()` 转换

```cpp
#include "Core/PathManager.h"

FFPath::GetAbsolutePath("mat/wood.ffasset");  // → "E:/forfun/assets/mat/wood.ffasset"
FFPath::Normalize("E:\\forfun\\assets\\mat\\wood.ffasset");  // → "mat/wood.ffasset"
FFPath::GetAssetsDir();   // "E:/forfun/assets"
FFPath::GetDebugDir();    // "E:/forfun/debug"
FFPath::GetSourceDir();   // "E:/forfun/source/code"
```

**强制规则**: 禁止硬编码 `E:/forfun`（除 `FFPath::Initialize()`）

---

## Graphics Rendering (Quick Reference)

**详细文档**: `docs/RENDERING.md`

### Physics-Based Rendering

**所有图形特性必须物理正确**。禁止非物理 hack。

**Color Space**:
- Albedo/Emissive: `UNORM_SRGB`
- Normal/Metallic/Roughness/AO: `UNORM`
- Intermediate RT: `R16G16B16A16_FLOAT`

**CSM Shadow**: 1-4 cascades, PCF 3×3

**IBL**: Diffuse (32×32) + Specular pre-filtered (128×128, 7 mip)

### Volumetric Lightmap (NEW)

**位置**: `Engine/Rendering/VolumetricLightmap.h/cpp`

**架构**: UE4/5 风格，用于高质量 Per-Pixel 漫反射全局光照
- 自适应八叉树 Brick 系统
- 两级 GPU 查找: World Position → Indirection → Brick Atlas
- L1 球谐 (SH9) 编码
- Overlap Baking 消除接缝

**Diffuse GI Mode** (`SceneLightSettings.h`):
```cpp
enum class EDiffuseGIMode : int {
    VolumetricLightmap = 0,  // Per-Pixel GI
    GlobalIBL = 1,           // Skybox Irradiance
    None = 2                 // Disabled
};
```

---

## Editor System (Quick Reference)

**详细文档**: `docs/EDITOR.md`

**添加新 Panel**:
1. 声明到 `Editor/Panels.h`
2. 实现 `Editor/Panels_PanelName.cpp`
3. 添加到 `CMakeLists.txt`
4. 在 main loop 中调用

**Transform Gizmo**: W (Translate) / E (Rotate) / R (Scale), World/Local 切换

---

## DX12 Known Issues

参见 `ROADMAP.md` "已知问题" 章节：
- 纹理初始数据上传未完成
- Buffer 初始数据上传未完成
- 资源状态跟踪警告

---

## Documentation Index

### Core Documents (Root)
- `CLAUDE.md` (本文件) - AI 工作指南 + 快速参考
- `CODING_STYLE.md` - 命名约定（C/S/I/E 前缀）
- `ROADMAP.md` - 开发路线图 + 已知问题
- `render.json` - 渲染后端配置

### Detailed References (docs/)
- `docs/RENDERING.md` - 渲染系统完整文档
- `docs/EDITOR.md` - 编辑器系统完整文档
- `docs/TESTING.md` - 自动化测试完整文档
- `docs/TEXTURE_MANAGER.md` - 纹理管理器文档（同步/异步加载）

---

**Last Updated**: 2025-12-17
