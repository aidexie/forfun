#include "ForwardRenderPipeline.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/DebugEvent.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Camera.h"
#include <d3d11.h>

using Microsoft::WRL::ComPtr;

bool CForwardRenderPipeline::Initialize()
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return false;

    // 初始化各个 Pass
    if (!m_shadowPass.Initialize()) {
        CFFLog::Error("Failed to initialize ShadowPass");
        return false;
    }

    if (!m_sceneRenderer.Initialize()) {
        CFFLog::Error("Failed to initialize SceneRenderer");
        return false;
    }

    m_postProcess.Initialize();
    m_debugLinePass.Initialize();
    CGridPass::Instance().Initialize();

    CFFLog::Info("ForwardRenderPipeline initialized");
    return true;
}

void CForwardRenderPipeline::Shutdown()
{
    m_shadowPass.Shutdown();
    m_sceneRenderer.Shutdown();
    m_postProcess.Shutdown();
    m_debugLinePass.Shutdown();
    CGridPass::Instance().Shutdown();

    m_off.Reset();
    m_offLDR.Reset();
}

void CForwardRenderPipeline::Render(const RenderContext& ctx)
{
    ID3D11DeviceContext* d3dCtx = CDX11Context::Instance().GetContext();
    if (!d3dCtx) return;

    // ============================================
    // 0. Unbind resources to avoid hazards
    // ============================================
    ID3D11ShaderResourceView* nullSRV[8] = { nullptr };
    d3dCtx->VSSetShaderResources(0, 8, nullSRV);
    d3dCtx->PSSetShaderResources(0, 8, nullSRV);
    d3dCtx->OMSetRenderTargets(0, nullptr, nullptr);

    // ============================================
    // 1. Ensure offscreen targets are ready
    // ============================================
    ensureOffscreen(ctx.width, ctx.height);

    // ============================================
    // 2. Shadow Pass (if enabled)
    // ============================================
    const CShadowPass::Output* shadowData = nullptr;
    if (ctx.showFlags.Shadows) {
        // Find DirectionalLight in scene
        SDirectionalLight* dirLight = nullptr;
        for (auto& objPtr : ctx.scene.GetWorld().Objects()) {
            dirLight = objPtr->GetComponent<SDirectionalLight>();
            if (dirLight) break;
        }

        if (dirLight) {
            CScopedDebugEvent evt(d3dCtx, L"Shadow Pass");
            m_shadowPass.Render(ctx.scene, dirLight,
                              ctx.camera.GetViewMatrix(),
                              ctx.camera.GetProjectionMatrix());
            shadowData = &m_shadowPass.GetOutput();
        }
    }

    // ============================================
    // 3. Clear HDR render target
    // ============================================
    d3dCtx->OMSetRenderTargets(1, m_off.rtv.GetAddressOf(), m_off.dsv.Get());

    const float clearColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    d3dCtx->ClearRenderTargetView(m_off.rtv.Get(), clearColor);
    if (m_off.dsv) {
        d3dCtx->ClearDepthStencilView(m_off.dsv.Get(),
                                     D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                     1.0f, 0);
    }

    // ============================================
    // 4. Scene Rendering (Opaque + Transparent + Skybox)
    // ============================================
    {
        CScopedDebugEvent evt(d3dCtx, L"Scene Rendering");
        m_sceneRenderer.Render(ctx.camera, ctx.scene,
                              m_off.rtv.Get(), m_off.dsv.Get(),
                              ctx.width, ctx.height, ctx.deltaTime,
                              shadowData);
    }

    // ============================================
    // 5. Debug Lines (if enabled)
    // ============================================
    if (ctx.showFlags.DebugLines) {
        CScopedDebugEvent evt(d3dCtx, L"Debug Lines");
        m_debugLinePass.Render(ctx.camera.GetViewMatrix(),
                              ctx.camera.GetProjectionMatrix(),
                              ctx.width, ctx.height);
    }

    // ============================================
    // 6. Post-Processing (HDR → LDR)
    // ============================================
    if (ctx.showFlags.PostProcessing) {
        CScopedDebugEvent evt(d3dCtx, L"Post-Processing");
        m_postProcess.Render(m_off.srv.Get(), m_offLDR.rtv.Get(),
                           ctx.width, ctx.height, 1.0f);
    } else {
        // No post-processing: copy HDR to LDR directly
        // TODO: Implement simple copy if needed
        // For now, still apply tone mapping (otherwise output is HDR)
        m_postProcess.Render(m_off.srv.Get(), m_offLDR.rtv.Get(),
                           ctx.width, ctx.height, 1.0f);
    }

    // ============================================
    // 7. Grid (if enabled)
    // ============================================
    if (ctx.showFlags.Grid) {
        CScopedDebugEvent evt(d3dCtx, L"Grid");
        // Rebind LDR RTV + HDR depth (Grid needs depth test)
        d3dCtx->OMSetRenderTargets(1, m_offLDR.rtv.GetAddressOf(), m_off.dsv.Get());

        CGridPass::Instance().Render(ctx.camera.GetViewMatrix(),
                                     ctx.camera.GetProjectionMatrix(),
                                     ctx.camera.position);
    }

    // ============================================
    // 8. Copy final result to output RTV (if different)
    // ============================================
    if (ctx.outputRTV != m_offLDR.rtv.Get()) {
        // Copy m_offLDR to ctx.outputRTV
        // TODO: Implement copy if needed
        // For now, assume caller uses GetOffscreenSRV() for display
    }
}

void CForwardRenderPipeline::ensureOffscreen(unsigned int w, unsigned int h)
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    if (w == 0 || h == 0) return;
    if (m_off.color && w == m_off.w && h == m_off.h) return;

    // ============================================
    // HDR Render Target (R16G16B16A16_FLOAT)
    // ============================================
    m_off.Reset();
    m_off.w = w; m_off.h = h;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR linear space
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&td, nullptr, m_off.color.GetAddressOf());

    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format = td.Format;
    rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_off.color.Get(), &rvd, m_off.rtv.GetAddressOf());

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_off.color.Get(), &sv, m_off.srv.GetAddressOf());

    // ============================================
    // Depth Buffer (R24G8_TYPELESS)
    // ============================================
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&dd, nullptr, m_off.depth.GetAddressOf());

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    device->CreateDepthStencilView(m_off.depth.Get(), &dsvDesc, m_off.dsv.GetAddressOf());

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
    depthSRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSRVDesc.Texture2D.MostDetailedMip = 0;
    depthSRVDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_off.depth.Get(), &depthSRVDesc, m_off.depthSRV.GetAddressOf());

    // ============================================
    // LDR sRGB Render Target (R8G8B8A8_TYPELESS)
    // ============================================
    m_offLDR.Reset();
    m_offLDR.w = w; m_offLDR.h = h;

    D3D11_TEXTURE2D_DESC ldrDesc{};
    ldrDesc.Width = w;
    ldrDesc.Height = h;
    ldrDesc.MipLevels = 1;
    ldrDesc.ArraySize = 1;
    ldrDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;  // TYPELESS for different views
    ldrDesc.SampleDesc.Count = 1;
    ldrDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&ldrDesc, nullptr, m_offLDR.color.GetAddressOf());

    D3D11_RENDER_TARGET_VIEW_DESC ldrRTVDesc{};
    ldrRTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // RTV: sRGB (gamma correction on write)
    ldrRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_offLDR.color.Get(), &ldrRTVDesc, m_offLDR.rtv.GetAddressOf());

    D3D11_SHADER_RESOURCE_VIEW_DESC ldrSRVDesc{};
    ldrSRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // SRV: UNORM (no sRGB decode, already gamma-corrected)
    ldrSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ldrSRVDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_offLDR.color.Get(), &ldrSRVDesc, m_offLDR.srv.GetAddressOf());
}
