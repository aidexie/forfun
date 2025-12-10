#pragma once

#include <DirectXMath.h>
#include "RHI/RHIResources.h"

class CCamera;
class CScene;
class CRenderPipeline;

// ============================================
// CubemapRenderer: 共享的 Cubemap 渲染工具类
// 被 LightProbeBaker 和 ReflectionProbeBaker 共同使用
// ============================================
class CCubemapRenderer
{
public:
    // 设置相机朝向指定的 cubemap face
    // DirectX 左手坐标系：+X=Right, +Y=Up, +Z=Forward
    // face: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
    static void SetupCameraForCubemapFace(
        CCamera& camera,
        int face,
        const DirectX::XMFLOAT3& position);

    // 渲染场景到 cubemap 的 6 个面 (RHI version)
    static void RenderToCubemap(
        const DirectX::XMFLOAT3& position,
        int resolution,
        CScene& scene,
        CRenderPipeline* pipeline,
        RHI::ITexture* outputCubemap);

private:
    // 渲染单个 cubemap face
    static void RenderCubemapFace(
        int face,
        const DirectX::XMFLOAT3& position,
        int resolution,
        CScene& scene,
        CRenderPipeline* pipeline,
        RHI::ITexture* outputCubemap);
};
