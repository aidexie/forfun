cmake_minimum_required(VERSION 3.20)
project(DX11RefactoredCube LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-DUNICODE -D_UNICODE)

add_executable(DX11RefactoredCube WIN32
    src/WinMain.cpp
    src/Renderer.cpp
)

if (MSVC)
    target_compile_options(DX11RefactoredCube PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

target_link_libraries(DX11RefactoredCube
    d3d11
    dxgi
    d3dcompiler
)

set_property(TARGET DX11RefactoredCube PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")