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

1. **Core/** - Low-level rendering and resource management
   - `DX11Context`: Singleton managing D3D11 device, context, swapchain, backbuffer
   - `Renderer`: Scene rendering to offscreen targets; manages GPU meshes, shaders, materials
   - `Mesh`, `ObjLoader`, `GltfLoader`, `TextureLoader`: Asset loading utilities
   - `Offscreen`: Offscreen render target management

2. **Engine/** - Entity-component-system and scene management
   - `World`: Container managing all GameObjects
   - `GameObject`: Named entities that own Components
   - `Component`: Base class for all components (Transform, MeshRenderer, etc.)
   - `Scene`: Combines World with selection state for editor
   - `Camera`: Editor camera with orbit controls

3. **Editor/** - ImGui-based editor UI (docking enabled)
   - `Panels.h`: Interface for Hierarchy, Inspector, Viewport, Dockspace panels
   - Each panel implemented in separate .cpp files
   - Viewport renders offscreen RT from Renderer as ImGui image

### Component System

Components attach to GameObjects and define behavior/data:
- `Transform`: Position, rotation (euler), scale; provides WorldMatrix()
- `MeshRenderer`: References mesh kind (Plane/Cube/Obj), path, and GPU mesh indices in Renderer

The MeshRenderer bridges Engine and Core layers:
- `EnsureUploaded()`: Creates GPU mesh in Renderer if needed
- `UpdateTransform()`: Updates world matrix of GPU mesh each frame

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
        {"type": "MeshRenderer", "Mesh Kind": 1, "Path": ""}
      ]
    }
  ]
}
```

### Rendering Pipeline

1. **main.cpp** manages the main loop:
   - **Lazy initialization**: For each GameObject, calls `MeshRenderer::EnsureUploaded()` to handle newly added components
   - Updates GameObject transforms via `MeshRenderer::UpdateTransform()`
   - `Renderer::RenderToOffscreen()` renders scene to offscreen RT
   - Viewport panel displays offscreen RT via ImGui::Image()
   - ImGui rendered to backbuffer
   - Backbuffer presented via DX11Context

2. **Renderer** maintains `std::vector<GpuMesh>`:
   - Each GpuMesh has VBO, IBO, world matrix, albedo/normal SRVs
   - MeshRenderer stores indices into this vector
   - Renderer does NOT know about GameObjects/Components

3. **Lazy Resource Loading**:
   - Components added via Inspector are not immediately uploaded to GPU
   - Main loop calls `EnsureUploaded()` each frame, which is idempotent
   - This handles dynamic component addition without invasive callbacks

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
6. If it needs rendering, follow MeshRenderer pattern of storing Renderer indices

## Editor Panel Integration

To add a new panel:

1. Add function declaration to `Editor/Panels.h`
2. Implement in `Editor/PanelName.cpp`
3. Add cpp file to EDITOR_SRC in CMakeLists.txt
4. Call from main loop after `ImGui::NewFrame()`

Panels receive `Scene&` to access World and selection state.
