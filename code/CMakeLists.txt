cmake_minimum_required(VERSION 3.15)
project(ForFunEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT WIN32)
    message(FATAL_ERROR "This sample targets Windows (DX11).")
endif()
if (MSVC)
    add_compile_options(/utf-8)
endif()
set(CODE_PATH E:/forfun/source/code)
set(THIRD_PARTY_PATH E:/forfun/thirdparty)
set(IMGUI_DIR ${THIRD_PARTY_PATH}/imgui)
set(IMGUI_BACK ${IMGUI_DIR}/backends)
set(IMGUIZMO_DIR ${THIRD_PARTY_PATH}/ImGuizmo)
set(KTX_DIR ${THIRD_PARTY_PATH}/KTX-Software)

add_library(imgui_docking STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_BACK}/imgui_impl_win32.cpp
    ${IMGUI_BACK}/imgui_impl_dx11.cpp
)
target_include_directories(imgui_docking PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_BACK}
)
target_compile_definitions(imgui_docking PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
)
if (MSVC)
    target_compile_options(imgui_docking PRIVATE /utf-8)
endif()

# ImGuizmo library
add_library(imguizmo STATIC
    ${IMGUIZMO_DIR}/ImGuizmo.cpp
)
target_include_directories(imguizmo PUBLIC
    ${IMGUIZMO_DIR}
    ${IMGUI_DIR}
)
if (MSVC)
    target_compile_options(imguizmo PRIVATE /utf-8)
endif()

file(GLOB CORE_SRC ${CODE_PATH}/Core/*.cpp ${CODE_PATH}/Core/*.h)
file(GLOB ENGINE_SRC ${CODE_PATH}/Engine/*.cpp ${CODE_PATH}/Engine/*.h)
set(EDITOR_SRC
    ${CODE_PATH}/Editor/Panels.h
    ${CODE_PATH}/Editor/Panels_Dockspace.cpp
    ${CODE_PATH}/Editor/HierarchyPanel.cpp
    ${CODE_PATH}/Editor/InspectorPanel.cpp
    ${CODE_PATH}/Editor/ViewportPanel.cpp
    ${CODE_PATH}/Editor/Panels_IrradianceDebug.cpp
    ${CODE_PATH}/Editor/Panels_HDRExport.cpp
)

add_executable(forfun WIN32
    ${CORE_SRC} ${ENGINE_SRC}
    ${EDITOR_SRC}
    ${CODE_PATH}/main.cpp
)
target_include_directories(forfun PRIVATE
    ${CODE_PATH}
    ${CODE_PATH}/Core
    ${CODE_PATH}/Engine
    ${CODE_PATH}/Editor
    ${THIRD_PARTY_PATH}/cgltf-master
    ${THIRD_PARTY_PATH}/nlohmann
    ${KTX_DIR}/include
)
target_compile_definitions(forfun PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(forfun
    PRIVATE
        imgui_docking
        imguizmo
        d3d11 dxgi d3dcompiler
        ${KTX_DIR}/lib/ktx.lib
)

# Copy KTX DLL to output directory
add_custom_command(TARGET forfun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KTX_DIR}/bin/ktx.dll"
        $<TARGET_FILE_DIR:forfun>
)


# Added World/GameObject/Component system
set(ENGINE_SOURCES
    ${CODE_PATH}/Engine/GameObject.h
    ${CODE_PATH}/Engine/Component.h
    ${CODE_PATH}/Engine/ComponentRegistry.h
    ${CODE_PATH}/Engine/World.h
    ${CODE_PATH}/Engine/PropertyVisitor.h
    ${CODE_PATH}/Engine/Scene.h
    ${CODE_PATH}/Engine/Scene.cpp
    ${CODE_PATH}/Engine/SceneSerializer.h
    ${CODE_PATH}/Engine/SceneSerializer.cpp
    ${CODE_PATH}/Engine/Components/Transform.h
    ${CODE_PATH}/Engine/Components/MeshRenderer.h
    ${CODE_PATH}/Engine/Components/MeshRenderer.cpp
    ${CODE_PATH}/Engine/Components/DirectionalLight.h
    ${CODE_PATH}/Engine/Components/Material.h
    ${CODE_PATH}/Engine/Rendering/MainPass.h
    ${CODE_PATH}/Engine/Rendering/MainPass.cpp
    ${CODE_PATH}/Engine/Rendering/ShadowPass.h
    ${CODE_PATH}/Engine/Rendering/ShadowPass.cpp
    ${CODE_PATH}/Engine/Rendering/Skybox.h
    ${CODE_PATH}/Engine/Rendering/Skybox.cpp
    ${CODE_PATH}/Engine/Rendering/PostProcessPass.h
    ${CODE_PATH}/Engine/Rendering/PostProcessPass.cpp
    ${CODE_PATH}/Engine/Rendering/IBLGenerator.h
    ${CODE_PATH}/Engine/Rendering/IBLGenerator.cpp
)

# Shader files (for IDE visibility)
set(SHADER_SOURCES
    ${CODE_PATH}/Shader/MainPass.vs.hlsl
    ${CODE_PATH}/Shader/MainPass.ps.hlsl
    ${CODE_PATH}/Shader/IrradianceConvolution.vs.hlsl
    ${CODE_PATH}/Shader/IrradianceConvolution.ps.hlsl
    ${CODE_PATH}/Shader/PreFilterEnvironmentMap.vs.hlsl
    ${CODE_PATH}/Shader/PreFilterEnvironmentMap.ps.hlsl
    ${CODE_PATH}/Shader/BrdfLut.vs.hlsl
    ${CODE_PATH}/Shader/BrdfLut.ps.hlsl
    ${CODE_PATH}/Shader/Skybox.vs.hlsl
    ${CODE_PATH}/Shader/Skybox.ps.hlsl
    ${CODE_PATH}/Shader/EquirectToCubemap.vs.hlsl
    ${CODE_PATH}/Shader/EquirectToCubemap.ps.hlsl
)

target_sources(forfun PRIVATE ${ENGINE_SOURCES} ${SHADER_SOURCES})

# Configure shader compilation for Visual Studio
if (MSVC)
    # Vertex Shaders
    set_source_files_properties(
        ${CODE_PATH}/Shader/MainPass.vs.hlsl
        ${CODE_PATH}/Shader/IrradianceConvolution.vs.hlsl
        ${CODE_PATH}/Shader/PreFilterEnvironmentMap.vs.hlsl
        ${CODE_PATH}/Shader/BrdfLut.vs.hlsl
        ${CODE_PATH}/Shader/Skybox.vs.hlsl
        ${CODE_PATH}/Shader/EquirectToCubemap.vs.hlsl
        PROPERTIES
        VS_SHADER_TYPE Vertex
        VS_SHADER_MODEL 5.0
        VS_SHADER_ENTRYPOINT main
    )

    # Pixel Shaders
    set_source_files_properties(
        ${CODE_PATH}/Shader/MainPass.ps.hlsl
        ${CODE_PATH}/Shader/IrradianceConvolution.ps.hlsl
        ${CODE_PATH}/Shader/PreFilterEnvironmentMap.ps.hlsl
        ${CODE_PATH}/Shader/BrdfLut.ps.hlsl
        ${CODE_PATH}/Shader/Skybox.ps.hlsl
        ${CODE_PATH}/Shader/EquirectToCubemap.ps.hlsl
        PROPERTIES
        VS_SHADER_TYPE Pixel
        VS_SHADER_MODEL 5.0
        VS_SHADER_ENTRYPOINT main
    )
endif()
