# Coding Style Guide

命名约定和代码风格规范。格式化规则见 `.clang-format`。

## 命名约定

### 类型名

| 类型 | 前缀 | 风格 | 示例 |
|------|------|------|------|
| Class | `C` | PascalCase | `CDX11Context`, `CMeshResourceManager` |
| Struct | `S` | PascalCase | `STransform`, `SMaterial`, `SVertexPNT` |

### 变量名

| 作用域 | 前缀 | 风格 | 示例 |
|--------|------|------|------|
| 类成员 | `m_` | snake_case | `m_device`, `m_vertex_buffer` |
| 结构体成员 | 无 | snake_case | `position`, `rotation_euler` |
| 局部变量 | 无 | snake_case | `mesh_path`, `vertex_count` |
| 全局变量 | `g_` | snake_case | `g_main_pass`, `g_scene` |
| 常量 | `k_` | snake_case | `k_window_width`, `k_max_cascades` |

### 函数名

| 类型 | 风格 | 示例 |
|------|------|------|
| Public 方法 | PascalCase | `Initialize()`, `GetDevice()` |
| Private/Protected 方法 | camelCase | `createDeviceAndSwapchain()` |
| Public 自由函数 | PascalCase | `LoadMesh()` |
| Internal 辅助函数 | camelCase | `parseHeader()` |

### 枚举值

使用 UPPER_SNAKE_CASE：`RENDER_PASS_MAIN`, `SHADOW_MAP_SIZE_2048`

## 文件命名

| 类型 | 规则 | 示例 |
|------|------|------|
| 头文件 | 匹配主类/结构体名 | `CDX11Context.h`, `SMaterial.h` |
| 实现文件 | 匹配头文件名 | `CDX11Context.cpp` |
| Panel 实现 | `Panels_*.cpp` | `Panels_Hierarchy.cpp` |
| Shader | `PassName.stage.hlsl` | `MainPass.vs.hlsl`, `MainPass.ps.hlsl` |

## Include Guard

统一使用 `#pragma once`，不使用传统 `#ifndef`/`#define`/`#endif`。

## 日志输出

**必须**使用 `std::cout`/`std::cerr` 输出到控制台：

```cpp
std::cout << "IBL: Starting generation..." << std::endl;
std::cerr << "ERROR: Failed to load shader!" << std::endl;
```

**禁止**使用 `OutputDebugStringA`（仅VS调试器可见）。

例外：Shader编译错误可同时输出到两处。
