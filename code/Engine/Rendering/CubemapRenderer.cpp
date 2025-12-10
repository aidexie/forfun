#include "CubemapRenderer.h"
#include "RenderPipeline.h"
#include "ShowFlags.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "Core/FFLog.h"
#include "Engine/Camera.h"
#include "Engine/Scene.h"

using namespace DirectX;

void CCubemapRenderer::SetupCameraForCubemapFace(
    CCamera& camera,
    int face,
    const XMFLOAT3& position)
{
    camera.position = position;
    camera.fovY = XM_PIDIV2;         // 90 度
    camera.aspectRatio = 1.0f;       // 1:1 方形
    camera.nearZ = 0.3f;
    camera.farZ = 1000.0f;

    XMFLOAT3 target, up;

    // DirectX Cubemap 标准约定（左手坐标系）
    switch (face) {
    case 0: // +X (Right)
        target = XMFLOAT3(position.x + 1.0f, position.y, position.z);
        up = XMFLOAT3(0, 1, 0);
        break;
    case 1: // -X (Left)
        target = XMFLOAT3(position.x - 1.0f, position.y, position.z);
        up = XMFLOAT3(0, 1, 0);
        break;
    case 2: // +Y (Up)
        target = XMFLOAT3(position.x, position.y + 1.0f, position.z);
        up = XMFLOAT3(0, 0, -1);  // 左手坐标系: up = -Z
        break;
    case 3: // -Y (Down)
        target = XMFLOAT3(position.x, position.y - 1.0f, position.z);
        up = XMFLOAT3(0, 0, 1);   // 左手坐标系: up = +Z
        break;
    case 4: // +Z (Forward)
        target = XMFLOAT3(position.x, position.y, position.z + 1.0f);
        up = XMFLOAT3(0, 1, 0);
        break;
    case 5: // -Z (Back)
        target = XMFLOAT3(position.x, position.y, position.z - 1.0f);
        up = XMFLOAT3(0, 1, 0);
        break;
    }

    camera.SetLookAt(position, target, up);
}

void CCubemapRenderer::RenderToCubemap(
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    CRenderPipeline* pipeline,
    RHI::ITexture* outputCubemap)
{
    // 渲染 6 个面
    for (int face = 0; face < 6; face++)
    {
        RenderCubemapFace(face, position, resolution, scene, pipeline, outputCubemap);
    }

    // 解绑所有 render targets（确保 GPU 写入完成）
    RHI::ICommandList* cmdList = RHI::CRHIManager::Instance().GetRenderContext()->GetCommandList();
    if (cmdList) {
        cmdList->UnbindRenderTargets();
    }
}

void CCubemapRenderer::RenderCubemapFace(
    int face,
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    CRenderPipeline* pipeline,
    RHI::ITexture* outputCubemap)
{
    // 设置相机
    CCamera camera;
    SetupCameraForCubemapFace(camera, face, position);

    // 渲染场景（ReflectionProbe 模式：不渲染 skybox，只渲染几何体）
    FShowFlags showFlags = FShowFlags::ReflectionProbe();

    CRenderPipeline::RenderContext ctx{
        camera,
        scene,
        static_cast<unsigned int>(resolution),
        static_cast<unsigned int>(resolution),
        0.0f,  // deltaTime
        showFlags
    };

    // Configure output: copy HDR result to this cubemap face
    ctx.finalOutputTexture = outputCubemap;
    ctx.finalOutputArraySlice = face;
    ctx.finalOutputMipLevel = 0;
    ctx.outputFormat = CRenderPipeline::RenderContext::EOutputFormat::HDR;

    pipeline->Render(ctx);
}
