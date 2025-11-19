# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Vision

This is a mid-sized game engine and editor project, targeting functionality similar to Unity/Unreal but at a smaller scale. The goal is to create a complete game editor that can support development of several fixed game types (e.g., 3D action, puzzle, platformer).

**Current Status**: Early development with basic foundation in place
- Entity-Component-System architecture established
- Editor UI with Hierarchy, Inspector, Viewport, and Debug panels
- Basic 3D rendering with OBJ/glTF model loading
- Transform, MeshRenderer, DirectionalLight, Material components working
- Reflection system for component properties
- Scene serialization (Save/Load) in JSON format with auto-registration
- CSM shadow mapping with bounding sphere stabilization and texel snapping
- PBR rendering with Cook-Torrance BRDF (direct lighting)
- IBL irradiance map generation with debug visualization (diffuse indirect lighting - TODO: integrate into shaders)

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
- `Material`: PBR material properties (per-GameObject)
  - `Albedo` (XMFLOAT3): Base color [0,1] for each RGB channel
  - `Metallic` (float): Metallic property [0,1], controlled by slider in Inspector
  - `Roughness` (float): Surface roughness [0,1], controlled by slider in Inspector
  - Used in MainPass rendering: passed to CB_Object constant buffer for PBR shader
  - Default values: Albedo=(1,1,1), Metallic=0.0, Roughness=0.5
- `DirectionalLight`: Directional light source with CSM shadow casting support
  - Color, Intensity: Basic light properties
  - Direction: Controlled by Transform rotation via `GetDirection()`
  - **Shadow Map Parameters**:
    - `ShadowMapSizeIndex` (0/1/2): Resolution 1024/2048/4096
    - `ShadowDistance`: Maximum shadow visible distance from camera
    - `ShadowBias`: Depth bias to prevent shadow acne
    - `ShadowNearPlaneOffset`: Extends near plane to capture tall objects
    - `EnableSoftShadows`: Toggle PCF 3×3 soft shadow filtering
  - **CSM Parameters**:
    - `CascadeCount` (1-4): Number of cascades for quality distribution
    - `CascadeSplitLambda` (0-1): Split scheme (0=uniform, 1=logarithmic)
    - `CascadeBlendRange` (0-0.5): Blend distance at cascade boundaries
    - `DebugShowCascades`: Visualize cascade levels with color coding

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
     - **PBR Shaders**: Loaded from external files (`Shader/MainPass.vs.hlsl`, `MainPass.ps.hlsl`)
       - Cook-Torrance BRDF: GGX normal distribution, Schlick-GGX geometry, Fresnel-Schlick
       - Energy conservation: `kD = (1 - kS) * (1 - metallic)`
       - Constant buffers: `CB_Frame` (camera, light), `CB_Object` (world matrix, material properties)
     - Manages offscreen render target for Viewport
     - `UpdateCamera(w, h, dt)`: Updates camera matrices (called before ShadowPass)
     - `Render(Scene&, w, h, dt, shadowData)`: Traverses scene and issues draw calls
     - Reads `Material` component per GameObject, passes albedo/metallic/roughness to shader
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
        - Reads Material component (albedo, metallic, roughness) or uses defaults
        - Updates CB_Object with world matrix and material properties
        - Renders each GpuMeshResource with world transform
     3. Uses default textures for meshes without albedo/normal/metallic/roughness textures

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

### Color Space Management

**Critical Design Principle:** The rendering pipeline maintains **strict color space separation** for physically correct rendering.

**Pipeline Color Space Flow:**

```
┌──────────────────────────────────────────────────────────────────────┐
│ 1. Texture Input (Albedo/Emissive)                                   │
│    Format: DXGI_FORMAT_R8G8B8A8_UNORM_SRGB                           │
│    Action: GPU automatically converts sRGB → Linear on sample        │
│    Result: Linear space color values in shader                       │
└────────────────────────┬─────────────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 2. All Intermediate Passes (MainPass, ShadowPass, Skybox, etc.)     │
│    Format: DXGI_FORMAT_R16G16B16A16_FLOAT (HDR)                      │
│    Space: Linear (all lighting calculations in linear space)         │
│    Output: HDR linear color values [0, ∞]                            │
└────────────────────────┬─────────────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 3. PostProcessPass (Final Stage)                                     │
│    Input: HDR linear RT (R16G16B16A16_FLOAT)                         │
│    Process:                                                           │
│      a) Tone Mapping (HDR → LDR, still linear)                       │
│      b) Output to DXGI_FORMAT_R8G8B8A8_UNORM_SRGB RT                 │
│    Action: GPU automatically applies Gamma correction (Linear → sRGB)│
│    Result: LDR sRGB color ready for display                          │
└──────────────────────────────────────────────────────────────────────┘
```

**Key Rules:**

1. **Albedo/Emissive Textures**: Always use `UNORM_SRGB` format
   - Automatically converted to linear space when sampled in shaders
   - Example: Albedo from PNG/JPG (stored in sRGB) → sampled as linear

2. **Normal/Metallic/Roughness/AO Textures**: Always use `UNORM` format
   - These contain linear data (not colors), no sRGB conversion needed

3. **Intermediate Render Targets**: Use `R16G16B16A16_FLOAT`
   - Supports HDR values > 1.0 (required for physically correct lighting)
   - All lighting math (PBR, shadows, IBL) performed in linear space

4. **Final Output**: Use `R8G8B8A8_UNORM_SRGB`
   - PostProcessPass performs tone mapping (HDR → LDR in linear space)
   - GPU automatically applies gamma correction when writing to sRGB RT
   - No manual `pow(color, 1/2.2)` needed in shader

**Implementation Example:**

```cpp
// Core/TextureLoader.cpp
LoadTextureWIC(device, "albedo.png", srv, /*srgb=*/true);   // sRGB format
LoadTextureWIC(device, "normal.png", srv, /*srgb=*/false);  // Linear format

// Engine/Rendering/MainPass.cpp - ensureOffscreen()
td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR linear RT
ldrDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // LDR sRGB RT

// Engine/Rendering/PostProcessPass.cpp
// Shader outputs linear values, GPU converts to sRGB when writing to RT
float3 ldrColor = ACESFilm(hdrColor);  // Tone mapping (linear → linear)
return float4(ldrColor, 1.0);  // GPU applies gamma (linear → sRGB)
```

**Why This Matters:**

- **Physically Correct**: All lighting calculations in linear space (required for energy conservation)
- **HDR Support**: Intermediate HDR RTs allow bloom, exposure, and other post-effects
- **No Double Gamma**: Clear separation prevents accidental double/missing gamma correction
- **Performance**: GPU hardware sRGB conversion is free (no shader cost)

**Common Mistakes to Avoid:**

- ❌ Using `UNORM` for albedo textures (causes dark, incorrect colors)
- ❌ Using `UNORM_SRGB` for normal maps (causes incorrect normals)
- ❌ Doing tone mapping in intermediate passes (prevents HDR post-effects)
- ❌ Manual gamma correction when using `UNORM_SRGB` output (causes double gamma)

### IBL (Image-Based Lighting) System

**Architecture**: Offline generation of diffuse irradiance cubemaps from HDR environment maps.

**Components:**

1. **IBLGenerator** (`Engine/Rendering/IBLGenerator.h/cpp`):
   - `GenerateIrradianceMap(envMap, size)`: Convolves environment cubemap over hemisphere
   - `SaveIrradianceMapToDDS(path)`: Saves generated cubemap to DDS file with manual header writing
   - `GetIrradianceFaceSRV(faceIndex)`: Returns individual face SRV for debug visualization (0-5)

2. **Irradiance Convolution Shader** (`Shader/IrradianceConvolution.ps.hlsl`):
   - Hemisphere sampling: 144 samples in phi × 64 samples in theta = 9,216 samples per pixel
   - Lambert cosine-weighted integration: `color * cos(theta) * sin(theta)`
   - Output: 32×32 HDR cubemap (R16G16B16A16_FLOAT) per face

3. **Debug Visualization** (`Editor/Panels_IrradianceDebug.cpp`):
   - ImGui window displaying all 6 cubemap faces in cross layout
   - Real-time preview without file I/O
   - Adjustable display size (64-512 pixels)

**Key Implementation Details:**

**Cubemap Direction Mapping** (D3D11 Convention):
```hlsl
// GetCubemapDirection: UV [0,1] → 3D direction
// Critical: Texture V is top-to-bottom, World Y is bottom-to-top
float2 tc = uv * 2.0 - 1.0;  // [-1, 1]
// Must negate tc.y for most faces to match D3D11 right-handed coords:
case 0: dir = float3( 1.0, -tc.y, -tc.x); break;  // +X
case 4: dir = float3( tc.x, -tc.y,  1.0); break;  // +Z
// Exception: +Y/-Y faces don't negate the primary axis
case 2: dir = float3( tc.x,  1.0,  tc.y); break;  // +Y
```

**TBN Matrix Usage**:
```hlsl
float3x3 TBN = float3x3(T, B, N);  // Column vectors
float3 worldDir = mul(TBN, tangentDir);  // Matrix × column vector
// NEVER use mul(tangentDir, TBN) - incorrect axis mapping
```

**Hemisphere Integration**:
- Spherical coordinates: `phi ∈ [0, 2π]`, `theta ∈ [0, π/2]`
- Tangent space: `(sin(θ)cos(φ), sin(θ)sin(φ), cos(θ))`
- Solid angle weight: `cos(θ) * sin(θ) * dθ * dφ`
- Final normalization: `π * sum / sampleCount`

**Debug Workflow**:
1. File → Generate IBL (uses skybox environment map)
2. View "Irradiance Map Debug" window showing 6 faces
3. For direct sampling test: Uncomment debug code in shader main() to verify environment map binding
4. For quick iteration: Use low sample count (9×8=72) by uncommenting debug mode in ConvolveIrradiance()

**Current Limitations**:
- Generation is synchronous (blocks frame, ~1 second for 32×32)
- No mipmap generation (irradiance is pre-blurred, mips not needed)
- File saving uses manual DDS implementation (no DirectXTex dependency)
- Not yet integrated into MainPass PBR lighting (TODO)

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

**Architecture**: CSM (Cascaded Shadow Maps) with bounding sphere stabilization and texel snapping for anti-shimmer.

### **ShadowPass: Shadow Map Generation**

Located in `Engine/Rendering/ShadowPass.h/cpp`.

**Key Features**:
- Depth-only rendering from DirectionalLight's perspective
- Orthographic projection for parallel light rays
- **CSM Support**: 1-4 cascades with configurable split scheme
- **Bounding Sphere Stabilization**: Fixed projection bounds per cascade
- **Texel Snapping**: Eliminates shadow shimmer during camera movement
- Configurable shadow map resolution (1024/2048/4096)
- Default 1×1 white shadow map (depth=1.0) when no light exists

**Current Algorithm (Bounding Sphere + Texel Snapping)**:

Located in `ShadowPass::calculateTightLightMatrix()`:

1. **Calculate bounding sphere for frustum sub-section**:
   ```cpp
   BoundingSphere sphere = calculateBoundingSphere(frustumCornersWS);
   // sphere.radius is FIXED for each cascade (frustum size fixed by split distances)
   ```
   - Uses centroid as center, max distance as radius
   - Sphere radius is deterministic per cascade (independent of camera orientation)

2. **Build fixed light space basis** (only depends on light direction):
   ```cpp
   XMVECTOR lightRight = normalize(cross(worldUp, lightDir));
   XMVECTOR lightUp = cross(lightDir, lightRight);
   ```

3. **Texel snapping in world space**:
   - Project sphere center onto light-perpendicular plane:
     ```cpp
     float centerRight = dot(sphereCenter, lightRight);
     float centerUp = dot(sphereCenter, lightUp);
     ```
   - Align to texel grid (prevents sub-pixel jitter):
     ```cpp
     float texelSize = (sphere.radius * 2.0f) / shadowMapResolution;
     centerRight = floor(centerRight / texelSize) * texelSize;
     centerUp = floor(centerUp / texelSize) * texelSize;
     ```
   - Reconstruct aligned center (preserve depth component):
     ```cpp
     alignedCenter = lightRight * centerRight + lightUp * centerUp + lightDir * centerForward;
     ```

4. **Build light view matrix with aligned center**:
   ```cpp
   XMVECTOR lightPos = alignedCenter - lightDir * 100.0f;
   XMMATRIX lightView = LookAtLH(lightPos, alignedCenter, lightUp);
   ```

5. **Fixed orthographic projection** (using sphere diameter):
   ```cpp
   float orthoSize = sphere.radius * 2.0f;  // Fixed per cascade
   XMMATRIX lightProj = OrthographicOffCenterLH(-halfSize, halfSize, -halfSize, halfSize, minZ, maxZ);
   ```

**Why Bounding Sphere?**
- Traditional AABB: Changes size/shape with camera rotation → shimmer
- Bounding Sphere: Fixed radius → stable projection bounds
- Trade-off: ~27% wasted texels, but eliminates scale-induced shimmer

**Why Texel Snapping?**
- Problem: Camera movement causes sub-pixel shadow map shifts → edge flickering
- Solution: Quantize sphere center to texel grid in world space
- Effect: Shadow map "jumps" by 1 texel increments (imperceptible) instead of continuous sliding
- Key insight: Align sphere center in the plane **perpendicular to light direction**

**Texel Snapping Algorithm Details**:

The goal is to align the LookAt target (sphere center) to a texel grid in world space. Critical understanding:

1. **Why sphere center must align in world space, not light space**:
   - LookAt places target at (0,0,Z) in light space by definition
   - Cannot align something already at origin
   - Must align the world space position BEFORE constructing view matrix

2. **The perpendicular plane concept**:
   ```
   Light direction ↓
   ┌───────────────┐
   │ ░ ░ ░ ░ ░ ░ ░ │ ← Texel grid exists on this plane
   │ ░ ░ ● ░ ░ ░ ░ │   (perpendicular to light)
   │ ░ ░ ░ ░ ░ ░ ░ │
   └───────────────┘
   ```
   - Shadow map pixels correspond to positions on this plane
   - Movement parallel to light doesn't affect projection
   - Only movement on perpendicular plane causes shimmer

3. **Implementation steps**:
   ```cpp
   // Decompose sphere center into light-space basis components
   float centerRight = dot(sphereCenter, lightRight);   // X in light space
   float centerUp = dot(sphereCenter, lightUp);         // Y in light space
   float centerForward = dot(sphereCenter, lightDir);   // Z in light space

   // Quantize ONLY the XY components (perpendicular plane)
   centerRight = floor(centerRight / texelSize) * texelSize;
   centerUp = floor(centerUp / texelSize) * texelSize;
   // centerForward unchanged - doesn't affect projection

   // Reconstruct aligned world position
   alignedCenter = lightRight * centerRight
                 + lightUp * centerUp
                 + lightDir * centerForward;
   ```

4. **Why this works**:
   - Frame 1: Sphere at (10.42, 5.87, 20.0) → aligns to (10.40, 5.85, 20.0)
   - Frame 2: Sphere at (10.48, 5.91, 20.0) → aligns to (10.40, 5.85, 20.0) (same!)
   - Frame 3: Sphere at (10.56, 5.95, 20.0) → aligns to (10.53, 5.94, 20.0) (jumps 1 texel)
   - Result: View matrix stable for small movements, discrete jumps when threshold crossed

**Common Pitfalls**:
- ❌ Trying to align projection matrix directly when using LookAt (sphere always at origin)
- ❌ Aligning in clip space [-1,1] (view matrix already built, too late)
- ❌ Aligning along light direction (doesn't affect projection result)
- ✅ Align world space sphere center on perpendicular plane BEFORE LookAt

**CSM Cascade Calculation**:
```cpp
// Practical Split Scheme (GPU Gems 3, Ch.10)
splits[i] = lambda * logSplit + (1-lambda) * uniformSplit;
```
- `lambda = 0`: Uniform distribution (wastes resolution at distance)
- `lambda = 1`: Logarithmic distribution (matches perspective aliasing)
- `lambda = 0.5-0.9`: Balanced (typical in production)

**Pass Output Bundle**:
```cpp
struct Output {
    int cascadeCount;                              // Active cascades (1-4)
    float cascadeSplits[4];                        // Split distances
    ID3D11ShaderResourceView* shadowMapArray;      // Texture2DArray SRV
    XMMATRIX lightSpaceVPs[4];                     // Per-cascade VP matrices
    ID3D11SamplerState* shadowSampler;             // Comparison sampler (PCF)
    float cascadeBlendRange;                       // Blend at seams
    bool debugShowCascades;                        // Visualize cascade levels
    bool enableSoftShadows;                        // PCF toggle
};
```

### **MainPass: Shadow Application**

**Pixel Shader CSM Sampling**:
- Select cascade based on pixel depth in camera space
- Transform to appropriate light space
- Sample from Texture2DArray slice
- Optional cascade blending at boundaries
- PCF (Percentage Closer Filtering) for soft shadows

**Shader Integration**:
```hlsl
// Select cascade based on depth
int cascadeIndex = SelectCascade(pixelDepth, gCascadeSplits);

// Transform to light space
float4 posLS = mul(float4(posWS, 1.0), gLightSpaceVPs[cascadeIndex]);

// Sample shadow map array
float shadow = gShadowMapArray.SampleCmpLevelZero(gShadowSampler,
                    float3(uv, cascadeIndex), depth);

// Apply PCF if enabled
if (gEnableSoftShadows) {
    shadow = PCF3x3(posLS, cascadeIndex);
}
```

### **Current Status & Known Limitations**

**Implemented**:
- ✅ CSM with 1-4 configurable cascades
- ✅ Bounding sphere stabilization (fixed projection bounds)
- ✅ Texel snapping (world space alignment)
- ✅ Practical split scheme with lambda parameter
- ✅ Texture2DArray for efficient cascade storage
- ✅ PCF soft shadows with 3×3 kernel

**Current Limitations**:
1. **Bounding sphere inefficiency**: Wastes ~27% texels compared to tight AABB
   - Acceptable trade-off for stability
   - Could implement "stable AABB" (quantized bounds) as alternative

2. **Near plane clipping**: Tall objects may clip if they extend beyond frustum
   - Current mitigation: `ShadowNearPlaneOffset` parameter extends near plane
   - Proper fix: Include scene AABB in Z bounds calculation

3. **No cascade blending**: Hard transitions between cascade levels
   - Parameters defined but not implemented in shader
   - Future: Blend between adjacent cascades at boundaries

4. **Fixed light space basis**: Light rotation causes basis flip when near vertical
   - Current mitigation: Switch up vector when light is near vertical (>0.99)
   - Could improve with stable basis selection

**Future Improvements**:
- Implement cascade blending for smooth transitions
- Optimize bounding sphere (Ritter's algorithm for tighter fit)
- Add cascade visualization debug mode
- Implement stable AABB as alternative to sphere
- Add shadow fade-out at max distance

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

**Current Panels**:
- `DrawDockspace`: Main docking container with File menu (Save/Load Scene, Generate IBL)
- `DrawHierarchy`: GameObject tree view with selection
- `DrawInspector`: Component property editor with reflection-based UI
- `DrawViewport`: 3D scene view using offscreen render target
- `DrawIrradianceDebug`: Debug visualization of generated IBL irradiance cubemap (6 faces in cross layout)

**To add a new panel**:

1. Add function declaration to `Editor/Panels.h`
2. Implement in `Editor/Panels_PanelName.cpp`
3. Add cpp file to EDITOR_SRC in CMakeLists.txt
4. Call from main loop after `ImGui::NewFrame()`

Panels receive `Scene&` to access World and selection state. Some panels (e.g., DrawIrradianceDebug) receive specific subsystem pointers (e.g., `IBLGenerator*`).
