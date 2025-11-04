cmake_minimum_required(VERSION 3.20)
project(DX11Triangle LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows and Unicode
add_definitions(-DUNICODE -D_UNICODE)

# Create executable (Win32 subsystem)
add_executable(DX11Triangle WIN32
    src/main.cpp
)

# Include Windows SDK headers automatically on MSVC
if (MSVC)
    target_compile_options(DX11Triangle PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

# Link required libraries
target_link_libraries(DX11Triangle
    d3d11
    dxgi
    d3dcompiler
)

# Set working directory to the binary dir when running from IDEs
set_property(TARGET DX11Triangle PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")