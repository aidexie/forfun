# DXR GPU Baking Verification Roadmap

This document describes the step-by-step verification process for the DXR-based GPU lightmap baking feature using NVIDIA Nsight Graphics.

## Debug Flags

Enable specific verification phases via `SDXRDebugFlags` in code:

```cpp
// In DXRLightmapBaker.h
struct SDXRDebugFlags {
    bool enableCaptureDelay = true;      // Phase 0: 5-second pause for Nsight capture
    bool logAccelerationStructure = true; // Phase 1: Log BLAS/TLAS info
    bool logPipelineCreation = true;      // Phase 2: Log pipeline/SBT creation
    bool logResourceBinding = true;       // Phase 3: Log resource binding
    bool logDispatchInfo = true;          // Phase 4: Log DispatchRays params
    bool logReadbackResults = true;       // Phase 5: Log SH output values
    bool pauseAfterFirstBrick = false;    // Phase 6: Pause after first brick for inspection
};
```

---

## Phase 0: Nsight Capture Setup

### Goal
Capture the exact frame containing DXR dispatch for analysis.

### Steps
1. Launch `forfun.exe` via Nsight Graphics (Frame Debugger activity)
2. Trigger bake in editor
3. Console shows:
   ```
   ========================================
   [DXRBaker] NSIGHT CAPTURE POINT
   [DXRBaker] Press F11 in Nsight Graphics NOW to capture
   [DXRBaker] Waiting 5 seconds before dispatch...
   ========================================
   ```
4. Press **F11** within 5 seconds

### Debug Flag
- `enableCaptureDelay = true` (default)
- Set to `false` for production/automated testing

---

## Phase 1: Verify Acceleration Structure Build

### Goal
Confirm BLAS (Bottom-Level) and TLAS (Top-Level) are built correctly.

### Nsight Graphics Steps

1. Open **Acceleration Structure Viewer**: View -> Acceleration Structures

2. **Check BLAS**:
   | Item | Expected | Problem if Wrong |
   |------|----------|------------------|
   | Geometry count | >= 1 per mesh | No geometry exported |
   | Triangle count | Matches mesh | Vertex/index buffer issue |
   | Bounds (AABB) | Reasonable size | Transform issue |

3. **Check TLAS**:
   | Item | Expected | Problem if Wrong |
   |------|----------|------------------|
   | Instance count | = Number of meshes | Instance buffer issue |
   | Instance transforms | Identity or world matrix | Transform not applied |
   | BLAS references | Valid pointers | BLAS not built |

### Expected Console Output
```
[DXRBaker] === Acceleration Structure Debug Info ===
[DXRBaker] BLAS count: N
[DXRBaker] BLAS[0]: triangles=XXX, bounds=(min)-(max)
[DXRBaker] TLAS instance count: N
[DXRBaker] TLAS GPU VA: 0x00000001XXXXXXXX
[DXRBaker] === End AS Debug Info ===
```

### Pass Criteria
- TLAS GPU VA is non-zero
- Geometry visible in AS viewer
- Bounds match scene objects

### Debug Flag
- `logAccelerationStructure = true`

---

## Phase 2: Verify Ray Tracing Pipeline Creation

### Goal
Confirm PSO and shader compilation succeeded.

### Nsight Graphics Steps

1. Find `CreateStateObject` in API Inspector
2. Verify State Object Type: `D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE`
3. Check Subobjects:
   - DXIL Library (shader bytecode)
   - Hit Groups (HitGroup, ShadowHitGroup)
   - Shader Config (payload size, attribute size)
   - Pipeline Config (max recursion depth)

4. Verify Shader Exports:
   | Export Name | Type |
   |-------------|------|
   | `RayGen` | Ray Generation |
   | `Miss` | Miss Shader |
   | `ShadowMiss` | Miss Shader |
   | `HitGroup` | Hit Group (ClosestHit) |
   | `ShadowHitGroup` | Hit Group (AnyHit) |

### Expected Console Output
```
[DXRBaker] === Pipeline Creation Debug Info ===
[DXRBaker] Compiling DXR shader library: .../Shader/DXR/LightmapBake.hlsl
[DXRBaker] Shader compiled successfully (XXXX bytes)
[DXRBaker] Pipeline exports: RayGen, Miss, ShadowMiss, HitGroup, ShadowHitGroup
[DXRBaker] Ray tracing pipeline created successfully
[DXRBaker] SBT created: rayGen=1, miss=2, hitGroup=2
[DXRBaker] === End Pipeline Debug Info ===
```

### Pass Criteria
- No shader compilation errors
- State object created successfully
- All shader exports present

### Debug Flag
- `logPipelineCreation = true`

---

## Phase 3: Verify Resource Binding

### Goal
Confirm all shader resources are bound correctly.

### Nsight Graphics Steps

1. Find `DispatchRays` call in API Inspector
2. Check Root Signature Bindings (click on DispatchRays, view bound resources):

   | Slot | Resource Type | Expected Content |
   |------|---------------|------------------|
   | b0 | CBV | `CB_BakeParams` (brick bounds, config) |
   | t0 | SRV | TLAS (acceleration structure) |
   | t1 | SRV | Skybox texture (optional) |
   | t2 | SRV | Material buffer |
   | t3 | SRV | Light buffer |
   | t4 | SRV | Instance buffer |
   | u0 | UAV | Output buffer (64 x SVoxelSHOutput) |
   | s0 | Sampler | Linear sampler |

3. Inspect Constant Buffer:
   - Right-click CBV -> View Memory
   - Verify struct values match expected

### Expected Console Output
```
[DXRBaker] === Resource Binding Debug Info ===
[DXRBaker] CB_BakeParams:
[DXRBaker]   brickWorldMin: (X.XX, Y.XX, Z.XX)
[DXRBaker]   brickWorldMax: (X.XX, Y.XX, Z.XX)
[DXRBaker]   samplesPerVoxel: 256
[DXRBaker]   maxBounces: 3
[DXRBaker]   numLights: N
[DXRBaker] TLAS bound at t0, GPU VA: 0xXXXXXXXX
[DXRBaker] Output UAV bound at u0, size: XXXX bytes
[DXRBaker] === End Resource Binding Debug Info ===
```

### Pass Criteria
- TLAS SRV bound at t0
- Output UAV bound at u0
- CB values correct

### Debug Flag
- `logResourceBinding = true`

---

## Phase 4: Verify DispatchRays Execution

### Goal
Confirm rays are dispatched and shaders execute.

### Nsight Graphics Steps

1. Find `DispatchRays` in API Inspector:
   ```
   DispatchRays(
       RayGenerationShaderRecord,
       MissShaderTable,
       HitGroupTable,
       Width=4, Height=4, Depth=4
   )
   ```

2. Check Shader Binding Table (SBT):
   | Table | Records |
   |-------|---------|
   | Ray Generation | 1 (RayGen) |
   | Miss | 2 (Miss, ShadowMiss) |
   | Hit Group | 2 (HitGroup, ShadowHitGroup) |

3. Debug Shader Execution (optional):
   - Right-click DispatchRays -> Debug Shader
   - Set breakpoint in RayGen
   - Verify `DispatchRaysIndex()` returns valid indices

4. Check GPU Timeline:
   - DispatchRays should show execution time > 0

### Expected Console Output
```
[DXRBaker] === Dispatch Debug Info ===
[DXRBaker] DispatchRays: 4 x 4 x 4 = 64 threads
[DXRBaker] SBT addresses:
[DXRBaker]   RayGen: 0xXXXXXXXX (size: XX)
[DXRBaker]   Miss: 0xXXXXXXXX (stride: XX, size: XX)
[DXRBaker]   HitGroup: 0xXXXXXXXX (stride: XX, size: XX)
[DXRBaker] === Dispatch Complete ===
```

### Pass Criteria
- DispatchRays call present
- SBT tables non-empty
- GPU execution time > 0

### Debug Flag
- `logDispatchInfo = true`

---

## Phase 5: Verify Output Buffer Readback

### Goal
Confirm shader writes correct SH data to output buffer.

### Nsight Graphics Steps

1. Find Output UAV in Resources panel:
   - Look for buffer named `DXRBaker_OutputBuffer`

2. View Buffer Contents AFTER DispatchRays:
   - Right-click -> View as Structured Buffer
   - Set structure stride = 124 bytes (sizeof SVoxelSHOutput)

3. Check Values:
   | Field | Expected (Sky-Only Test) |
   |-------|--------------------------|
   | sh[0] (L0) | ~(0.28 x skyColor) |
   | sh[1-8] | Small directional values |
   | validity | 0.0 or 1.0 |

### Common Problems
| Output Pattern | Likely Cause |
|----------------|--------------|
| All zeros | UAV not bound, shader not writing |
| All NaN | Division by zero, bad ray origin |
| All same value | DispatchRaysIndex not used |
| Garbage | Struct alignment mismatch |

### Expected Console Output
```
[DXRBaker] === Readback Debug Info ===
[DXRBaker] Voxel SH[0] stats: valid=64, zero=0, nan/inf=0
[DXRBaker] SH[0] luminance range: [0.001234, 0.567890]
[DXRBaker] Voxel[0] SH[0] (L0): (0.0845, 0.0923, 0.1012)
[DXRBaker] Voxel[0] SH[1] (L1y): (0.0012, 0.0015, 0.0018)
[DXRBaker] Voxel[0] validity: 1.00
[DXRBaker] === End Readback Debug Info ===
```

### Pass Criteria
- valid count > 0
- SH[0] has reasonable RGB values (not zero, not NaN)
- Luminance range is physically plausible

### Debug Flag
- `logReadbackResults = true`

---

## Phase 6: Verify Visual Results

### Goal
Confirm baked lightmap produces correct lighting in viewport.

### Steps

1. Complete Full Bake (let all bricks finish)

2. Enable Volumetric Lightmap:
   - Set `DiffuseGIMode = VolumetricLightmap`
   - Disable other GI sources

3. Visual Checks:
   | Check | Expected |
   |-------|----------|
   | Indoor areas | Bounced light from walls |
   | Shadows | Soft indirect shadows |
   | Color bleeding | Wall color on nearby surfaces |
   | Sky visibility | Brighter near openings |

4. Optional Debug Visualization:
   - Render voxel grid as debug overlay
   - Color-code by SH[0] luminance

### Debug Flag
- `pauseAfterFirstBrick = true` - Pause after first brick to inspect intermediate results

---

## Troubleshooting Decision Tree

```
Start
  |
  +-- TLAS GPU VA = 0?
  |     +-- Yes -> Fix: BuildAccelerationStructures() failed
  |
  +-- Pipeline creation failed?
  |     +-- Yes -> Fix: Check shader compilation errors
  |
  +-- DispatchRays not called?
  |     +-- Yes -> Fix: Check cmdList->DispatchRays() path
  |
  +-- Output buffer all zeros?
  |     +-- Yes -> Fix: UAV not bound, check root signature
  |
  +-- Output buffer all NaN?
  |     +-- Yes -> Fix: Bad ray origin/direction in shader
  |
  +-- Valid output but wrong lighting?
        +-- Fix: Check SH reconstruction or normal direction
```

---

## Quick Reference

### Nsight Workflow
```
1. Launch forfun.exe via Nsight Graphics
2. Trigger "Bake Lightmap (GPU/DXR)" in editor
3. Press F11 during 5-second pause
4. Wait for capture to complete
5. Analyze: AS -> Pipeline -> Bindings -> Dispatch -> Output
```

### Key Nsight Panels
| Panel | Use For |
|-------|---------|
| API Inspector | Find DXR calls, check parameters |
| Acceleration Structures | Visualize BLAS/TLAS |
| Resources | View buffer contents |
| Shader Debugger | Step through RT shaders |
| GPU Trace | Timing analysis |

---

## Related Files

- `Engine/Rendering/RayTracing/DXRLightmapBaker.h` - Debug flags struct
- `Engine/Rendering/RayTracing/DXRLightmapBaker.cpp` - Implementation with debug logging
- `Shader/DXR/LightmapBake.hlsl` - Ray tracing shaders
- `Shader/DXR/LightmapBakeCommon.hlsl` - Shared utilities

---

**Last Updated**: 2025-12-19
