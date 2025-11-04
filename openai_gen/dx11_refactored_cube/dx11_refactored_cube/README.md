# DX11 Refactored Rotating Cube (Window module + Renderer module)

在上一版旋转立方体基础上，**将窗口/消息循环**与**渲染器**拆分为两个模块：
- `WinMain.cpp`：仅负责 Win32 窗口创建、消息循环、把 `WM_SIZE` 转发给渲染器。
- `Renderer.h/.cpp`：封装 DX11 初始化、RTV/DSV 管理、着色器/缓冲、MVP、绘制与 Present。

## 构建
```bat
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Debug
.\Debug\DX11RefactoredCube.exe
```

## 结构
- **Renderer** 提供接口：
  - `bool Initialize(HWND hwnd, UINT width, UINT height)`
  - `void Resize(UINT width, UINT height)`
  - `void Render()`
  - `void Shutdown()`

这样后续你可以继续把输入、资源加载、RHI 抽象等独立出去，而 `WinMain` 仍然保持极简。