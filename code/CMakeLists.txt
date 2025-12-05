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

file(GLOB_RECURSE CORE_SRC ${CODE_PATH}/Core/*.cpp ${CODE_PATH}/Core/*.h)
# Explicitly list SphericalHarmonics for visibility
set(CORE_SRC ${CORE_SRC}
    ${CODE_PATH}/Core/SphericalHarmonics.h
    ${CODE_PATH}/Core/SphericalHarmonics.cpp
)
file(GLOB ENGINE_SRC ${CODE_PATH}/Engine/*.cpp ${CODE_PATH}/Engine/*.h)
set(EDITOR_SRC
    ${CODE_PATH}/Editor/EditorContext.h
    ${CODE_PATH}/Editor/EditorContext.cpp
    ${CODE_PATH}/Editor/Panels.h
    ${CODE_PATH}/Editor/Panels_Dockspace.cpp
    ${CODE_PATH}/Editor/HierarchyPanel.cpp
    ${CODE_PATH}/Editor/InspectorPanel.cpp
    ${CODE_PATH}/Editor/ViewportPanel.cpp
    ${CODE_PATH}/Editor/Panels_IrradianceDebug.cpp
    ${CODE_PATH}/Editor/Panels_HDRExport.cpp
    ${CODE_PATH}/Editor/Panels_SceneLightSettings.cpp
    ${CODE_PATH}/Editor/Panels_MaterialEditor.cpp
    ${CODE_PATH}/Editor/PickingUtils.h
    ${CODE_PATH}/Editor/PickingUtils.cpp
)

# Test framework
# IMPORTANT: Explicitly list test files instead of using GLOB
# GLOB won't detect new files until CMake reconfigures
set(TEST_SRC
    ${CODE_PATH}/Tests/TestRayCast.cpp
    ${CODE_PATH}/Tests/TestMaterialAsset.cpp
    ${CODE_PATH}/Tests/TestAlphaTest.cpp
    ${CODE_PATH}/Tests/TestAlphaBlend.cpp
    ${CODE_PATH}/Tests/TestClusteredLighting.cpp
    ${CODE_PATH}/Tests/TestSimplePointLight.cpp
    ${CODE_PATH}/Tests/TestSpotLight.cpp
    ${CODE_PATH}/Tests/TestCopyPaste.cpp
    ${CODE_PATH}/Tests/TestReflectionProbe.cpp
    ${CODE_PATH}/Tests/TestLightProbeSH.cpp
    ${CODE_PATH}/Tests/TestLightProbeIntegration.cpp
)

add_executable(forfun WIN32
    ${CORE_SRC} ${ENGINE_SRC}
    ${EDITOR_SRC}
    ${TEST_SRC}
    ${CODE_PATH}/main.cpp
)
target_include_directories(forfun PRIVATE
    ${CODE_PATH}
    ${CODE_PATH}/Core
    ${CODE_PATH}/Engine
    ${CODE_PATH}/Editor
    ${THIRD_PARTY_PATH}/cgltf-master
    ${THIRD_PARTY_PATH}/nlohmann
    ${THIRD_PARTY_PATH}/stb
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
    ${CODE_PATH}/Engine/Components/PointLight.h
    ${CODE_PATH}/Engine/Components/SpotLight.h
    ${CODE_PATH}/Engine/Components/LightProbe.h
    ${CODE_PATH}/Engine/Components/ReflectionProbe.h
    # ✅ New Rendering Architecture
    ${CODE_PATH}/Engine/Rendering/ShowFlags.h
    ${CODE_PATH}/Engine/Rendering/RenderPipeline.h
    ${CODE_PATH}/Engine/Rendering/SceneRenderer.h
    ${CODE_PATH}/Engine/Rendering/SceneRenderer.cpp
    ${CODE_PATH}/Engine/Rendering/ForwardRenderPipeline.h
    ${CODE_PATH}/Engine/Rendering/ForwardRenderPipeline.cpp
    ${CODE_PATH}/Engine/Rendering/ShadowPass.h
    ${CODE_PATH}/Engine/Rendering/ShadowPass.cpp
    ${CODE_PATH}/Engine/Rendering/Skybox.h
    ${CODE_PATH}/Engine/Rendering/Skybox.cpp
    ${CODE_PATH}/Engine/Rendering/PostProcessPass.h
    ${CODE_PATH}/Engine/Rendering/PostProcessPass.cpp
    ${CODE_PATH}/Engine/Rendering/IBLGenerator.h
    ${CODE_PATH}/Engine/Rendering/IBLGenerator.cpp
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeBaker.h
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeBaker.cpp
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeManager.h
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeManager.cpp
    ${CODE_PATH}/Engine/Rendering/LightProbeManager.h
    ${CODE_PATH}/Engine/Rendering/LightProbeManager.cpp
    ${CODE_PATH}/Engine/Rendering/LightProbeBaker.h
    ${CODE_PATH}/Engine/Rendering/LightProbeBaker.cpp
    ${CODE_PATH}/Engine/Rendering/DebugLinePass.h
    ${CODE_PATH}/Engine/Rendering/DebugLinePass.cpp
    ${CODE_PATH}/Engine/Rendering/DebugRenderSystem.h
    ${CODE_PATH}/Engine/Rendering/DebugRenderSystem.cpp
    ${CODE_PATH}/Engine/Rendering/GridPass.h
    ${CODE_PATH}/Engine/Rendering/ClusteredLightingPass.h
    ${CODE_PATH}/Engine/Rendering/ClusteredLightingPass.cpp
    ${CODE_PATH}/Engine/Rendering/GridPass.cpp
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
    ${CODE_PATH}/Shader/DebugLine.vs.hlsl
    ${CODE_PATH}/Shader/DebugLine.gs.hlsl
    ${CODE_PATH}/Shader/DebugLine.ps.hlsl
    ${CODE_PATH}/Shader/Grid.vs.hlsl
    ${CODE_PATH}/Shader/Grid.ps.hlsl
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
        ${CODE_PATH}/Shader/DebugLine.vs.hlsl
        ${CODE_PATH}/Shader/Grid.vs.hlsl
        PROPERTIES
        VS_SHADER_TYPE Vertex
        VS_SHADER_MODEL 5.0
        VS_SHADER_ENTRYPOINT main
    )

    # Geometry Shaders
    set_source_files_properties(
        ${CODE_PATH}/Shader/DebugLine.gs.hlsl
        PROPERTIES
        VS_SHADER_TYPE Geometry
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
        ${CODE_PATH}/Shader/DebugLine.ps.hlsl
        ${CODE_PATH}/Shader/Grid.ps.hlsl
        PROPERTIES
        VS_SHADER_TYPE Pixel
        VS_SHADER_MODEL 5.0
        VS_SHADER_ENTRYPOINT main
    )
endif()
