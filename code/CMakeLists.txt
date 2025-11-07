
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

file(GLOB CORE_SRC ${CODE_PATH}/Core/*.cpp ${CODE_PATH}/Core/*.h)
file(GLOB ENGINE_SRC ${CODE_PATH}/Engine/*.cpp ${CODE_PATH}/Engine/*.h)
set(EDITOR_SRC
    ${CODE_PATH}/Editor/Panels.h
    ${CODE_PATH}/Editor/Panels_Dockspace.cpp
    ${CODE_PATH}/Editor/HierarchyPanel.cpp
    ${CODE_PATH}/Editor/InspectorPanel.cpp
    ${CODE_PATH}/Editor/ViewportPanel.cpp
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
)
target_compile_definitions(forfun PRIVATE UNICODE _UNICODE)
target_link_libraries(forfun
    PRIVATE
        imgui_docking
        d3d11 dxgi d3dcompiler
)


# Added World/GameObject/Component system
set(ENGINE_SOURCES
    ${CODE_PATH}/Engine/GameObject.h
    ${CODE_PATH}/Engine/Component.h
    ${CODE_PATH}/Engine/World.h
    ${CODE_PATH}/Engine/PropertyVisitor.h
    ${CODE_PATH}/Engine/SceneSerializer.h
    ${CODE_PATH}/Engine/SceneSerializer.cpp
    ${CODE_PATH}/Engine/Components/Transform.h
    ${CODE_PATH}/Engine/Components/MeshRenderer.h
    ${CODE_PATH}/Engine/Components/MeshRenderer.cpp
    ${CODE_PATH}/Engine/Rendering/MainPass.h
    ${CODE_PATH}/Engine/Rendering/MainPass.cpp
)
target_sources(forfun PRIVATE ${ENGINE_SOURCES})
