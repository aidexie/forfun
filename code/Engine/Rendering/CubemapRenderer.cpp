#include "CubemapRenderer.h"
#include "RenderPipeline.h"
#include "ShowFlags.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "Core/FFLog.h"
#include "Engine/Camera.h"
#include "Engine/Scene.h"

using Microsoft::WRL::ComPtr;
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
    ID3D11Texture2D* outputCubemap,
    ID3D11Texture2D* depthBuffer)
{
    auto* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    auto* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

    // 为每个 face 创建 RTV
    ComPtr<ID3D11RenderTargetView> faceRTVs[6];
    for (int face = 0; face < 6; face++)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.ArraySize = 1;

        HRESULT hr = device->CreateRenderTargetView(outputCubemap, &rtvDesc, faceRTVs[face].GetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[CubemapRenderer] Failed to create RTV for face %d", face);
            return;
        }
    }

    // 创建 DSV
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;

    HRESULT hr = device->CreateDepthStencilView(depthBuffer, &dsvDesc, dsv.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[CubemapRenderer] Failed to create DSV");
        return;
    }

    // 渲染 6 个面
    for (int face = 0; face < 6; face++)
    {
        RenderCubemapFace(face, position, resolution, scene, pipeline, faceRTVs[face].Get(), dsv.Get());
    }

    // 解绑所有 render targets 和 depth stencil（确保 GPU 写入完成）
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullptr);
}

void CCubemapRenderer::RenderCubemapFace(
    int face,
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    CRenderPipeline* pipeline,
    ID3D11RenderTargetView* faceRTV,
    ID3D11DepthStencilView* dsv)
{
    auto* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

    // 设置 viewport
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(resolution);
    viewport.Height = static_cast<float>(resolution);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // 清空 RT 和 depth buffer
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(faceRTV, clearColor);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // 设置 RT
    context->OMSetRenderTargets(1, &faceRTV, dsv);

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
    ctx.finalOutputRTV = faceRTV;
    ctx.outputFormat = CRenderPipeline::RenderContext::EOutputFormat::HDR;

    pipeline->Render(ctx);
}
