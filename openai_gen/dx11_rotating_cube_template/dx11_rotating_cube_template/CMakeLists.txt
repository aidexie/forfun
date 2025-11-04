cmake_minimum_required(VERSION 3.20)
project(DX11RotatingCube LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-DUNICODE -D_UNICODE)

add_executable(DX11RotatingCube WIN32
    src/main.cpp
)

if (MSVC)
    target_compile_options(DX11RotatingCube PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

target_link_libraries(DX11RotatingCube
    d3d11
    dxgi
    d3dcompiler
)

set_property(TARGET DX11RotatingCube PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")