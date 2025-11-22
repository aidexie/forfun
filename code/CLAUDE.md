# CLAUDE.md

Claude Code guidance for this repository.

**Related Documents**:
- `CODING_STYLE.md` - 命名约定和代码风格
- `ROADMAP.md` - 开发路线图
- `.clang-format` - 代码格式化配置

---

## Project Overview

中型游戏引擎+编辑器项目，目标类似 Unity/Unreal 但规模较小。

**当前状态**:
- ECS 架构
- Editor UI (Hierarchy, Inspector, Viewport, Debug panels)
- 3D 渲染 (OBJ/glTF)
- PBR (Cook-Torrance BRDF)
- CSM 阴影 (bounding sphere stabilization + texel snapping)
- IBL (diffuse irradiance + specular pre-filtered map)
- 场景序列化 + 组件自动注册
- Transform Gizmo (ImGuizmo: 平移/旋转/缩放, Local/World, Grid snapping)
- HDR Export 工具 (HDR → KTX2 + .ffasset)
- KTX2 资源加载 (.ffasset → env/irr/prefilter)

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

**Last Updated**: 2025-11-22
