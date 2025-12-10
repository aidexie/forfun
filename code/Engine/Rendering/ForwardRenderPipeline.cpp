#include "ForwardRenderPipeline.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "Core/FFLog.h"
#include "Core/DebugEvent.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Camera.h"
#include <d3d11.h>

bool CForwardRenderPipeline::Initialize()
{
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) return false;

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

    m_offHDR.reset();
    m_offDepth.reset();
    m_offLDR.reset();
    m_offscreenWidth = 0;
    m_offscreenHeight = 0;
}

void CForwardRenderPipeline::Render(const RenderContext& ctx)
{
    ID3D11DeviceContext* d3dCtx = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());
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

    // Get RTV/DSV from RHI textures
    ID3D11RenderTargetView* hdrRTV = static_cast<ID3D11RenderTargetView*>(m_offHDR->GetRTV());
    ID3D11DepthStencilView* depthDSV = static_cast<ID3D11DepthStencilView*>(m_offDepth->GetDSV());
    ID3D11ShaderResourceView* hdrSRV = static_cast<ID3D11ShaderResourceView*>(m_offHDR->GetSRV());
    ID3D11RenderTargetView* ldrRTV = static_cast<ID3D11RenderTargetView*>(m_offLDR->GetRTV());

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
    d3dCtx->OMSetRenderTargets(1, &hdrRTV, depthDSV);

    const float clearColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    d3dCtx->ClearRenderTargetView(hdrRTV, clearColor);
    if (depthDSV) {
        d3dCtx->ClearDepthStencilView(depthDSV,
                                     D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                     1.0f, 0);
    }

    // ============================================
    // 4. Scene Rendering (Opaque + Transparent + Skybox)
    // ============================================
    {
        CScopedDebugEvent evt(d3dCtx, L"Scene Rendering");
        m_sceneRenderer.Render(ctx.camera, ctx.scene,
                              m_offHDR.get(), m_offDepth.get(),
                              ctx.width, ctx.height, ctx.deltaTime,
                              shadowData);
    }

    // ============================================
    // 5. Post-Processing (HDR -> LDR)
    // ============================================
    if (ctx.showFlags.PostProcessing) {
        CScopedDebugEvent evt(d3dCtx, L"Post-Processing");
        m_postProcess.Render(m_offHDR.get(), m_offLDR.get(),
                           ctx.width, ctx.height, 1.0f);
    }
    // Note: If PostProcessing is disabled, we skip this step and use HDR directly

    // ============================================
    // 6. Debug Lines (if enabled)
    // ============================================
    if (ctx.showFlags.DebugLines) {
        CScopedDebugEvent evt(d3dCtx, L"Debug Lines");
        // Rebind LDR RTV + HDR depth (Debug lines need depth test)
        d3dCtx->OMSetRenderTargets(1, &ldrRTV, depthDSV);
        m_debugLinePass.Render(ctx.camera.GetViewMatrix(),
                              ctx.camera.GetProjectionMatrix(),
                              ctx.width, ctx.height);
    }

    // ============================================
    // 7. Grid (if enabled)
    // ============================================
    if (ctx.showFlags.Grid) {
        CScopedDebugEvent evt(d3dCtx, L"Grid");
        // Rebind LDR RTV + HDR depth (Grid needs depth test)
        d3dCtx->OMSetRenderTargets(1, &ldrRTV, depthDSV);

        CGridPass::Instance().Render(ctx.camera.GetViewMatrix(),
                                     ctx.camera.GetProjectionMatrix(),
                                     ctx.camera.position);
    }

    // ============================================
    // 8. Copy final result to finalOutputRTV (if provided)
    // ============================================
    if (ctx.finalOutputRTV) {
        // Unbind all render targets before copy operations
        // D3D11 does not allow a texture to be bound as RTV and used as copy source
        ID3D11RenderTargetView* nullRTV = nullptr;
        ID3D11DepthStencilView* nullDSV = nullptr;
        d3dCtx->OMSetRenderTargets(1, &nullRTV, nullDSV);

        // Choose source based on outputFormat
        ID3D11Texture2D* sourceTexture;
        if (ctx.outputFormat == RenderContext::EOutputFormat::HDR) {
            sourceTexture = static_cast<ID3D11Texture2D*>(m_offHDR->GetNativeHandle());  // HDR linear
        } else {
            sourceTexture = static_cast<ID3D11Texture2D*>(m_offLDR->GetNativeHandle());  // LDR sRGB
        }

        // Get destination resource from RTV
        ID3D11Resource* dstResource = nullptr;
        ctx.finalOutputRTV->GetResource(&dstResource);

        if (dstResource) {
            // Get descriptors to check compatibility
            D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
            sourceTexture->GetDesc(&srcDesc);
            ((ID3D11Texture2D*)dstResource)->GetDesc(&dstDesc);

            // Check if we can use CopyResource (requires identical dimensions)
            bool canUseCopyResource =
                (srcDesc.Width == dstDesc.Width) &&
                (srcDesc.Height == dstDesc.Height) &&
                (srcDesc.ArraySize == dstDesc.ArraySize) &&
                (srcDesc.MipLevels == dstDesc.MipLevels) &&
                (srcDesc.Format == dstDesc.Format);

            if (canUseCopyResource) {
                // Full resource copy (fastest)
                d3dCtx->CopyResource(dstResource, sourceTexture);
            } else {
                // Copy single subresource (e.g., 2D -> Cubemap face)
                UINT dstSubresource = 0;

                // If destination is array texture (like cubemap), we need to get the correct subresource
                D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
                ctx.finalOutputRTV->GetDesc(&rtvDesc);

                if (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
                    UINT arraySlice = rtvDesc.Texture2DArray.FirstArraySlice;
                    UINT mipSlice = rtvDesc.Texture2DArray.MipSlice;
                    dstSubresource = D3D11CalcSubresource(mipSlice, arraySlice, dstDesc.MipLevels);
                }

                d3dCtx->CopySubresourceRegion(
                    dstResource,
                    dstSubresource,
                    0, 0, 0,
                    sourceTexture,
                    0,
                    nullptr
                );
            }

            dstResource->Release();
        }
    }
}

void CForwardRenderPipeline::ensureOffscreen(unsigned int w, unsigned int h)
{
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) return;

    if (w == 0 || h == 0) return;
    if (m_offHDR && w == m_offscreenWidth && h == m_offscreenHeight) return;

    m_offscreenWidth = w;
    m_offscreenHeight = h;

    // ============================================
    // HDR Render Target (R16G16B16A16_FLOAT)
    // ============================================
    {
        RHI::TextureDesc desc = RHI::TextureDesc::RenderTarget(w, h, RHI::ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "HDR_RenderTarget";
        m_offHDR.reset(rhiCtx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // Depth Buffer (R24G8_TYPELESS with DSV + SRV)
    // ============================================
    {
        RHI::TextureDesc desc = RHI::TextureDesc::DepthStencilWithSRV(w, h);
        desc.debugName = "Depth_Buffer";
        m_offDepth.reset(rhiCtx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // LDR sRGB Render Target (R8G8B8A8_TYPELESS with sRGB RTV + UNORM SRV)
    // ============================================
    {
        RHI::TextureDesc desc = RHI::TextureDesc::LDRRenderTarget(w, h);
        desc.debugName = "LDR_RenderTarget";
        m_offLDR.reset(rhiCtx->CreateTexture(desc, nullptr));
    }
}
