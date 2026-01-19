# FSR 2.0 (AMD FidelityFX Super Resolution 2)

## Overview

FSR 2.0 is AMD's open-source temporal upscaling solution that provides high-quality image reconstruction from lower-resolution input. Unlike DLSS, FSR 2.0 works on all GPUs (not limited to NVIDIA RTX).

**Key Benefits**:
- Works on any GPU (AMD, NVIDIA, Intel)
- Open source SDK (source-based integration)
- Temporal anti-aliasing + upscaling in one pass
- Replaces TAA when enabled

**Limitations**:
- DX12 backend only (DX11 not supported)
- Requires motion vectors, depth, and jittered rendering
- Slightly higher computational cost than native TAA

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    FSR 2.0 Integration                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────┐    ┌──────────────────┐                   │
│  │  SFSR2Settings  │───►│    CFSR2Pass     │                   │
│  │  (UI Config)    │    │  (Orchestration) │                   │
│  └─────────────────┘    └────────┬─────────┘                   │
│                                  │                              │
│                                  ▼                              │
│                         ┌────────────────┐                      │
│                         │  CFSR2Context  │                      │
│                         │ (SDK Wrapper)  │                      │
│                         └────────┬───────┘                      │
│                                  │                              │
│                                  ▼                              │
│                         ┌────────────────┐                      │
│                         │  FFX FSR2 SDK  │                      │
│                         │   (Optional)   │                      │
│                         └────────────────┘                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Quality Modes

| Mode | Scale | Render Resolution (at 1080p) | Use Case |
|------|-------|------------------------------|----------|
| NativeAA | 1.0x | 1920×1080 | Temporal AA only, no upscaling |
| Quality | 1.5x | 1280×720 | Best quality with performance gain |
| Balanced | 1.7x | 1129×635 | Balance of quality and performance |
| Performance | 2.0x | 960×540 | High performance, good quality |
| UltraPerformance | 3.0x | 640×360 | Maximum performance, acceptable quality |

## Files

### Core Implementation

| File | Purpose |
|------|---------|
| `Engine/Rendering/FSR2Context.h` | SDK wrapper header (opaque pointer pattern) |
| `Engine/Rendering/FSR2Context.cpp` | SDK integration with conditional compilation |
| `Engine/Rendering/FSR2Pass.h` | Pass orchestration header |
| `Engine/Rendering/FSR2Pass.cpp` | Pass orchestration implementation |
| `Engine/SceneLightSettings.h` | EFSR2QualityMode enum + SFSR2Settings struct |

### Integration Points

| File | Changes |
|------|---------|
| `CMakeLists.txt` | FSR2 SDK library + conditional FSR2_AVAILABLE macro |
| `Engine/Camera.h/cpp` | Custom jitter method for FSR2 |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | FSR2 pass integration |
| `Editor/Panels_SceneLightSettings.cpp` | FSR2 UI section |
| `Tests/TestFSR2.cpp` | Automated test case |

## Conditional Compilation

FSR 2.0 uses a conditional compilation system to handle cases where the SDK is not built:

```cpp
#if FSR2_AVAILABLE
// Full FSR2 SDK implementation
#include "ffx_fsr2.h"
#include "ffx_fsr2_dx12.h"
// ... full implementation
#else
// Stub implementation
bool CFSR2Context::IsSupported() { return false; }
bool CFSR2Context::Initialize(...) {
    CFFLog::Warning("[FSR2] FSR 2.0 SDK not available.");
    return false;
}
#endif
```

The `FSR2_AVAILABLE` macro is set by CMake based on whether the FSR2 SDK shader permutation headers exist.

## Pipeline Integration

FSR 2.0 integrates into the post-processing pipeline as follows:

```
G-Buffer Pass
    ↓
Deferred Lighting
    ↓
SSAO / SSR
    ↓
┌─────────────────────────────┐
│ FSR 2.0 (if enabled)        │  ← Replaces TAA
│   OR                        │
│ TAA (fallback)              │
└─────────────────────────────┘
    ↓
Auto Exposure
    ↓
Bloom
    ↓
Depth of Field
    ↓
Motion Blur
    ↓
Tone Mapping
    ↓
FXAA/SMAA (optional)
```

## Input Requirements

FSR 2.0 requires the following inputs:

1. **Color Buffer** (HDR, render resolution)
   - Pre-tone-mapped HDR color
   - With camera jitter applied

2. **Depth Buffer** (render resolution)
   - Reversed-Z format (near=1, far=0)
   - Full precision preferred

3. **Motion Vectors** (render resolution)
   - Screen-space pixels
   - From G-Buffer RT4 (velocity buffer)

4. **Camera Parameters**
   - Near/far planes
   - Vertical FOV
   - Delta time in milliseconds
   - Jitter offset (NDC)

## Usage

### Enabling FSR 2.0

1. In the Editor, open Scene Light Settings panel
2. Under "FSR 2.0" section, check "Enable"
3. Select quality mode (Quality recommended for best balance)
4. Adjust sharpness (0.0-1.0, default 0.5)

TAA is automatically disabled when FSR 2.0 is enabled.

### Programmatic Control

```cpp
auto& settings = CScene::Instance().GetLightSettings();
settings.fsr2.enabled = true;
settings.fsr2.qualityMode = EFSR2QualityMode::Quality;
settings.fsr2.sharpness = 0.5f;
```

## Building the FSR2 SDK

If FSR 2.0 shows as "Not Supported", you need to build the SDK:

```bash
cd E:/forfun/thirdparty/fsr2/build
GenerateSolutions.bat
cmake --build .
```

After building, reconfigure and rebuild the main project:

```bash
cd E:/forfun/source/code
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun
```

## Known Limitations

1. **DX12 Only**: FSR 2.0 integration only works with the DX12 backend
2. **Object Motion Vectors**: Currently uses camera-only motion vectors; object motion requires worldPrev tracking (deferred)
3. **Reactive Mask**: Not implemented; some transparent objects may ghost

## Test Case

Run the FSR 2.0 test with:

```bash
./build/Debug/forfun.exe --test TestFSR2
```

The test verifies:
- DX12 backend detection
- FSR2 initialization with different quality modes
- Context creation and destruction
- Visual output at NativeAA, Quality, and Performance modes

## References

- [AMD FidelityFX FSR 2.0](https://gpuopen.com/fidelityfx-superresolution-2/)
- [FSR 2.0 SDK GitHub](https://github.com/GPUOpen-Effects/FidelityFX-FSR2)
- [FSR 2.0 Integration Guide](https://gpuopen.com/manuals/fidelityfx_sdk/fidelityfx_sdk-page_techniques_super-resolution-temporal/)

---

**Last Updated**: 2026-01-19
