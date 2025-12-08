// Engine/Rendering/DebugRenderSystem.cpp
#include "DebugRenderSystem.h"
#include "Scene.h"
#include "GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "VolumetricLightmap.h"

using namespace DirectX;

CDebugRenderSystem& CDebugRenderSystem::Instance() {
    static CDebugRenderSystem instance;
    return instance;
}

void CDebugRenderSystem::CollectAndRender(CScene& scene, CDebugLinePass& linePass) {
    // 收集各类 debug geometry
    CollectAABBs(scene, linePass);

    // Volumetric Lightmap 八叉树可视化
    CollectVolumetricLightmapOctree(scene, linePass);

    // 未来扩展：
    // CollectRays(scene, linePass);
    // CollectGizmos(scene, linePass);
    // CollectColliders(scene, linePass);
}

void CDebugRenderSystem::CollectAABBs(CScene& scene, CDebugLinePass& linePass) {
    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* meshRenderer = objPtr->GetComponent<SMeshRenderer>();
        auto* transform = objPtr->GetComponent<STransform>();

        if (meshRenderer && transform && meshRenderer->showBounds) {
            XMFLOAT3 localMin, localMax;
            if (meshRenderer->GetLocalBounds(localMin, localMax)) {
                linePass.AddAABB(
                    localMin, localMax,
                    transform->WorldMatrix(),
                    XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f)  // Green
                );
            }
        }
    }
}

void CDebugRenderSystem::CollectVolumetricLightmapOctree(CScene& scene, CDebugLinePass& linePass) {
    // 委托给 VolumetricLightmap 绘制
    auto& vl = scene.GetVolumetricLightmap();
    vl.DrawOctreeDebug(linePass);
}
