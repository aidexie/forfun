# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Vision

This is a mid-sized game engine and editor project, targeting functionality similar to Unity/Unreal but at a smaller scale. The goal is to create a complete game editor that can support development of several fixed game types (e.g., 3D action, puzzle, platformer).

**Current Status**: Early development with basic foundation in place
- Entity-Component-System architecture established
- Editor UI with Hierarchy, Inspector, and Viewport panels
- Basic 3D rendering with OBJ/glTF model loading
- Transform and MeshRenderer components working
- Reflection system for component properties
- Scene serialization (Save/Load) in JSON format

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
- **SceneSerializer**: Handles save/load operations
- **Format**: JSON with GameObject hierarchy and component properties
- **Reflection-based**: Uses PropertyVisitor for automatic serialization
- **File Menu**: Save Scene (Ctrl+S), Load Scene (Ctrl+O)

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
     - `Render(Scene&, w, h, dt)`: Traverses scene and issues draw calls
     - Directly uses `DX11Context::Instance().GetDevice/GetContext()` for D3D11 API calls
   - Each frame, MainPass:
     1. Traverses all GameObjects in Scene
     2. For each GameObject with MeshRenderer + Transform:
        - Calls `EnsureUploaded()` (idempotent, handles lazy loading)
        - Gets world matrix from Transform
        - Renders each GpuMeshResource with world transform
     3. Uses default textures for meshes without materials

3. **Main Loop Flow** (`main.cpp`):
   ```cpp
   MainPass gMainPass;
   gMainPass.Initialize();

   // Each frame:
   gMainPass.Render(gScene, vpWidth, vpHeight, dt);
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

## Editor Panel Integration

To add a new panel:

1. Add function declaration to `Editor/Panels.h`
2. Implement in `Editor/PanelName.cpp`
3. Add cpp file to EDITOR_SRC in CMakeLists.txt
4. Call from main loop after `ImGui::NewFrame()`

Panels receive `Scene&` to access World and selection state.
