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

# Descriptor set migration - disable legacy binding APIs
option(FF_LEGACY_BINDING_DISABLED "Disable legacy slot-based binding APIs (SetShaderResource, etc.)" ON)

if(FF_LEGACY_BINDING_DISABLED)
    add_compile_definitions(FF_LEGACY_BINDING_DISABLED=1)
endif()

set(CODE_PATH E:/forfun/source/code)
set(THIRD_PARTY_PATH E:/forfun/thirdparty)
set(IMGUI_DIR ${THIRD_PARTY_PATH}/imgui)
set(IMGUI_BACK ${IMGUI_DIR}/backends)
set(IMGUIZMO_DIR ${THIRD_PARTY_PATH}/ImGuizmo)
set(KTX_DIR ${THIRD_PARTY_PATH}/KTX-Software)
set(XATLAS_DIR ${THIRD_PARTY_PATH}/xatlas/source/xatlas)
set(OIDN_DIR ${THIRD_PARTY_PATH}/oidn)
set(FSR2_DIR ${THIRD_PARTY_PATH}/fsr2)

add_library(imgui_docking STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_BACK}/imgui_impl_win32.cpp
    ${IMGUI_BACK}/imgui_impl_dx11.cpp
    ${IMGUI_BACK}/imgui_impl_dx12.cpp
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

# xatlas library (UV2 generation for lightmaps)
add_library(xatlas STATIC
    ${XATLAS_DIR}/xatlas.cpp
)
target_include_directories(xatlas PUBLIC
    ${XATLAS_DIR}
)
if (MSVC)
    target_compile_options(xatlas PRIVATE /utf-8 /W0)  # Disable warnings for thirdparty
endif()

# FSR2 library (AMD FidelityFX Super Resolution 2)
# Note: FSR2 SDK requires building to generate shader permutation headers.
# If the shader headers don't exist, we skip FSR2 and use stub implementation.
set(FSR2_API_DIR ${FSR2_DIR}/src/ffx-fsr2-api)
set(FSR2_SHADER_HEADER ${FSR2_API_DIR}/dx12/shaders/ffx_fsr2_tcr_autogen_pass_permutations.h)

if(EXISTS "${FSR2_SHADER_HEADER}")
    message(STATUS "FSR2: Found shader permutation headers, building FSR2 library")
    add_library(fsr2 STATIC
        ${FSR2_API_DIR}/ffx_fsr2.cpp
        ${FSR2_API_DIR}/ffx_assert.cpp
        ${FSR2_API_DIR}/dx12/ffx_fsr2_dx12.cpp
        ${FSR2_API_DIR}/dx12/shaders/ffx_fsr2_shaders_dx12.cpp
    )
    target_include_directories(fsr2 PUBLIC
        ${FSR2_API_DIR}
        ${FSR2_API_DIR}/dx12
    )
    target_compile_definitions(fsr2 PRIVATE FFX_GCC)
    target_compile_definitions(fsr2 PUBLIC FSR2_AVAILABLE=1)
    if (MSVC)
        target_compile_options(fsr2 PRIVATE /utf-8 /W0 /wd4244 /wd4267)
    endif()
else()
    message(WARNING "FSR2: Shader permutation headers not found. FSR2 will be disabled.")
    message(WARNING "       To enable FSR2, build the FSR2 SDK first:")
    message(WARNING "       cd ${FSR2_DIR}/build && GenerateSolutions.bat && cmake --build .")
    # Create empty interface library so linking still works
    add_library(fsr2 INTERFACE)
    target_compile_definitions(fsr2 INTERFACE FSR2_AVAILABLE=0)
endif()

# Core sources - explicitly listed (no GLOB)
set(CORE_SRC
    # Root
    ${CODE_PATH}/Core/Console.cpp
    ${CODE_PATH}/Core/Console.h
    ${CODE_PATH}/Core/DebugPaths.h
    ${CODE_PATH}/Core/FFLog.cpp
    ${CODE_PATH}/Core/FFLog.h
    ${CODE_PATH}/Core/GpuMeshResource.h
    ${CODE_PATH}/Core/MaterialAsset.cpp
    ${CODE_PATH}/Core/MaterialAsset.h
    ${CODE_PATH}/Core/MaterialManager.cpp
    ${CODE_PATH}/Core/MaterialManager.h
    ${CODE_PATH}/Core/Mesh.cpp
    ${CODE_PATH}/Core/Mesh.h
    ${CODE_PATH}/Core/MeshResourceManager.cpp
    ${CODE_PATH}/Core/MeshResourceManager.h
    ${CODE_PATH}/Core/PathManager.cpp
    ${CODE_PATH}/Core/PathManager.h
    ${CODE_PATH}/Core/RenderConfig.cpp
    ${CODE_PATH}/Core/RenderConfig.h
    ${CODE_PATH}/Core/ReflectionProbeAsset.cpp
    ${CODE_PATH}/Core/ReflectionProbeAsset.h
    ${CODE_PATH}/Core/RenderData.h
    ${CODE_PATH}/Core/RenderDocCapture.cpp
    ${CODE_PATH}/Core/RenderDocCapture.h
    ${CODE_PATH}/Core/SphericalHarmonics.cpp
    ${CODE_PATH}/Core/SphericalHarmonics.h
    ${CODE_PATH}/Core/TextureManager.cpp
    ${CODE_PATH}/Core/TextureManager.h
    ${CODE_PATH}/Core/TextureHandle.h
    # Exporter
    ${CODE_PATH}/Core/Exporter/KTXExporter.cpp
    ${CODE_PATH}/Core/Exporter/KTXExporter.h
    # Loader
    ${CODE_PATH}/Core/Loader/FFAssetLoader.cpp
    ${CODE_PATH}/Core/Loader/FFAssetLoader.h
    ${CODE_PATH}/Core/Loader/GltfLoader.cpp
    ${CODE_PATH}/Core/Loader/GltfLoader.h
    ${CODE_PATH}/Core/Loader/HdrLoader.cpp
    ${CODE_PATH}/Core/Loader/HdrLoader.h
    ${CODE_PATH}/Core/Loader/LUTLoader.cpp
    ${CODE_PATH}/Core/Loader/LUTLoader.h
    ${CODE_PATH}/Core/Loader/KTXLoader.cpp
    ${CODE_PATH}/Core/Loader/KTXLoader.h
    ${CODE_PATH}/Core/Loader/ObjLoader.cpp
    ${CODE_PATH}/Core/Loader/ObjLoader.h
    ${CODE_PATH}/Core/Loader/TextureLoader.cpp
    ${CODE_PATH}/Core/Loader/TextureLoader.h
    # Testing
    ${CODE_PATH}/Core/Testing/RenderStats.h
    ${CODE_PATH}/Core/Testing/Screenshot.cpp
    ${CODE_PATH}/Core/Testing/Screenshot.h
    ${CODE_PATH}/Core/Testing/TestCase.cpp
    ${CODE_PATH}/Core/Testing/TestCase.h
    ${CODE_PATH}/Core/Testing/TestRegistry.h
    # RDG (Render Dependency Graph)
    ${CODE_PATH}/Core/RDG/RDGTypes.h
    ${CODE_PATH}/Core/RDG/RDGBuilder.cpp
    ${CODE_PATH}/Core/RDG/RDGBuilder.h
    ${CODE_PATH}/Core/RDG/RDGCompiler.h
    ${CODE_PATH}/Core/RDG/RDGHeapAllocator.h
    ${CODE_PATH}/Core/RDG/RDGBarrierBatcher.h
    ${CODE_PATH}/Core/RDG/RDGContext.h
)

# RHI sources
set(RHI_SRC
    # RHI Core
    ${CODE_PATH}/RHI/RHICommon.h
    ${CODE_PATH}/RHI/RHIDescriptors.h
    ${CODE_PATH}/RHI/RHIResources.h
    ${CODE_PATH}/RHI/RHIRayTracing.h
    ${CODE_PATH}/RHI/RHIHelpers.cpp
    ${CODE_PATH}/RHI/RHIHelpers.h
    ${CODE_PATH}/RHI/ICommandList.h
    ${CODE_PATH}/RHI/IDescriptorSet.h
    ${CODE_PATH}/RHI/IRenderContext.h
    ${CODE_PATH}/RHI/RHIFactory.cpp
    ${CODE_PATH}/RHI/RHIFactory.h
    ${CODE_PATH}/RHI/RHIManager.cpp
    ${CODE_PATH}/RHI/RHIManager.h
    ${CODE_PATH}/RHI/RHIPointers.h
    ${CODE_PATH}/RHI/PerFrameSlots.h
    ${CODE_PATH}/RHI/PerPassSlots.h
    ${CODE_PATH}/RHI/PerMaterialSlots.h
    ${CODE_PATH}/RHI/PerDrawSlots.h
    ${CODE_PATH}/RHI/CB_PerFrame.h
    # DX11 Backend (implemented in Phase 0.3)
    ${CODE_PATH}/RHI/DX11/DX11CommandList.cpp
    ${CODE_PATH}/RHI/DX11/DX11CommandList.h
    ${CODE_PATH}/RHI/DX11/DX11Context.cpp
    ${CODE_PATH}/RHI/DX11/DX11Context.h
    ${CODE_PATH}/RHI/DX11/DX11RenderContext.cpp
    ${CODE_PATH}/RHI/DX11/DX11RenderContext.h
    ${CODE_PATH}/RHI/DX11/DX11Resources.h
    ${CODE_PATH}/RHI/DX11/DX11Buffer.cpp
    ${CODE_PATH}/RHI/DX11/DX11Texture.cpp
    ${CODE_PATH}/RHI/DX11/DX11ShaderCompiler.cpp
    ${CODE_PATH}/RHI/DX11/DX11Utils.h
    ${CODE_PATH}/RHI/ShaderCompiler.h
    # DX12 Backend (Phase 6: + PSO and Pipeline State)
    ${CODE_PATH}/RHI/DX12/DX12Common.h
    ${CODE_PATH}/RHI/DX12/DX12Context.h
    ${CODE_PATH}/RHI/DX12/DX12Context.cpp
    ${CODE_PATH}/RHI/DX12/DX12DescriptorHeap.h
    ${CODE_PATH}/RHI/DX12/DX12DescriptorHeap.cpp
    ${CODE_PATH}/RHI/DX12/DX12Resources.h
    ${CODE_PATH}/RHI/DX12/DX12Buffer.cpp
    ${CODE_PATH}/RHI/DX12/DX12Texture.cpp
    ${CODE_PATH}/RHI/DX12/DX12UploadManager.h
    ${CODE_PATH}/RHI/DX12/DX12UploadManager.cpp
    ${CODE_PATH}/RHI/DX12/DX12DynamicBuffer.h
    ${CODE_PATH}/RHI/DX12/DX12DynamicBuffer.cpp
    ${CODE_PATH}/RHI/DX12/DX12ResourceStateTracker.h
    ${CODE_PATH}/RHI/DX12/DX12ResourceStateTracker.cpp
    ${CODE_PATH}/RHI/DX12/DX12CommandList.h
    ${CODE_PATH}/RHI/DX12/DX12CommandList.cpp
    ${CODE_PATH}/RHI/DX12/DX12GenerateMipsPass.h
    ${CODE_PATH}/RHI/DX12/DX12GenerateMipsPass.cpp
    ${CODE_PATH}/RHI/DX12/DX12RenderContext.h
    ${CODE_PATH}/RHI/DX12/DX12RenderContext.cpp
    ${CODE_PATH}/RHI/DX12/DX12PipelineState.h
    ${CODE_PATH}/RHI/DX12/DX12PipelineState.cpp
    ${CODE_PATH}/RHI/DX12/DX12AccelerationStructure.h
    ${CODE_PATH}/RHI/DX12/DX12AccelerationStructure.cpp
    ${CODE_PATH}/RHI/DX12/DX12RayTracingPipeline.h
    ${CODE_PATH}/RHI/DX12/DX12RayTracingPipeline.cpp
    ${CODE_PATH}/RHI/DX12/DX12ShaderBindingTable.h
    ${CODE_PATH}/RHI/DX12/DX12ShaderBindingTable.cpp
    ${CODE_PATH}/RHI/DX12/DX12ShaderCompiler.cpp
    ${CODE_PATH}/RHI/DX12/DX12Debug.cpp
    ${CODE_PATH}/RHI/DX12/DX12DescriptorSet.h
    ${CODE_PATH}/RHI/DX12/DX12DescriptorSet.cpp
    ${CODE_PATH}/RHI/DX12/DX12DescriptorSetAllocator.h
    ${CODE_PATH}/RHI/DX12/DX12DescriptorSetAllocator.cpp
    ${CODE_PATH}/RHI/DX12/DX12RootSignatureCache.h
    ${CODE_PATH}/RHI/DX12/DX12RootSignatureCache.cpp
)

# Engine root sources - explicitly listed (no GLOB)
set(ENGINE_SRC
    ${CODE_PATH}/Engine/Camera.cpp
    ${CODE_PATH}/Engine/Camera.h
    ${CODE_PATH}/Engine/Component.h
    ${CODE_PATH}/Engine/ComponentRegistry.h
    ${CODE_PATH}/Engine/GameObject.h
    ${CODE_PATH}/Engine/JsonPropertyVisitor.h
    ${CODE_PATH}/Engine/PropertyVisitor.h
    ${CODE_PATH}/Engine/Scene.cpp
    ${CODE_PATH}/Engine/Scene.h
    ${CODE_PATH}/Engine/SceneLightSettings.h
    ${CODE_PATH}/Engine/SceneSerializer.cpp
    ${CODE_PATH}/Engine/SceneSerializer.h
    ${CODE_PATH}/Engine/World.h
)
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
    ${CODE_PATH}/Tests/TestAsyncTextureLoad.cpp
    ${CODE_PATH}/Tests/TestAlphaTest.cpp
    ${CODE_PATH}/Tests/TestAlphaBlend.cpp
    ${CODE_PATH}/Tests/TestClusteredLighting.cpp
    ${CODE_PATH}/Tests/TestSimplePointLight.cpp
    ${CODE_PATH}/Tests/TestSpotLight.cpp
    ${CODE_PATH}/Tests/TestCopyPaste.cpp
    ${CODE_PATH}/Tests/TestReflectionProbe.cpp
    ${CODE_PATH}/Tests/TestLightProbeSH.cpp
    ${CODE_PATH}/Tests/TestLightProbeIntegration.cpp
    ${CODE_PATH}/Tests/TestGPUReadback.cpp
    ${CODE_PATH}/Tests/TestDXRReadback.cpp
    ${CODE_PATH}/Tests/TestDXRBakeVisualize.cpp
    ${CODE_PATH}/Tests/TestDXRCubemapBaker.cpp
    ${CODE_PATH}/Tests/TestLightmapUV2.cpp
    ${CODE_PATH}/Tests/TestLightmap2D.cpp
    ${CODE_PATH}/Tests/TestOIDNDenoiser.cpp
    ${CODE_PATH}/Tests/TestGBuffer.cpp
    ${CODE_PATH}/Tests/TestMaterialTypes.cpp
    ${CODE_PATH}/Tests/TestDeferredPerf.cpp
    ${CODE_PATH}/Tests/TestBloom.cpp
    ${CODE_PATH}/Tests/TestAutoExposure.cpp
    ${CODE_PATH}/Tests/TestColorGrading.cpp
    ${CODE_PATH}/Tests/TestSSAO.cpp
    ${CODE_PATH}/Tests/TestHiZ.cpp
    ${CODE_PATH}/Tests/TestSSR.cpp
    ${CODE_PATH}/Tests/TestMotionBlur.cpp
    ${CODE_PATH}/Tests/TestDoF.cpp
    ${CODE_PATH}/Tests/TestTAA.cpp
    ${CODE_PATH}/Tests/TestFSR2.cpp
    ${CODE_PATH}/Tests/TestAntiAliasing.cpp
    ${CODE_PATH}/Tests/TestRDGBasic.cpp
    ${CODE_PATH}/Tests/TestDeferredStress.cpp
    ${CODE_PATH}/Tests/TestDescriptorSet.cpp
)

add_executable(forfun WIN32
    ${CORE_SRC} ${RHI_SRC} ${ENGINE_SRC}
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
    ${OIDN_DIR}/include
)
target_compile_definitions(forfun PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(forfun
    PRIVATE
        imgui_docking
        imguizmo
        xatlas
        fsr2
        d3d11 d3d12 dxgi d3dcompiler dxguid
        ${KTX_DIR}/lib/ktx.lib
        ${OIDN_DIR}/lib/OpenImageDenoise.lib
)

# Copy KTX DLL to output directory
add_custom_command(TARGET forfun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KTX_DIR}/bin/ktx.dll"
        $<TARGET_FILE_DIR:forfun>
)

# Copy OIDN DLLs to output directory
add_custom_command(TARGET forfun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OIDN_DIR}/bin/OpenImageDenoise.dll"
        $<TARGET_FILE_DIR:forfun>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OIDN_DIR}/bin/OpenImageDenoise_core.dll"
        $<TARGET_FILE_DIR:forfun>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OIDN_DIR}/bin/OpenImageDenoise_device_cpu.dll"
        $<TARGET_FILE_DIR:forfun>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OIDN_DIR}/bin/tbb12.dll"
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
    ${CODE_PATH}/Engine/Rendering/BloomPass.h
    ${CODE_PATH}/Engine/Rendering/BloomPass.cpp
    ${CODE_PATH}/Engine/Rendering/MotionBlurPass.h
    ${CODE_PATH}/Engine/Rendering/MotionBlurPass.cpp
    ${CODE_PATH}/Engine/Rendering/DepthOfFieldPass.h
    ${CODE_PATH}/Engine/Rendering/DepthOfFieldPass.cpp
    ${CODE_PATH}/Engine/Rendering/IBLGenerator.h
    ${CODE_PATH}/Engine/Rendering/IBLGenerator.cpp
    ${CODE_PATH}/Engine/Rendering/CubemapRenderer.h
    ${CODE_PATH}/Engine/Rendering/CubemapRenderer.cpp
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeBaker.h
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeBaker.cpp
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeManager.h
    ${CODE_PATH}/Engine/Rendering/ReflectionProbeManager.cpp
    ${CODE_PATH}/Engine/Rendering/LightProbeManager.h
    ${CODE_PATH}/Engine/Rendering/LightProbeManager.cpp
    ${CODE_PATH}/Engine/Rendering/LightProbeBaker.h
    ${CODE_PATH}/Engine/Rendering/LightProbeBaker.cpp
    ${CODE_PATH}/Engine/Rendering/VolumetricLightmap.h
    ${CODE_PATH}/Engine/Rendering/VolumetricLightmap.cpp
    ${CODE_PATH}/Engine/Rendering/RayTracing/RayTracer.h
    ${CODE_PATH}/Engine/Rendering/RayTracing/RayTracer.cpp
    ${CODE_PATH}/Engine/Rendering/RayTracing/PathTraceBaker.h
    ${CODE_PATH}/Engine/Rendering/RayTracing/PathTraceBaker.cpp
    ${CODE_PATH}/Engine/Rendering/RayTracing/SceneGeometryExport.h
    ${CODE_PATH}/Engine/Rendering/RayTracing/SceneGeometryExport.cpp
    ${CODE_PATH}/Engine/Rendering/RayTracing/DXRAccelerationStructureManager.h
    ${CODE_PATH}/Engine/Rendering/RayTracing/DXRAccelerationStructureManager.cpp
    ${CODE_PATH}/Engine/Rendering/RayTracing/DXRCubemapBaker.h
    ${CODE_PATH}/Engine/Rendering/RayTracing/DXRCubemapBaker.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapTypes.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapUV2.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapUV2.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapAtlas.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapAtlas.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapRasterizer.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapRasterizer.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapBaker.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapBaker.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/Lightmap2DGPUBaker.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/Lightmap2DGPUBaker.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/Lightmap2DManager.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/Lightmap2DManager.cpp
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapDenoiser.h
    ${CODE_PATH}/Engine/Rendering/Lightmap/LightmapDenoiser.cpp
    ${CODE_PATH}/Engine/Rendering/DebugLinePass.h
    ${CODE_PATH}/Engine/Rendering/DebugLinePass.cpp
    ${CODE_PATH}/Engine/Rendering/DebugRenderSystem.h
    ${CODE_PATH}/Engine/Rendering/DebugRenderSystem.cpp
    ${CODE_PATH}/Engine/Rendering/GridPass.h
    ${CODE_PATH}/Engine/Rendering/ClusteredLightingPass.h
    ${CODE_PATH}/Engine/Rendering/ClusteredLightingPass.cpp
    ${CODE_PATH}/Engine/Rendering/IPerFrameContributor.h
    ${CODE_PATH}/Engine/Rendering/PassLayouts.h
    ${CODE_PATH}/Engine/Rendering/SSAOPass.h
    ${CODE_PATH}/Engine/Rendering/SSAOPass.cpp
    ${CODE_PATH}/Engine/Rendering/HiZPass.h
    ${CODE_PATH}/Engine/Rendering/HiZPass.cpp
    ${CODE_PATH}/Engine/Rendering/SSRPass.h
    ${CODE_PATH}/Engine/Rendering/SSRPass.cpp
    ${CODE_PATH}/Engine/Rendering/TAAPass.h
    ${CODE_PATH}/Engine/Rendering/TAAPass.cpp
    ${CODE_PATH}/Engine/Rendering/FSR2Context.h
    ${CODE_PATH}/Engine/Rendering/FSR2Context.cpp
    ${CODE_PATH}/Engine/Rendering/FSR2Pass.h
    ${CODE_PATH}/Engine/Rendering/FSR2Pass.cpp
    ${CODE_PATH}/Engine/Rendering/AntiAliasingPass.h
    ${CODE_PATH}/Engine/Rendering/AntiAliasingPass.cpp
    ${CODE_PATH}/Engine/Rendering/SMAALookupTextures.h
    ${CODE_PATH}/Engine/Rendering/SMAALookupTextures.cpp
    ${CODE_PATH}/Engine/Rendering/AutoExposurePass.h
    ${CODE_PATH}/Engine/Rendering/AutoExposurePass.cpp
    ${CODE_PATH}/Engine/Rendering/GridPass.cpp
    # Deferred Rendering Pipeline (Phase 3.2)
    ${CODE_PATH}/Engine/Rendering/Deferred/GBuffer.h
    ${CODE_PATH}/Engine/Rendering/Deferred/GBuffer.cpp
    ${CODE_PATH}/Engine/Rendering/Deferred/DepthPrePass.h
    ${CODE_PATH}/Engine/Rendering/Deferred/DepthPrePass.cpp
    ${CODE_PATH}/Engine/Rendering/Deferred/GBufferPass.h
    ${CODE_PATH}/Engine/Rendering/Deferred/GBufferPass.cpp
    ${CODE_PATH}/Engine/Rendering/Deferred/DeferredLightingPass.h
    ${CODE_PATH}/Engine/Rendering/Deferred/DeferredLightingPass.cpp
    ${CODE_PATH}/Engine/Rendering/Deferred/TransparentForwardPass.h
    ${CODE_PATH}/Engine/Rendering/Deferred/TransparentForwardPass.cpp
    ${CODE_PATH}/Engine/Rendering/Deferred/DeferredRenderPipeline.h
    ${CODE_PATH}/Engine/Rendering/Deferred/DeferredRenderPipeline.cpp
)

# Shader files (for IDE visibility)
set(SHADER_SOURCES
    ${CODE_PATH}/Shader/MainPass.vs.hlsl
    ${CODE_PATH}/Shader/MainPass.ps.hlsl
    ${CODE_PATH}/Shader/GBuffer.vs.hlsl
    ${CODE_PATH}/Shader/GBuffer.ps.hlsl
    ${CODE_PATH}/Shader/DeferredLighting.ps.hlsl
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
    ${CODE_PATH}/Shader/Fullscreen.vs.hlsl
    ${CODE_PATH}/Shader/FXAA.ps.hlsl
    ${CODE_PATH}/Shader/SMAAEdgeDetection.ps.hlsl
    ${CODE_PATH}/Shader/SMAABlendingWeight.ps.hlsl
    ${CODE_PATH}/Shader/SMAANeighborhoodBlend.ps.hlsl
    ${CODE_PATH}/Shader/DoFCoC.ps.hlsl
    ${CODE_PATH}/Shader/DoFDownsampleSplit.ps.hlsl
    ${CODE_PATH}/Shader/DoFBlurH.ps.hlsl
    ${CODE_PATH}/Shader/DoFBlurV.ps.hlsl
    ${CODE_PATH}/Shader/DoFComposite.ps.hlsl
    # SM 5.1 shaders (descriptor sets with register spaces)
    ${CODE_PATH}/Shader/Common.hlsli
    ${CODE_PATH}/Shader/TestDescriptorSet.vs.hlsl
    ${CODE_PATH}/Shader/TestDescriptorSet.ps.hlsl
)

target_sources(forfun PRIVATE ${ENGINE_SOURCES} ${SHADER_SOURCES})

# Configure shader compilation for Visual Studio
if (MSVC)
    # Vertex Shaders
    set_source_files_properties(
        ${CODE_PATH}/Shader/MainPass.vs.hlsl
        ${CODE_PATH}/Shader/GBuffer.vs.hlsl
        ${CODE_PATH}/Shader/IrradianceConvolution.vs.hlsl
        ${CODE_PATH}/Shader/PreFilterEnvironmentMap.vs.hlsl
        ${CODE_PATH}/Shader/BrdfLut.vs.hlsl
        ${CODE_PATH}/Shader/Skybox.vs.hlsl
        ${CODE_PATH}/Shader/EquirectToCubemap.vs.hlsl
        ${CODE_PATH}/Shader/DebugLine.vs.hlsl
        ${CODE_PATH}/Shader/Grid.vs.hlsl
        ${CODE_PATH}/Shader/Fullscreen.vs.hlsl
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
        ${CODE_PATH}/Shader/GBuffer.ps.hlsl
        ${CODE_PATH}/Shader/DeferredLighting.ps.hlsl
        ${CODE_PATH}/Shader/IrradianceConvolution.ps.hlsl
        ${CODE_PATH}/Shader/PreFilterEnvironmentMap.ps.hlsl
        ${CODE_PATH}/Shader/BrdfLut.ps.hlsl
        ${CODE_PATH}/Shader/Skybox.ps.hlsl
        ${CODE_PATH}/Shader/EquirectToCubemap.ps.hlsl
        ${CODE_PATH}/Shader/DebugLine.ps.hlsl
        ${CODE_PATH}/Shader/Grid.ps.hlsl
        ${CODE_PATH}/Shader/SMAAEdgeDetection.ps.hlsl
        ${CODE_PATH}/Shader/SMAABlendingWeight.ps.hlsl
        ${CODE_PATH}/Shader/SMAANeighborhoodBlend.ps.hlsl
        ${CODE_PATH}/Shader/DoFCoC.ps.hlsl
        ${CODE_PATH}/Shader/DoFDownsampleSplit.ps.hlsl
        ${CODE_PATH}/Shader/DoFBlurH.ps.hlsl
        ${CODE_PATH}/Shader/DoFBlurV.ps.hlsl
        ${CODE_PATH}/Shader/DoFComposite.ps.hlsl
        PROPERTIES
        VS_SHADER_TYPE Pixel
        VS_SHADER_MODEL 5.0
        VS_SHADER_ENTRYPOINT main
    )

    # SM 5.1 Vertex Shaders (descriptor sets with register spaces)
    set_source_files_properties(
        ${CODE_PATH}/Shader/TestDescriptorSet.vs.hlsl
        PROPERTIES
        VS_SHADER_TYPE Vertex
        VS_SHADER_MODEL 5.1
        VS_SHADER_ENTRYPOINT main
    )

    # SM 5.1 Pixel Shaders (descriptor sets with register spaces)
    set_source_files_properties(
        ${CODE_PATH}/Shader/TestDescriptorSet.ps.hlsl
        ${CODE_PATH}/Shader/FXAA.ps.hlsl
        PROPERTIES
        VS_SHADER_TYPE Pixel
        VS_SHADER_MODEL 5.1
        VS_SHADER_ENTRYPOINT main
    )
endif()
