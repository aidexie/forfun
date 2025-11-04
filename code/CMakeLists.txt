
cmake_minimum_required(VERSION 3.20)
project(ForFun_Refactored LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(forfun_refactored
    WinMain.cpp
    # Engine layer
    Camera.h Camera.cpp
    Update.h Update.cpp
    Render.h Render.cpp
    # Core (legacy from your project)
    Renderer.h Renderer.cpp
    Mesh.h Mesh.cpp
    ObjLoader.h ObjLoader.cpp
    GltfLoader.h GltfLoader.cpp
    TextureLoader.h TextureLoader.cpp
)

if (MSVC)
  target_compile_definitions(forfun_refactored PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(forfun_refactored PRIVATE d3d11 dxgi d3dcompiler windowscodecs)
endif()
