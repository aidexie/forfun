# ForFun Engine

A mid-scale game engine and editor for Windows, inspired by Unity and Unreal Engine.

## Features

### Rendering
- **Dual RHI Backend**: DX11 (stable) and DX12 (default) with runtime switching
- **Two Pipelines**: Forward+ and Deferred rendering
- **PBR**: Cook-Torrance BRDF with energy conservation
- **Shadows**: Cascaded Shadow Maps (1-4 cascades, PCF filtering)
- **IBL**: Diffuse irradiance + specular pre-filtered cubemaps

### Lighting
- **Clustered Forward+**: 100+ dynamic lights @ 60 FPS
- **Volumetric Lightmap**: Adaptive octree, GPU DXR baking, per-pixel GI
- **2D Lightmap**: Path tracing with Intel OIDN denoising
- **Reflection Probes**: TextureCubeArray with box projection

### Post-Processing
- Bloom (HDR, dual Kawase blur, ACES tonemapping)
- SSAO (GTAO, HBAO, Crytek algorithms)
- SSR (HiZ trace, stochastic, temporal modes)
- TAA (6 quality levels, Halton jitter)
- Motion Blur, Depth of Field, Auto Exposure
- FSR 2.0 (AMD FidelityFX temporal upscaling)
- FXAA/SMAA, Color Grading (3D LUT)

### Ray Tracing (DXR)
- Cubemap baking for reflection probes
- Path tracing for lightmap baking
- Volumetric lightmap GPU baking

### Editor
- ImGui docking with professional multi-panel layout
- Hierarchy, Inspector, Viewport, Scene Light Settings
- Material Editor with real-time preview
- Transform Gizmo (ImGuizmo: W/E/R for Translate/Rotate/Scale)

## Build

**Requirements**: Windows, CMake, Ninja

```bash
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun
./build/Debug/forfun.exe
```

## Backend Selection

Configure via `render.json` in project root:

```json
{
  "renderBackend": "DX12"
}
```

Options: `"DX11"` (stable) | `"DX12"` (default)

## Project Structure

```
Core/           # Infrastructure (RHI, loaders, testing, logging)
RHI/            # Rendering Hardware Interface (DX11/DX12 abstraction)
  DX11/         # DirectX 11 backend
  DX12/         # DirectX 12 backend
Engine/         # ECS, components, rendering passes
  Components/   # Transform, MeshRenderer, Lights, etc.
  Rendering/    # Forward, Deferred, Post-processing passes
Editor/         # ImGui panels (Hierarchy, Inspector, Viewport)
Shader/         # HLSL shaders (58 files)
  DXR/          # Ray tracing shaders
Tests/          # 36 automated tests
```

## Testing

```bash
# Run a specific test
./build/Debug/forfun.exe --test TestGBuffer

# List all tests
./build/Debug/forfun.exe --list-tests
```

Test output: `E:/forfun/debug/{TestName}/runtime.log`

## Dependencies

- [ImGui](https://github.com/ocornut/imgui) (docking branch) - UI framework
- [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) - Transform gizmos
- [cgltf](https://github.com/jkuhlmann/cgltf) - GLTF parsing
- [nlohmann/json](https://github.com/nlohmann/json) - JSON serialization
- [xatlas](https://github.com/jpcy/xatlas) - UV2 generation for lightmaps
- [Intel OIDN](https://github.com/OpenImageDenoise/oidn) - AI denoising
- [KTX-Software](https://github.com/KhronosGroup/KTX-Software) - Texture format support
- [AMD FSR2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2) - Temporal upscaling
- DirectX 11/12 - Graphics APIs

## Documentation

- [CLAUDE.md](CLAUDE.md) - AI working guidelines
- [CODING_STYLE.md](CODING_STYLE.md) - Naming conventions
- [ROADMAP.md](ROADMAP.md) - Development roadmap
- [docs/RENDERING.md](docs/RENDERING.md) - Rendering system details
- [docs/EDITOR.md](docs/EDITOR.md) - Editor system details
- [docs/TESTING.md](docs/TESTING.md) - Testing framework details
