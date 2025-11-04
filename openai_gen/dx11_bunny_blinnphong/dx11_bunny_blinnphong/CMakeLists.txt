cmake_minimum_required(VERSION 3.20)
project(DX11_Bunny_BlinnPhong LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-DUNICODE -D_UNICODE)

add_executable(DX11_Bunny_BlinnPhong WIN32
    src/WinMain.cpp
    src/Renderer.cpp
    src/Renderer.h
    src/Mesh.cpp
    src/Mesh.h
    src/ObjLoader.cpp
    src/ObjLoader.h
    src/TextureLoader.cpp
    src/TextureLoader.h
)

if (MSVC)
    target_compile_options(DX11_Bunny_BlinnPhong PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

target_link_libraries(DX11_Bunny_BlinnPhong
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    ole32
)

set_property(TARGET DX11_Bunny_BlinnPhong PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")