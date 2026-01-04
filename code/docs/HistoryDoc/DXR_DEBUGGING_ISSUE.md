# DXR Shader Execution Issue - Investigation Record

**Date**: 2025-12-22
**Status**: Unresolved - Requires GPU Debugger Investigation

## Problem Description

Ray generation shader in DXR (DirectX Raytracing) does not execute despite all API calls appearing correct. The shader is supposed to write `0xDEADBEEF` to a UAV buffer, but the buffer remains all zeros after DispatchRays.

## Test Case

**File**: `Tests/TestDXRReadback.cpp`

**Minimal Shader**:
```hlsl
RWByteAddressBuffer g_Output : register(u0);

[shader("raygeneration")]
void MinimalRayGen() {
    g_Output.Store(0, 0xDEADBEEFu);

    uint3 threadId = DispatchRaysIndex();
    uint3 dims = DispatchRaysDimensions();
    uint linearIdx = threadId.x + threadId.y * dims.x;
    g_Output.Store(4 + linearIdx * 4, 42u + linearIdx);
}

[shader("miss")]
void MinimalMiss(inout SRayPayload payload : SV_RayPayload) {
    payload.color = float3(0, 0, 0);
}
```

**Dispatch**: 4x4x1 threads

## Environment

- **GPU**: NVIDIA GeForce RTX 2060
- **API**: DirectX 12 with DXR 1.0
- **Compiler**: DXC (lib_6_3 target)
- **Debug Layer**: Enabled with GPU-based validation
- **DRED**: Enabled

## What Was Verified Working

### 1. Device & Feature Support
- RTX 2060 reports raytracing support (Tier 1.0+)
- ID3D12Device5 successfully queried
- ID3D12GraphicsCommandList4 available

### 2. Shader Compilation
- DXC compiles shader library successfully (8460 bytes)
- Target: `lib_6_3`

### 3. State Object Creation
- `CreateStateObject()` succeeds
- `ID3D12StateObjectProperties` interface obtained
- Shader identifiers retrievable:
  - `MinimalRayGen`: `0A000000 00007C03...`
  - `MinimalMiss`: `0B000000 0000BC03...`

### 4. Shader Binding Table (SBT)
- Created on UPLOAD heap with GENERIC_READ state
- RayGen record at offset 0 (32 bytes)
- Miss record at offset 64 (32 bytes)
- Shader identifiers correctly written to SBT buffer
- SBT content dump verified in logs

### 5. Root Signature
- Ray tracing root signature created with:
  - Param 0: Root CBV (b0)
  - Param 1: SRV descriptor table (t0-t4)
  - Param 2: UAV descriptor table (u0)
  - Param 3: Sampler descriptor table (s0)
- Same root signature used for pipeline creation and command list binding
- Root signature re-set after `SetPipelineState1()` per DXR samples

### 6. Resource Bindings
- CBV bound to param 0
- NULL SRV descriptors created and bound to param 1
- UAV descriptor (RAW buffer format) bound to param 2
- Sampler descriptor bound to param 3
- All bindings use shader-visible descriptor heap

### 7. UAV Buffer
- Created with `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`
- UAV descriptor: `DXGI_FORMAT_R32_TYPELESS` with `D3D12_BUFFER_UAV_FLAG_RAW`
- Transitioned to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` before dispatch
- FlushBarriers() called before DispatchRays

### 8. DispatchRays Call
- D3D12_DISPATCH_RAYS_DESC correctly populated:
  ```
  RayGeneration: StartAddress=0x1BD82000, SizeInBytes=32
  MissShaderTable: StartAddress=0x1BD82040, SizeInBytes=32, StrideInBytes=32
  HitGroupTable: StartAddress=0x0, SizeInBytes=0, StrideInBytes=0
  Dimensions: 4 x 4 x 1
  ```
- `DispatchRays()` API call completes without error

### 9. Synchronization & Readback
- UAV barrier after dispatch
- Transition to COPY_SOURCE
- CopyBufferRegion to readback buffer
- ExecuteAndWait() with fence synchronization
- Pre-fill test confirmed copy works (0xCAFEBABE overwritten with zeros)

### 10. D3D12 Validation
- Debug layer enabled
- GPU-based validation enabled
- DRED enabled
- **No errors or warnings reported**

## Fixes Applied During Investigation

1. **FlushBarriers before DispatchRays** (`DX12CommandList.cpp`)
   - Added `FlushBarriers()` call to ensure resource barriers are submitted

2. **HitGroup table address when size=0** (`DX12CommandList.cpp`)
   - Set `HitGroupTable.StartAddress = 0` when `SizeInBytes = 0`

3. **Root signature re-set after SetPipelineState1** (`DX12CommandList.cpp`)
   - Added `SetComputeRootSignature()` call after `SetPipelineState1()` per DXR samples

4. **SBT content debugging** (`DX12ShaderBindingTable.cpp`)
   - Added logging for shader identifiers and SBT buffer contents

5. **D3D12 debug messages to CFFLog** (`DX12Context.cpp`)
   - Modified `FlushDebugMessages()` to output to log file

6. **NULL SRV descriptors** (`TestDXRReadback.cpp`)
   - Created proper NULL SRV descriptors for unused t0-t4 slots

## The Mystery

Despite all of the above being verified correct:
- Output buffer remains all zeros
- Shader never writes `0xDEADBEEF`
- No validation errors from D3D12 debug layer
- GPU validation doesn't catch any issues

## Hypotheses

1. **Driver Bug**: Possible NVIDIA driver issue with specific DXR configuration
2. **Subtle State Issue**: Some GPU state not being set correctly that validation doesn't catch
3. **Descriptor Heap Issue**: Possible issue with how descriptors are laid out or accessed
4. **Command List Recording**: Commands might not be recording to the list properly
5. **Fence/Sync Issue**: GPU might not be executing the workload despite fence wait

## Recommended Next Steps

1. **Use GPU Debugger**
   - Nsight Graphics or PIX to capture frame
   - Verify DispatchRays appears in GPU timeline
   - Inspect shader execution and resource bindings

2. **Test on Different Hardware**
   - Try AMD GPU or different NVIDIA card
   - Try different driver versions

3. **Simplify Further**
   - Create standalone minimal DXR project
   - Use simpler root signature (single UAV only)
   - Test with Microsoft's DXR samples as reference

4. **Check NVIDIA-Specific Requirements**
   - Verify no NVIDIA-specific extensions required
   - Check if specific driver settings affect DXR

## Related Files

- `Tests/TestDXRReadback.cpp` - Test case
- `RHI/DX12/DX12CommandList.cpp` - DispatchRays implementation
- `RHI/DX12/DX12ShaderBindingTable.cpp` - SBT builder
- `RHI/DX12/DX12RayTracingPipeline.cpp` - State object creation
- `RHI/DX12/DX12RenderContext.cpp` - Root signature creation (ray tracing)
- `RHI/DX12/DX12Context.cpp` - Device initialization

## Log Output Location

Test logs saved to: `E:/forfun/debug/TestDXRReadback/runtime.log`
