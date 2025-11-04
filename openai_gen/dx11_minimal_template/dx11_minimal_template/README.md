# DX11 Minimal Triangle (CMake + WinMain)

A tiny Direct3D 11 project for Windows that opens a Win32 window and renders a colored triangle. No external assets.

## Prerequisites
- **Windows 10/11 (x64)**
- **Visual Studio 2022** (Desktop development with C++)
- **CMake 3.20+**
- **Windows SDK** (installs with VS)
- GPU/driver supporting Direct3D 11

## Build & Run (VS + CMake GUI)
1. Open **CMake GUI** (or use VS "Open Folder").
2. Set *source* to this folder and *build* to a separate `build` folder.
3. Click **Configure** (choose Visual Studio 2022, x64), then **Generate**.
4. Open the generated solution and build the `DX11Triangle` target.
5. Run. You should see a window with a colored triangle. Resize works.

## Build (CLI)
```bat
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Debug
.\Debug\DX11Triangle.exe
```

## Notes
- Shaders are inlined and compiled at runtime with `D3DCompile` (from `d3dcompiler_47.dll`).
- We only create an RTV (no depth buffer). Perfect for the first step.
- For PIX/RenderDoc debugging, make sure to disable fullscreen optimizations if needed.
- Next steps: add a depth buffer, constant buffers, camera matrices, textures.