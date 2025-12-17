# Editor System Documentation

Complete reference for editor panels, gizmos, and tools.

---

## Editor Architecture

### Panel System

**位置**: `Editor/Panels.h`, `Editor/Panels_*.cpp`

**架构**:
- `Panel` 基类 (接口)
- 每个面板独立 `.cpp` 文件
- Dockable ImGui 布局

**当前面板**:
- Dockspace (主窗口容器)
- Hierarchy (场景层级)
- Inspector (属性编辑器)
- Viewport (3D 视图)
- Scene Light Settings (场景光照配置)
- Irradiance Debug (IBL 调试)
- HDR Export (HDR 导出工具)

### Adding New Panel

**步骤**:

1. **声明接口** (`Editor/Panels.h`):
```cpp
void DrawMyNewPanel();
```

2. **实现面板** (`Editor/Panels_MyNewPanel.cpp`):
```cpp
#include "Panels.h"
#include "Engine/Scene.h"

void DrawMyNewPanel() {
    if (ImGui::Begin("My New Panel")) {
        // Panel content here
    }
    ImGui::End();
}
```

3. **添加到 CMakeLists.txt**:
```cmake
set(EDITOR_SRC
    ...
    Editor/Panels_MyNewPanel.cpp
)
```

4. **调用面板** (main loop):
```cpp
ImGui::NewFrame();
DrawDockspace();
DrawHierarchy();
DrawInspector();
DrawMyNewPanel();  // Add here
ImGui::Render();
```

---

## Hierarchy Panel

**位置**: `Editor/Panels_Hierarchy.cpp`

**功能**: 显示场景中所有 GameObject 的树状列表

**交互**:
- 左键点击: 选中对象
- 右键菜单:
  - Create Empty: 创建空 GameObject
  - Delete: 删除选中对象

**实现要点**:
```cpp
for (int i = 0; i < scene.GetWorld().Count(); ++i) {
    auto* obj = scene.GetWorld().Get(i);
    bool selected = (scene.GetSelectedIndex() == i);

    if (ImGui::Selectable(obj->GetName().c_str(), selected)) {
        scene.SetSelected(i);
    }
}
```

---

## Inspector Panel

**位置**: `Editor/Panels_Inspector.cpp`

**功能**: 编辑选中 GameObject 的所有 Component 属性

**特性**:
- 对象名称编辑
- 组件属性编辑（通过 `PropertyVisitor` 反射）
- 添加组件按钮
- 删除组件按钮

**实现要点**:
```cpp
auto* obj = scene.GetWorld().Get(scene.GetSelectedIndex());

// Name editor
char buf[128];
strcpy(buf, obj->GetName().c_str());
if (ImGui::InputText("Name", buf, sizeof(buf))) {
    obj->SetName(buf);
}

// Component editors
for (auto& component : obj->GetComponents()) {
    if (ImGui::CollapsingHeader(component->GetTypeName())) {
        ImGuiPropertyVisitor visitor;
        component->VisitProperties(visitor);
    }
}
```

**PropertyVisitor 系统**:
- `ImGuiPropertyVisitor`: 生成 ImGui 控件
- `JsonWriteVisitor`/`JsonReadVisitor`: 序列化/反序列化
- 组件实现 `VisitProperties()` 暴露字段

---

## Viewport Panel

**位置**: `Editor/Panels_Viewport.cpp`

**功能**: 显示 3D 场景渲染结果，提供变换工具和相机控制

### 3D Rendering

```cpp
ImVec2 viewportSize = ImGui::GetContentRegionAvail();
ImGui::Image((void*)gMainPass.GetOffscreenSRV(), viewportSize);
```

### Camera Controls

**WASD**: 前后左右移动
**QE**: 上下移动
**右键拖拽**: 旋转视角
**滚轮**: 调整移动速度

**实现**: `Engine/Rendering/MainPass.cpp` → `UpdateCamera()`

### Toolbar

**位置**: Viewport 顶部

**控件**:
- Transform 模式按钮: Translate (W) / Rotate (E) / Scale (R)
- 空间模式切换: World / Local
- Snap 开关 + 步进值设置

---

## Transform Gizmo

**位置**: Viewport 中心（选中对象时显示）

**依赖**: ImGuizmo 库

### Operation Modes

| 模式      | 快捷键 | 功能     | Snap 步进         |
|-----------|--------|----------|-------------------|
| Translate | W      | 平移物体 | 0.01-10m (默认1m) |
| Rotate    | E      | 旋转物体 | 1-90° (默认15°)   |
| Scale     | R      | 缩放物体 | 0.01-2 (默认0.5)  |

### Space Modes

- **World**: 轴向始终对齐世界坐标系（XYZ = 红绿蓝）
- **Local**: 轴向随物体旋转（对齐局部坐标系）

### Grid Snapping

**启用**: 勾选 Toolbar 中的 "Snap" 复选框

**步进值设置**:
```cpp
// Translate snap
ImGui::DragFloat("##snapValue", &translateSnapValue, 0.1f, 0.01f, 10.0f);

// Rotate snap (degrees)
ImGui::DragFloat("##snapValue", &rotateSnapValue, 1.0f, 1.0f, 90.0f);

// Scale snap
ImGui::DragFloat("##snapValue", &scaleSnapValue, 0.01f, 0.01f, 2.0f);
```

**实现**:
```cpp
float snapValues[3] = {currentSnapValue, currentSnapValue, currentSnapValue};
ImGuizmo::Manipulate(
    viewMatrix, projMatrix,
    operation, mode,
    matrix,
    nullptr,
    useSnap ? snapValues : nullptr
);
```

---

## View Orientation Gizmo

**位置**: Viewport 右上角

**功能**: 显示当前相机朝向，快速切换视图

### 渲染实现

**自定义 ImGui DrawList 渲染**:
```cpp
ImDrawList* drawList = ImGui::GetWindowDrawList();

// 1. 绘制坐标轴（深度排序）
struct Axis {
    XMFLOAT3 dir;
    ImU32 color;
    const char* label;
    float depth;  // for depth sorting
};

// 2. 投影到屏幕空间
XMVECTOR screenPos = XMVector3Project(axisEnd, 0, 0, w, h, 0, 1, proj, view, identity);

// 3. 绘制（先绘制背面，后绘制前面）
std::sort(axes.begin(), axes.end(), [](auto& a, auto& b) {
    return a.depth < b.depth;  // 远的先画
});

for (auto& axis : axes) {
    if (axis.depth > 0) {
        // 正方向: 亮色 + 箭头 + 标签
        drawList->AddLine(center, end, axis.color, 2.0f);
        drawList->AddTriangle(...);  // Arrow
        drawList->AddText(end, axis.color, axis.label);
    } else {
        // 负方向: 灰色细线
        drawList->AddLine(center, end, IM_COL32(100,100,100,255), 1.0f);
    }
}
```

### 交互

**未来计划**: 点击轴标签快速切换到正交视图（Top/Front/Right）

---

## Scene Light Settings Panel

**位置**: `Editor/Panels_SceneLightSettings.cpp`

**功能**: 编辑场景级别光照配置（非 GameObject 属性）

### Skybox Configuration

**控件**:
```cpp
char buf[256];
strcpy(buf, settings.skyboxAssetPath.c_str());
if (ImGui::InputText("Skybox Asset Path", buf, sizeof(buf))) {
    settings.skyboxAssetPath = buf;

    // 立即应用
    if (ImGui::Button("Apply")) {
        CScene::Instance().Initialize(settings.skyboxAssetPath);
    }
}
```

**路径示例**: `skybox/test.ffasset`

### 数据存储

**位置**: `Engine/SceneLightSettings.h`

```cpp
struct SceneLightSettings {
    std::string skyboxAssetPath;
};
```

**序列化**: 保存到 `.scene` 文件的 `lightSettings` 节点

---

## Irradiance Debug Panel

**位置**: `Editor/Panels_IrradianceDebug.cpp`

**功能**: 实时预览 IBL 纹理（环境/辐照度/预过滤）

### Tab Layout

```cpp
if (ImGui::BeginTabBar("IrradianceTabs")) {
    if (ImGui::BeginTabItem("Irradiance Map")) {
        // 显示 32×32 diffuse irradiance
    }
    if (ImGui::BeginTabItem("Pre-Filtered Map")) {
        // 显示 128×128 specular pre-filtered
        // Mip level slider (0-6)
    }
    if (ImGui::BeginTabItem("Environment Map")) {
        // 显示原始环境贴图
        // Mip level slider (0-9)
    }
}
```

### Cubemap Unwrapping

**显示方式**: 十字展开布局（5个面可见）

```
      +Y (Top)
-X  +Z  +X  -Z
      -Y (Bottom)
```

**实现**: 多个 `ImGui::Image()` 调用，精确计算 UV 矩形

```cpp
// +Z face (center)
ImVec2 size(faceSize, faceSize);
ImVec2 uv0(1.0f/3, 1.0f/3);  // Face +Z in cubemap array
ImVec2 uv1(2.0f/3, 2.0f/3);
ImGui::Image(srv, size, uv0, uv1);
```

---

## HDR Export Tool

**位置**: `Editor/Panels_HDRExport.cpp`

**功能**: 将 HDR 环境贴图导出为 IBL 资源包（KTX2 + .ffasset）

### Export Workflow

**UI**: Window → HDR Export

**步骤**:

1. **选择 HDR 源文件**:
```cpp
if (ImGui::Button("Browse HDR...")) {
    std::string path = OpenFileDialog("HDR Files\0*.hdr\0All Files\0*.*\0");
    if (!path.empty()) {
        hdrSourcePath = path;
    }
}
ImGui::Text("Source: %s", hdrSourcePath.c_str());
```

2. **输入输出目录**:
```cpp
ImGui::InputText("Output Directory", outputDir, 256);
ImGui::InputText("Asset Name", assetName, 128);
```

3. **点击 Export**:
```cpp
if (ImGui::Button("Export")) {
    ExportHDRAsset(hdrSourcePath, outputDir, assetName);
}
```

### Export Process

**实现**: `Core/Exporter/KTXExporter.h/cpp`

**流程**:

1. **加载 HDR 源文件** (stb_image: `stbi_loadf()`)
2. **Equirectangular → Cubemap** (6个面，每个面渲染一次)
3. **生成 Environment Map** (512×512 + mip chain)
4. **生成 Irradiance Map** (32×32, uniform solid angle sampling)
5. **生成Pre-Filtered Map** (128×128 + 7 mips, GGX importance sampling)
6. **压缩为 BC6H** (libktx: `ktxTexture2_CompressBasisEx()`)
7. **写入 KTX2 文件** (3个文件)
8. **生成 .ffasset 描述符** (JSON)

### Output Files

```
{outputDir}/
  ├── {assetName}_env.ktx2         (环境贴图, 512×512, 10 mip)
  ├── {assetName}_irr.ktx2         (辐照度图, 32×32, 1 mip)
  ├── {assetName}_prefilter.ktx2   (预过滤图, 128×128, 7 mip)
  └── {assetName}.ffasset          (描述符)
```

### .ffasset Format

```json
{
  "type": "skybox",
  "version": "1.0",
  "source": "source.hdr",
  "data": {
    "env": "{assetName}_env.ktx2",
    "irr": "{assetName}_irr.ktx2",
    "prefilter": "{assetName}_prefilter.ktx2"
  }
}
```

### Usage

**加载资源包**:
```cpp
CScene::Instance().Initialize("skybox/{assetName}.ffasset");
```

**注意**:
- 导出过程需要 1-5 分钟（取决于采样数）
- 进度显示在 Console (runtime.log)
- 导出失败会显示错误对话框

---

## File Dialog Utilities

**位置**: `Core/DebugPaths.h` (Windows 平台)

**功能**: 原生文件对话框（Open/Save）

### OpenFileDialog

```cpp
std::string path = OpenFileDialog(
    "HDR Files\0*.hdr\0All Files\0*.*\0"
);
if (!path.empty()) {
    // User selected a file
}
```

**实现**: `IFileOpenDialog` (COM API)

### SaveFileDialog

```cpp
std::string path = SaveFileDialog(
    "Scene Files\0*.scene\0All Files\0*.*\0",
    "scene"  // Default extension
);
```

**实现**: `IFileSaveDialog` (COM API)

---

## 后续计划

### Phase 1: Editor 增强
- [ ] Undo/Redo 系统
- [ ] Multi-selection 支持
- [ ] Drag & Drop 资源导入
- [ ] Material Editor 面板

### Phase 2: Scene Management
- [ ] Prefab 系统
- [ ] Scene Templates
- [ ] Asset Browser 面板

### Phase 3: Advanced Tools
- [ ] Terrain Editor
- [ ] Particle System Editor
- [ ] Animation Timeline

---

**Last Updated**: 2025-12-17
