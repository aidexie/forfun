# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Vision

This is a mid-sized game engine and editor project, targeting functionality similar to Unity/Unreal but at a smaller scale. The goal is to create a complete game editor that can support development of several fixed game types (e.g., 3D action, puzzle, platformer).

**Current Status**: Early development with basic foundation in place
- Entity-Component-System architecture established
- Editor UI with Hierarchy, Inspector, and Viewport panels
- Basic 3D rendering with OBJ/glTF model loading
- Transform, MeshRenderer, DirectionalLight components working
- Reflection system for component properties
- Scene serialization (Save/Load) in JSON format with auto-registration
- Shadow mapping with tight frustum fitting for quality optimization

**Development Philosophy**:
- Keep the architecture clean and maintainable
- Focus on practical game development workflows
- Prioritize editor usability and iteration speed
- Build incrementally with working features at each step

## Build Commands

This is a Windows-only DX11 project using CMake with Ninja generator.

```bash
# Configure (from repository root)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --target forfun

# Run
./build/forfun.exe
```

Note: The executable expects assets to be in `E:\forfun\assets`. The `ForceWorkDir()` function in main.cpp hardcodes this path.

## Architecture Overview

### Core Separation of Concerns

This is a custom game engine editor with three distinct layers:

1. **Core/** - Low-level device management and resource loading
   - `DX11Context`: Singleton managing D3D11 device, context, swapchain, backbuffer
   - `MeshResourceManager`: Singleton for loading and caching mesh resources (OBJ/glTF)
   - `GpuMeshResource`: RAII wrapper for GPU mesh data (VBO, IBO, textures)
   - `Mesh`, `ObjLoader`, `GltfLoader`, `TextureLoader`: Asset loading utilities
   - **Does NOT depend on Engine or Editor layers**

2. **Engine/** - Entity-component-system, scene management, and rendering logic
   - `World`: Container managing all GameObjects
   - `GameObject`: Named entities that own Components
   - `Component`: Base class for all components (Transform, MeshRenderer, etc.)
   - `Scene`: Combines World with selection state for editor
   - `Camera`: Editor camera with orbit controls
   - `Rendering/MainPass`: Main rendering pass (scene traversal, draw calls, shaders)

3. **Editor/** - ImGui-based editor UI (docking enabled)
   - `Panels.h`: Interface for Hierarchy, Inspector, Viewport, Dockspace panels
   - Each panel implemented in separate .cpp files
   - Viewport renders offscreen RT from MainPass as ImGui image

### Component System

Components attach to GameObjects and define behavior/data:
- `Transform`: Position, rotation (euler), scale; provides WorldMatrix()
- `MeshRenderer`: Stores mesh file path and owns `shared_ptr<GpuMeshResource>` for rendering
- `DirectionalLight`: Directional light source with shadow casting support
  - Color, Intensity: Light properties
  - ShadowMapSizeIndex (0/1/2): Shadow map resolution (1024/2048/4096)
  - ShadowDistance: Maximum shadow visible distance
  - ShadowBias: Depth bias to prevent shadow acne
  - Direction controlled by Transform rotation via `GetDirection()`

**Component Auto-Registration:**
- Components automatically register themselves using `REGISTER_COMPONENT(ComponentType)` macro
- Registry located in `Engine/ComponentRegistry.h`
- Adding a new component only requires adding the macro at the end of the component header file
- No manual updates to serialization code needed

Example:
```cpp
// Engine/Components/MyComponent.h
#include "ComponentRegistry.h"

struct MyComponent : public Component {
    float value = 0.0f;
    const char* GetTypeName() const override { return "MyComponent"; }
    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat("Value", value);
    }
};

REGISTER_COMPONENT(MyComponent)  // ← Auto-registers for serialization
```

**MeshRenderer Resource Management:**
- `path`: String path to mesh file (`.obj`, `.gltf`, `.glb`)
- `meshes`: Vector of `shared_ptr<GpuMeshResource>` (glTF files may contain multiple sub-meshes)
- `EnsureUploaded()`: Idempotent method that loads mesh via `MeshResourceManager::GetOrLoad(path)`
- MeshResourceManager handles caching with `weak_ptr` for automatic resource deduplication
- Changing path in Inspector automatically clears old meshes and triggers reload

### Reflection System

Components use **Visitor Pattern** for reflection:
- `PropertyVisitor`: Abstract interface defining property types (Float, Int, Bool, String, Float3, Enum)
- `Component::VisitProperties()`: Override to expose properties to editor/serialization
- `ImGuiPropertyVisitor`: Renders properties in Inspector panel
- `JsonWriteVisitor`/`JsonReadVisitor`: Serialize/deserialize to JSON

**Adding new reflected properties:**
1. Add member variable to component
2. Expose via `VisitProperties()` using visitor methods
3. Automatic UI generation and serialization

### Scene Serialization

Scenes save to JSON format (`.scene` files):
- **SceneSerializer**: Handles save/load operations using ComponentRegistry
- **Format**: JSON with GameObject hierarchy and component properties
- **Reflection-based**: Uses PropertyVisitor for automatic serialization
- **Auto-registration**: Components auto-register via `REGISTER_COMPONENT` macro
- **File Menu**: Save Scene (Ctrl+S), Load Scene (Ctrl+O)

**Serialization Architecture:**
- `GameObject::ForEachComponent()`: Iterates all components on a GameObject
- `ComponentRegistry::Instance().Create()`: Factory method creates components by type name
- `SceneSerializer` uses reflection to save/load all component properties automatically
- Adding new components requires NO changes to SceneSerializer code

JSON structure:
```json
{
  "version": "1.0",
  "gameObjects": [
    {
      "name": "Cube",
      "components": [
        {"type": "Transform", "Position": [0,0.5,0], "Rotation": [0,0,0], "Scale": [1,1,1]},
        {"type": "MeshRenderer", "Path": "mesh/cube.obj"}
      ]
    }
  ]
}
```

### Rendering Pipeline

**Architecture:** Layered rendering with clean separation between Core and Engine.

1. **Core Layer (Device & Resources)**:
   - `DX11Context`: Manages D3D11 device, context, swapchain
   - `MeshResourceManager`: Singleton that loads and caches mesh files
     - `GetOrLoad(path)`: Returns `vector<shared_ptr<GpuMeshResource>>`
     - Uses `weak_ptr` cache for automatic deduplication
     - Supports OBJ and glTF/GLB formats
   - `GpuMeshResource`: RAII wrapper containing VBO, IBO, texture SRVs

2. **Engine Layer (Rendering Logic)**:
   - `MainPass` (in `Engine/Rendering/MainPass.h/cpp`):
     - Owns rendering pipeline resources (shaders, constant buffers, samplers)
     - Manages offscreen render target for Viewport
     - `UpdateCamera(w, h, dt)`: Updates camera matrices (called before ShadowPass)
     - `Render(Scene&, w, h, dt, shadowData)`: Traverses scene and issues draw calls
     - Exposes camera view/proj matrices for shadow frustum fitting
     - Directly uses `DX11Context::Instance().GetDevice/GetContext()` for D3D11 API calls

   - `ShadowPass` (in `Engine/Rendering/ShadowPass.h/cpp`):
     - Renders shadow map from directional light's perspective
     - Uses **Tight Frustum Fitting** for optimal shadow quality
     - `Render(Scene&, light, cameraView, cameraProj)`: Generates shadow map
     - `GetOutput()`: Returns Output bundle (shadowMap, shadowSampler, lightSpaceVP)
     - Default 1×1 white shadow map when no DirectionalLight exists

   - Each frame rendering flow:
     1. `MainPass::UpdateCamera()` - Calculate camera matrices
     2. `ShadowPass::Render()` - Generate shadow map using camera frustum
     3. `MainPass::Render()` - Render scene with shadows

   - MainPass scene traversal:
     1. Traverses all GameObjects in Scene
     2. For each GameObject with MeshRenderer + Transform:
        - Calls `EnsureUploaded()` (idempotent, handles lazy loading)
        - Gets world matrix from Transform
        - Renders each GpuMeshResource with world transform
     3. Uses default textures for meshes without materials

3. **Main Loop Flow** (`main.cpp`):
   ```cpp
   MainPass gMainPass;
   ShadowPass gShadowPass;
   gMainPass.Initialize();
   gShadowPass.Initialize();

   // Each frame:
   // 1. Update camera
   gMainPass.UpdateCamera(vpWidth, vpHeight, dt);

   // 2. Collect DirectionalLight
   DirectionalLight* dirLight = FindDirectionalLight(gScene);

   // 3. Render shadow map
   if (dirLight) {
       gShadowPass.Render(gScene, dirLight,
                         gMainPass.GetCameraViewMatrix(),
                         gMainPass.GetCameraProjMatrix());
   }

   // 4. Render scene with shadows
   gMainPass.Render(gScene, vpWidth, vpHeight, dt, &gShadowPass.GetOutput());

   // 5. Display in viewport
   DrawViewport(gMainPass.GetOffscreenSRV(), ...);
   ```

4. **Resource Ownership**:
   - MeshRenderer owns `shared_ptr<GpuMeshResource>`
   - Multiple MeshRenderers can share the same resource (via shared_ptr)
   - Resources auto-release when last MeshRenderer is destroyed
   - MeshResourceManager cache uses weak_ptr to allow cleanup

### Third-Party Dependencies

- **imgui_docking**: Static library built from `${THIRD_PARTY_PATH}/imgui` (docking branch)
  - Includes Win32 and DX11 backends
  - Docking enabled via `ImGuiConfigFlags_DockingEnable`
- **cgltf**: Header-only glTF loader in `${THIRD_PARTY_PATH}/cgltf-master`
- **nlohmann/json**: Header-only JSON library in `${THIRD_PARTY_PATH}/nlohmann/json.hpp`
  - Download from: https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
  - Required for scene serialization
- DirectX 11, d3dcompiler (Windows SDK)

### Important Path Assumptions

- Source code: `E:/forfun/source/code`
- Third-party: `E:/forfun/thirdparty`
- Assets: `E:/forfun/assets` (hardcoded working directory in main.cpp)

When loading assets (OBJ, glTF, textures), paths are relative to the assets directory.

## Component Integration Pattern

To add a new component:

1. Create header in `Engine/Components/YourComponent.h`
2. Inherit from `Component`, implement `GetTypeName()`
3. Add to CMakeLists.txt ENGINE_SOURCES
4. Use `GameObject::AddComponent<YourComponent>()` to instantiate
5. Use `GameObject::GetComponent<YourComponent>()` to retrieve
6. If it needs rendering, own `shared_ptr<GpuMeshResource>` and load via MeshResourceManager

## Shadow System

**Architecture**: Two-pass rendering with tight frustum fitting for optimal quality.

### **ShadowPass: Shadow Map Generation**

Located in `Engine/Rendering/ShadowPass.h/cpp`.

**Key Features**:
- Depth-only rendering from DirectionalLight's perspective
- Orthographic projection for parallel light rays
- **Tight Frustum Fitting**: Shadow map tightly fits camera frustum for maximum precision
- Configurable shadow map resolution (1024/2048/4096)
- Default 1×1 white shadow map (depth=1.0) when no light exists

**Tight Frustum Fitting Algorithm**:
1. **Extract camera frustum corners**: Convert NDC space → World space (8 points)
2. **Extend far plane**: Move far plane corners along light's reverse direction by `ShadowDistance`
   - Captures shadow casters outside but near the frustum
   - Prevents shadow popping when objects enter/exit view
3. **Compute center**: Average of 8 extended corners
4. **Build light view matrix**: LookAt from light position → frustum center
5. **Transform to light space**: Convert 8 corners to light space coordinates
6. **Calculate AABB**: Find min/max X/Y/Z in light space
7. **Create tight projection**: Orthographic projection exactly covering AABB

**Why Tight Fitting?**
- Traditional: Fixed 50×50 coverage → Low precision (e.g., 40 pixels/unit)
- Tight Fitting: Covers only visible frustum → High precision (e.g., 200+ pixels/unit)
- **Quality improvement: 5-10× better shadow resolution**

**Extension Strategy**:
```cpp
// Extend far plane corners along -lightDir by shadowExtension (default 50 units)
extendedCorners[4-7] += (-lightDirection) * shadowExtension;
```
- Captures objects "upstream" of the frustum in light direction
- Example: Building behind camera casting shadow forward

**Pass Output Bundle**:
```cpp
struct Output {
    ID3D11ShaderResourceView* shadowMap;      // Shadow depth texture
    ID3D11SamplerState*       shadowSampler;  // Comparison sampler (PCF)
    DirectX::XMMATRIX         lightSpaceVP;   // Light space view-projection
};
```

### **MainPass: Shadow Application**

**Pixel Shader Shadow Sampling**:
- Transforms world position to light space
- Samples shadow map using comparison sampler
- PCF (Percentage Closer Filtering) 3×3 kernel for soft shadows
- Shadow bias from DirectionalLight component prevents shadow acne

**Shader Integration**:
```hlsl
// Transform to light space
float4 posLS = mul(float4(posWS, 1.0), gLightSpaceVP);

// PCF shadow sampling
float shadow = CalcShadowFactor(posLS, gShadowMap, gShadowSampler, gShadowBias);

// Apply to lighting
float3 diffuse = lightColor * max(dot(N, L), 0.0) * shadow;
```

### **Known Issues & Future Improvements**

**Current Limitations**:
1. **Near plane clipping**: Large objects may be clipped if they extend beyond frustum+extension
   - Symptoms: Parts of objects don't cast shadows (VS output Z < near plane)
   - Temporary fix: Increase Z margin in `calculateTightLightMatrix`
   - Proper fix: Include scene AABB in shadow bounds calculation

2. **Shadow shimmering**: Camera movement causes shadow map bounds to change
   - Future: Implement texel snapping (snap light space projection to texel grid)

3. **Single cascade**: All distances use same resolution
   - Future: Implement 2-4 level CSM (Cascaded Shadow Maps) for better quality distribution

**Upgrade Path**:
- Phase 1 (Current): Single frustum fitting
- Phase 2: Texel snapping for stability
- Phase 3: 2-level CSM (near/far splits)
- Phase 4: 4-level CSM with cascade blending

## Component Integration Pattern

To add a new component:

1. Create header in `Engine/Components/YourComponent.h`
2. Inherit from `Component`, implement `GetTypeName()`
3. Add `#include "ComponentRegistry.h"`
4. Add `REGISTER_COMPONENT(YourComponent)` at end of header file
5. Add to CMakeLists.txt ENGINE_SOURCES
6. Component automatically works with serialization and Inspector

Example:
```cpp
// Engine/Components/PointLight.h
#pragma once
#include "Component.h"
#include "ComponentRegistry.h"

struct PointLight : public Component {
    DirectX::XMFLOAT3 Color{1, 1, 1};
    float Intensity = 1.0f;
    float Range = 10.0f;

    const char* GetTypeName() const override { return "PointLight"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat3("Color", Color);
        visitor.VisitFloat("Intensity", Intensity);
        visitor.VisitFloat("Range", Range);
    }
};

REGISTER_COMPONENT(PointLight)  // ← Only addition needed!
```

## Editor Panel Integration

To add a new panel:

1. Add function declaration to `Editor/Panels.h`
2. Implement in `Editor/PanelName.cpp`
3. Add cpp file to EDITOR_SRC in CMakeLists.txt
4. Call from main loop after `ImGui::NewFrame()`

Panels receive `Scene&` to access World and selection state.
