# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Vision

This is a mid-sized game engine and editor project, targeting functionality similar to Unity/Unreal but at a smaller scale. The goal is to create a complete game editor that can support development of several fixed game types (e.g., 3D action, puzzle, platformer).

**Current Status**: Early development with basic foundation in place
- Entity-Component-System architecture established
- Editor UI with Hierarchy, Inspector, Viewport, and Debug panels
- Basic 3D rendering with OBJ/glTF model loading
- PBR rendering with Cook-Torrance BRDF (direct lighting)
- CSM shadow mapping with bounding sphere stabilization and texel snapping
- IBL diffuse irradiance map generation (Lambert cosine-weighted hemisphere convolution)
- IBL specular pre-filtered map generation (GGX importance sampling with Split Sum Approximation)
- Scene serialization with auto-registration

**Development Philosophy**:
- Keep the architecture clean and maintainable
- Focus on practical game development workflows
- Prioritize editor usability and iteration speed
- Build incrementally with working features at each step

---

## Coding Standards

### Coordinate System Convention

**CRITICAL RULE**: This engine uses **DirectX left-handed coordinate system** throughout the entire codebase.

**World Space Coordinate Axes**:
- **+X**: Right
- **+Y**: Up
- **+Z**: Forward (into the screen)

**Texture Coordinate (UV) Convention**:
- **Origin**: Top-left corner (0, 0)
- **U-axis**: Left-to-right (0 → 1)
- **V-axis**: Top-to-bottom (0 → 1)
- This is DirectX's native convention (differs from OpenGL which uses bottom-left origin)

**Key Implications**:
- All matrix operations use left-handed conventions (e.g., `XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`)
- Cross product order: `Right = Cross(Up, Forward)` (not `Cross(Forward, Up)`)
- Rotation conventions follow left-hand rule (thumb points along axis, fingers curl in positive rotation direction)
- Cubemap face directions and texture coordinate mappings follow DirectX conventions
- When sampling textures in shaders, V=0 corresponds to the top of the image

**Consistency Requirements**:
- ALL spatial calculations, transforms, and vector operations must respect this convention
- When interfacing with external tools (Blender, Maya), ensure asset export uses left-handed Y-up convention
- Shader code (`HLSL`) must use consistent coordinate system for normals, tangents, and lighting calculations

---

### Logging and Debug Output

**CRITICAL RULE**: All debug output and logging must go to the **console window** (`std::cout`, `std::cerr`), NOT to Visual Studio's output window.

**Correct**:
```cpp
std::cout << "IBL: Starting generation..." << std::endl;
std::cerr << "ERROR: Failed to load shader!" << std::endl;
```

**Incorrect** (DO NOT USE):
```cpp
OutputDebugStringA("Debug message");  // ❌ Only visible in VS debugger
printf("Message");                     // ❌ May not show in console
```

**Rationale**:
- Console output is visible to end users running the executable
- VS output window is only available during debugging in Visual Studio
- Consistent logging location simplifies troubleshooting
- Console output can be redirected to log files

**Exception**: Shader compilation errors use `OutputDebugStringA` for detailed HLSL errors in VS output during development, but critical errors should also log to console.

### Component Auto-Registration

Components automatically register using the `REGISTER_COMPONENT(ComponentType)` macro:

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

**Benefits**:
- No manual updates to serialization code needed
- Automatic Inspector UI generation via reflection
- Automatic JSON save/load support

---

## Build Commands & Paths

This is a Windows-only DX11 project using CMake with Ninja generator.

```bash
# Configure (from repository root)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --target forfun

# Run
./build/forfun.exe
```

### Important Path Assumptions

- **Source code**: `E:/forfun/source/code`
- **Third-party**: `E:/forfun/thirdparty`
- **Assets**: `E:/forfun/assets` (hardcoded in `main.cpp::ForceWorkDir()`)

All asset paths (OBJ, glTF, textures) are relative to the assets directory.

### Third-Party Dependencies

- **imgui_docking**: Static library (`${THIRD_PARTY_PATH}/imgui`, docking branch)
- **cgltf**: Header-only glTF loader (`${THIRD_PARTY_PATH}/cgltf-master`)
- **nlohmann/json**: Header-only JSON library (`${THIRD_PARTY_PATH}/nlohmann/json.hpp`)
  - Download: https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
- **DirectX 11, d3dcompiler** (Windows SDK)

---

## Architecture Overview

### Three-Layer Separation

1. **Core/** - Low-level device management and resource loading
   - `DX11Context`: Singleton managing D3D11 device, context, swapchain
   - `MeshResourceManager`: Singleton for loading/caching mesh resources (OBJ/glTF)
   - `GpuMeshResource`: RAII wrapper for GPU mesh data (VBO, IBO, textures)
   - **Does NOT depend on Engine or Editor layers**

2. **Engine/** - Entity-component-system, scene management, rendering
   - `World`: Container managing all GameObjects
   - `GameObject`: Named entities that own Components
   - `Component`: Base class for all components
   - `Scene`: Combines World with selection state for editor
   - `Rendering/MainPass`: Main rendering pass (PBR shaders, scene traversal)
   - `Rendering/ShadowPass`: CSM shadow map generation
   - `Rendering/Skybox`: HDR skybox from equirectangular maps with mipmaps
   - `Rendering/IBLGenerator`: Diffuse irradiance and specular pre-filtered map generation

3. **Editor/** - ImGui-based editor UI
   - `Panels.h`: Interface for all panels
   - Each panel in separate `.cpp` file
   - Viewport renders offscreen RT from MainPass

### Component System

**Built-in Components**:
- `Transform`: Position, rotation (euler), scale; provides `WorldMatrix()`
- `MeshRenderer`: Mesh file path, owns `shared_ptr<GpuMeshResource>`, lazy loading via `EnsureUploaded()`
- `Material`: PBR properties (Albedo, Metallic, Roughness)
- `DirectionalLight`: Directional light with CSM shadow support

**Adding New Components**:

1. Create `Engine/Components/YourComponent.h`
2. Inherit from `Component`, implement `GetTypeName()`
3. Add `#include "ComponentRegistry.h"` and `REGISTER_COMPONENT(YourComponent)`
4. Add to `CMakeLists.txt` ENGINE_SOURCES
5. Use `GameObject::AddComponent<T>()` and `GameObject::GetComponent<T>()`

**Example**:
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

### Reflection System

Components use **Visitor Pattern** for reflection:
- `PropertyVisitor`: Abstract interface defining property types (Float, Int, Bool, String, Float3, Enum)
- `Component::VisitProperties()`: Override to expose properties to editor/serialization
- `ImGuiPropertyVisitor`: Renders properties in Inspector
- `JsonWriteVisitor`/`JsonReadVisitor`: Serialize/deserialize to JSON

### Scene Serialization

Scenes save to JSON format (`.scene` files):
- **SceneSerializer**: Handles save/load using ComponentRegistry
- **Auto-registration**: Components auto-register via `REGISTER_COMPONENT` macro
- **File Menu**: Save Scene (Ctrl+S), Load Scene (Ctrl+O)

**JSON Structure**:
```json
{
  "version": "1.0",
  "gameObjects": [
    {
      "name": "Cube",
      "components": [
        {"type": "Transform", "Position": [0,0.5,0], "Rotation": [0,0,0], "Scale": [1,1,1]},
        {"type": "MeshRenderer", "Path": "mesh/cube.obj"},
        {"type": "Material", "Albedo": [1,1,1], "Metallic": 0.0, "Roughness": 0.5}
      ]
    }
  ]
}
```

---

## Rendering Pipeline

### Frame Rendering Flow

```cpp
// Each frame in main.cpp:
// 1. Update camera
gMainPass.UpdateCamera(vpWidth, vpHeight, dt);

// 2. Render shadow map (if DirectionalLight exists)
if (dirLight) {
    gShadowPass.Render(gScene, dirLight,
                      gMainPass.GetCameraViewMatrix(),
                      gMainPass.GetCameraProjMatrix());
}

// 3. Render scene with shadows
gMainPass.Render(gScene, vpWidth, vpHeight, dt, &gShadowPass.GetOutput());

// 4. Display in viewport
DrawViewport(gMainPass.GetOffscreenSRV(), ...);
```

### Color Space Management

**CRITICAL**: Strict color space separation for physically correct rendering.

**Pipeline Flow**:
```
Texture Input (Albedo)
  ↓ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
  ↓ GPU auto-converts sRGB → Linear
  ↓
All Intermediate Passes (MainPass, ShadowPass, Skybox)
  ↓ DXGI_FORMAT_R16G16B16A16_FLOAT (HDR Linear)
  ↓ All lighting calculations in linear space
  ↓
PostProcessPass
  ↓ Tone Mapping (HDR → LDR, still linear)
  ↓ Output to DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
  ↓ GPU auto-applies Gamma correction
  ↓
Final Display (LDR sRGB)
```

**Key Rules**:
1. **Albedo/Emissive textures**: Use `UNORM_SRGB` format (GPU converts to linear on sample)
2. **Normal/Metallic/Roughness/AO**: Use `UNORM` format (linear data, no conversion)
3. **Intermediate RTs**: Use `R16G16B16A16_FLOAT` (HDR linear space)
4. **Final output**: Use `R8G8B8A8_UNORM_SRGB` (GPU applies gamma on write)

**Common Mistakes to Avoid**:
- ❌ Using `UNORM` for albedo textures (causes dark colors)
- ❌ Using `UNORM_SRGB` for normal maps (causes incorrect normals)
- ❌ Manual gamma correction when using `UNORM_SRGB` output (double gamma)

### PBR Shaders

**MainPass** (`Shader/MainPass.vs.hlsl`, `MainPass.ps.hlsl`):
- Cook-Torrance BRDF: GGX normal distribution, Schlick-GGX geometry, Fresnel-Schlick
- Energy conservation: `kD = (1 - kS) * (1 - metallic)`
- Constant buffers: `CB_Frame` (camera, light, CSM data), `CB_Object` (world matrix, material)
- External shader files for easy iteration

---

## Shadow System (CSM)

**Architecture**: Cascaded Shadow Maps with bounding sphere stabilization and texel snapping.

### Key Features

- **CSM Support**: 1-4 configurable cascades
- **Bounding Sphere Stabilization**: Fixed projection bounds per cascade (eliminates rotation shimmer)
- **Texel Snapping**: Quantizes sphere center to texel grid (eliminates movement shimmer)
- **Configurable Resolution**: 1024/2048/4096
- **PCF Soft Shadows**: 3×3 kernel

### DirectionalLight Parameters

**Shadow Map**:
- `ShadowMapSizeIndex` (0/1/2): Resolution 1024/2048/4096
- `ShadowDistance`: Maximum shadow visible distance from camera
- `ShadowBias`: Depth bias to prevent shadow acne
- `ShadowNearPlaneOffset`: Extends near plane to capture tall objects
- `EnableSoftShadows`: Toggle PCF filtering

**CSM**:
- `CascadeCount` (1-4): Number of cascades
- `CascadeSplitLambda` (0-1): Split scheme (0=uniform, 1=logarithmic, 0.5-0.9=balanced)
- `CascadeBlendRange` (0-0.5): Blend distance at cascade boundaries
- `DebugShowCascades`: Visualize cascade levels with color coding

### Implementation Details

**Bounding Sphere Stabilization**:
- Traditional AABB changes size/shape with camera rotation → shimmer
- Bounding sphere has fixed radius → stable projection bounds
- Trade-off: ~27% wasted texels, but eliminates scale-induced shimmer

**Texel Snapping**:
1. Project sphere center onto light-perpendicular plane
2. Quantize to texel grid: `centerAligned = floor(center / texelSize) * texelSize`
3. Reconstruct aligned world position BEFORE building LookAt matrix
4. Shadow map "jumps" by 1 texel increments (imperceptible) instead of continuous sliding

**CSM Split Calculation** (GPU Gems 3, Ch.10):
```cpp
splits[i] = lambda * logSplit + (1-lambda) * uniformSplit;
```

### Shader Integration

```hlsl
// Select cascade based on pixel depth
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

---

## IBL (Image-Based Lighting) System

**Architecture**: Offline generation of diffuse irradiance cubemaps from HDR environment maps.

### Components

1. **IBLGenerator** (`Engine/Rendering/IBLGenerator.h/cpp`):
   - `GenerateIrradianceMap(envMap, size)`: Convolves environment cubemap over hemisphere
   - `SaveIrradianceMapToDDS(path)`: Saves to DDS file
   - `GetIrradianceFaceSRV(faceIndex)`: Returns individual face SRV for debug (0-5)

2. **Irradiance Convolution Shader** (`Shader/IrradianceConvolution.ps.hlsl`):
   - **Uniform Solid Angle Sampling**: Ensures even distribution across hemisphere
   - **Integer Loop**: Avoids floating-point accumulation errors
   - **Sample Quality**: Configurable (64×32=2K samples to 512×256=131K samples)
   - **Lambert Cosine Weighting**: Physically correct diffuse integration

3. **Pre-Filtered Environment Map** (`Engine/Rendering/IBLGenerator.cpp`, `Shader/PreFilterEnvironmentMap.ps.hlsl`):
   - Implements **Split Sum Approximation** for specular IBL (UE4 approach)
   - `GeneratePreFilteredMap(envMap, size, numMipLevels)`: Generates pre-convolved specular map
   - **GGX Importance Sampling**: Uses Hammersley low-discrepancy sequence
   - **Dynamic Sample Count**: 8K-65K samples based on roughness (low roughness needs more samples)
   - **Solid Angle Based Mip Selection**: Matches sample cone to texel cone for environment map sampling
   - Output: 128×128 cubemap with 7 mip levels (roughness 0.0 → 1.0)

4. **Debug UI** (`Editor/Panels_IrradianceDebug.cpp`):
   - Three tabs in unified window:
     - **Tab 1: Irradiance Map** (diffuse IBL, 32×32 single level)
     - **Tab 2: Pre-Filtered Map** (specular IBL, 128×128 with mip 0-6 selection)
     - **Tab 3: Environment Map** (source HDR cubemap, mip 0-9 selection)
   - Real-time preview of all 6 cubemap faces in cross layout
   - Adjustable display size (64-512 pixels)
   - No file I/O needed for visualization

### Key Implementation Details

**Uniform Solid Angle Sampling** (Critical for smooth results):
```hlsl
// Uniform solid angle distribution (NOT uniform theta/phi)
float phi = u * 2.0 * PI;
float cosTheta = 1.0 - v;  // Linear in cos(θ) → uniform solid angle
float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

// Weight is just cosTheta (solid angle already uniform)
irradiance += color * cosTheta;
```

**Why This Matters**:
- Uniform θ sampling causes uneven solid angle distribution
- Results in sparse sampling at horizon → visible artifacts/banding
- Uniform solid angle sampling ensures smooth, artifact-free results

**TBN Matrix Construction**:
```hlsl
// CRITICAL: HLSL float3x3(v1,v2,v3) creates ROW-major matrix
// We need COLUMN-major for tangent→world transform
// Solution: Use transpose()
float3x3 TBN = transpose(float3x3(T, B, N));
float3 worldDir = mul(TBN, tangentDir);
```

### Pre-Filtered Map Implementation

**Split Sum Approximation Theory**:

The specular IBL integral can be approximated as a product of two parts:
```
∫ Li(l) * BRDF(l,v) * cos(θ) dl ≈ (∫ Li(l) * D(h) * cos(θ) dl) × (∫ BRDF(l,v) * cos(θ) dl)
                                    └─────────────────────────┘   └─────────────────────┘
                                      Pre-filtered Environment         BRDF LUT
                                      (depends on roughness, N)      (depends on NdotV, roughness)
```

**Key Assumptions**:
- Environment lighting is slowly varying (low frequency)
- Fresnel can be factored out using Schlick approximation
- Works best for distant environment maps

**Failure Cases**:
- High-frequency environment (e.g., point lights) + low roughness → noise
- Grazing angles (NdotV ≈ 0) → reduced accuracy
- Anisotropic materials → not supported

**GGX Importance Sampling** (`PreFilterEnvironmentMap.ps.hlsl`):

```hlsl
// 1. Generate low-discrepancy 2D samples (Hammersley sequence)
float2 Xi = Hammersley(i, SAMPLE_COUNT);  // Van der Corput radical inverse

// 2. Importance sample GGX to get half-vector H in tangent space
float3 ImportanceSampleGGX(float2 Xi, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// 3. Transform H to world space using TBN matrix
float3 H = normalize(mul(TBN, H_tangent));

// 4. Calculate light direction L (reflection of V around H)
float3 L = normalize(2.0 * dot(V, H) * H - V);
```

**Dynamic Sample Count** (Reduces noise for low roughness):
```hlsl
uint GetSampleCount(float roughness) {
    if (roughness < 0.1)  return 65536u;  // Mirror-like: maximum samples
    if (roughness < 0.3)  return 32768u;  // Very smooth: high samples
    if (roughness < 0.6)  return 16384u;  // Medium: moderate samples
    return 8192u;                          // Rough: converges faster
}
```

**Rationale**: Low roughness → narrow GGX lobe → few samples hit bright features (e.g., sun) → high variance → need more samples.

**Solid Angle Based Mip Level Selection**:

When sampling the environment map during pre-filtering, we calculate an adaptive mip level for each sample to avoid aliasing:

```hlsl
// Calculate GGX PDF for this sample
float D = D_GGX(NdotH, roughness);
float pdf = D * NdotH / (4.0 * VdotH);  // GGX PDF in 1/sr

// Compare sample solid angle to texel solid angle
float saTexel = 4.0 * PI / (6.0 * envResolution * envResolution);  // One texel's solid angle
float saSample = 1.0 / (pdf * SAMPLE_COUNT);                        // This sample's solid angle

// Derive mip level from solid angle ratio (mathematical, not empirical)
float mipLevel = 0.5 * log2(saSample / saTexel);
```

**Mathematical Derivation**:
1. Mipmap level k has texel solid angle: `saTexel(k) = saTexel(0) × 4^k`
2. Match sample solid angle to texel solid angle: `saTexel(k) = saSample`
3. Solve for k: `4^k = saSample / saTexel(0)` → `k = 0.5 × log2(saSample / saTexel(0))`

**Physical Interpretation**:
- `saSample = 1/(pdf × N)`: The solid angle (in steradians) this sample "represents" on the hemisphere
  - High pdf (mirror-like) → small solid angle → sample from high-res mip
  - Low pdf (rough) → large solid angle → sample from low-res mip
- `saTexel`: The solid angle one environment map texel covers
- **Nyquist matching**: Sample cone should match texel cone to avoid aliasing

**Special Case Handling**:
```hlsl
if (roughness < 0.02) {
    mipLevel = 0.0;  // Force highest resolution
}
```

**Reason**: When roughness→0, GGX becomes Dirac delta, `pdf→∞`, `saSample→0`, formula breaks down. For perfect mirrors, directly sample mip 0.

**Monte Carlo Integration**:
```hlsl
// Accumulate with cosine weighting
if (dot(N, L) > 0.0) {
    float3 color = envMap.SampleLevel(sampler, L, mipLevel).rgb;
    prefilteredColor += color * dot(N, L);
    totalWeight += dot(N, L);
}

// Normalize
prefilteredColor /= totalWeight;
```

**Roughness Mapping**:
- Linear mapping: `roughness = mipLevel / (numMipLevels - 1)` ∈ [0, 1]
- Mip 0 = perfect mirror (roughness = 0.0)
- Mip 6 = fully rough (roughness = 1.0)

**Note**: Unity/UE use perceptual roughness (squared mapping), but current implementation uses linear for simplicity. Can add `roughness = perceptualRoughness²` if needed.

### Debug Workflow

**Irradiance Map (Diffuse IBL)**:
1. Open "Irradiance Map Debug" window → Tab 1
2. Click "Generate Irradiance Map" button
3. View 6 cubemap faces in cross layout
4. Verify results are smooth and artifact-free

**Pre-Filtered Map (Specular IBL)**:
1. File → Generate IBL (generates both irradiance and pre-filtered maps)
2. Switch to Tab 2 "Pre-Filtered Map"
3. Use "Mip Level" slider to view roughness levels 0-6
   - Mip 0: Mirror-like reflection (should show sharp sun)
   - Mip 3: Medium roughness (sun should be blurred)
   - Mip 6: Fully rough (completely diffuse look)
4. Check for noise artifacts in mip 1-3 around bright features

**Environment Map Verification**:
1. Switch to Tab 3 "Environment Map"
2. Verify source HDR cubemap has proper mipmaps (mip 0-9)
3. Check mip 0 shows clear details, higher mips progressively blur

**Current Limitations**:
- Generation is synchronous (blocks frame, ~1-10 seconds depending on sample count and mip levels)
- Pre-filtered map generation with 7 mip levels takes ~30-60 seconds (65K samples for low roughness)
- No asynchronous/progressive generation
- Not yet integrated into MainPass PBR lighting (TODO: add specular IBL term)

---

## Editor Panel Integration

**Current Panels**:
- `DrawDockspace`: Main docking container with File menu (Save/Load Scene)
- `DrawHierarchy`: GameObject tree view with selection
- `DrawInspector`: Component property editor with reflection-based UI
- `DrawViewport`: 3D scene view using offscreen render target
- `DrawIrradianceDebug`: IBL generation and debug visualization

**Adding New Panels**:
1. Add function declaration to `Editor/Panels.h`
2. Implement in `Editor/Panels_PanelName.cpp`
3. Add cpp file to `EDITOR_SRC` in CMakeLists.txt
4. Call from main loop after `ImGui::NewFrame()`

Panels receive `Scene&` to access World and selection state. Some panels receive specific subsystem pointers (e.g., `IBLGenerator*`, `MainPass*`).
