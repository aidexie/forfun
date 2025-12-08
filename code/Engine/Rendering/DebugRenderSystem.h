// Engine/Rendering/DebugRenderSystem.h
#pragma once
#include "DebugLinePass.h"

// Forward declarations
class CScene;

// CDebugRenderSystem: 负责收集并渲染场景中的调试几何体
// 当前支持：AABB 包围盒
// 未来扩展：射线、网格、轴、碰撞体等
class CDebugRenderSystem {
public:
    static CDebugRenderSystem& Instance();

    // 收集场景中的 debug geometry 并提交给 DebugLinePass 渲染
    void CollectAndRender(CScene& scene, CDebugLinePass& linePass);

private:
    CDebugRenderSystem() = default;
    ~CDebugRenderSystem() = default;
    CDebugRenderSystem(const CDebugRenderSystem&) = delete;
    CDebugRenderSystem& operator=(const CDebugRenderSystem&) = delete;

    // 收集 AABB 包围盒
    void CollectAABBs(CScene& scene, CDebugLinePass& linePass);

    // 收集 Volumetric Lightmap 八叉树调试可视化
    void CollectVolumetricLightmapOctree(CScene& scene, CDebugLinePass& linePass);
};
