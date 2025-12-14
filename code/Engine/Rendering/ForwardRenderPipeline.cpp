#include "ForwardRenderPipeline.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Camera.h"

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
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) return;

    RHI::ICommandList* cmdList = rhiCtx->GetCommandList();
    if (!cmdList) return;

    // ============================================
    // 0. Unbind resources to avoid hazards
    // ============================================
    cmdList->UnbindShaderResources(RHI::EShaderStage::Vertex, 0, 8);
    cmdList->UnbindShaderResources(RHI::EShaderStage::Pixel, 0, 8);
    cmdList->UnbindRenderTargets();

    // ============================================
    // 1. Ensure offscreen targets are ready
    // ============================================
    ensureOffscreen(ctx.width, ctx.height);

    // ============================================
    // DX12 Minimal Test: Only clear, skip complex rendering
    // TODO: Remove this flag once all features are working
    // ============================================
    const bool dx12MinimalMode = (RHI::CRHIManager::Instance().GetBackend() == RHI::EBackend::DX12);

    // ============================================
    // 2. Shadow Pass (if enabled)
    // ============================================
    const CShadowPass::Output* shadowData = nullptr;
    if (!dx12MinimalMode && ctx.showFlags.Shadows) {
        // Find DirectionalLight in scene
        SDirectionalLight* dirLight = nullptr;
        for (auto& objPtr : ctx.scene.GetWorld().Objects()) {
            dirLight = objPtr->GetComponent<SDirectionalLight>();
            if (dirLight) break;
        }

        if (dirLight) {
            RHI::CScopedDebugEvent evt(cmdList, L"Shadow Pass");
            m_shadowPass.Render(ctx.scene, dirLight,
                              ctx.camera.GetViewMatrix(),
                              ctx.camera.GetProjectionMatrix());
            shadowData = &m_shadowPass.GetOutput();
        }
    }

    // ============================================
    // 3. Clear HDR render target
    // ============================================
    RHI::ITexture* hdrRT = m_offHDR.get();
    cmdList->SetRenderTargets(1, &hdrRT, m_offDepth.get());
    cmdList->SetViewport(0, 0, (float)ctx.width, (float)ctx.height);
    cmdList->SetScissorRect(0, 0, ctx.width, ctx.height);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };  // Match optimizedClearValue in CreateTexture
    cmdList->ClearRenderTarget(m_offHDR.get(), clearColor);
    cmdList->ClearDepthStencil(m_offDepth.get(), true, 1.0f, true, 0);

    // ============================================
    // 4. Scene Rendering (Opaque + Transparent + Skybox)
    // ============================================
    if (!dx12MinimalMode) {
        RHI::CScopedDebugEvent evt(cmdList, L"Scene Rendering");
        m_sceneRenderer.Render(ctx.camera, ctx.scene,
                              m_offHDR.get(), m_offDepth.get(),
                              ctx.width, ctx.height, ctx.deltaTime,
                              shadowData);
    }

    // ============================================
    // 5. Post-Processing (HDR -> LDR)
    // ============================================
    if (!dx12MinimalMode && ctx.showFlags.PostProcessing) {
        RHI::CScopedDebugEvent evt(cmdList, L"Post-Processing");
        m_postProcess.Render(m_offHDR.get(), m_offLDR.get(),
                           ctx.width, ctx.height, 1.0f);
    } else {
        // DX12 minimal mode: Just copy HDR to LDR (or clear LDR)
        // For now, clear LDR with optimized clear color
        RHI::ITexture* ldrRT = m_offLDR.get();
        cmdList->SetRenderTargets(1, &ldrRT, nullptr);
        const float ldrClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };  // Match optimizedClearValue
        cmdList->ClearRenderTarget(m_offLDR.get(), ldrClearColor);
    }

    // ============================================
    // 6. Debug Lines (if enabled)
    // ============================================
    if (!dx12MinimalMode && ctx.showFlags.DebugLines) {
        RHI::CScopedDebugEvent evt(cmdList, L"Debug Lines");
        // Rebind LDR RTV + HDR depth (Debug lines need depth test)
        RHI::ITexture* ldrRT = m_offLDR.get();
        cmdList->SetRenderTargets(1, &ldrRT, m_offDepth.get());
        m_debugLinePass.Render(ctx.camera.GetViewMatrix(),
                              ctx.camera.GetProjectionMatrix(),
                              ctx.width, ctx.height);
    }

    // ============================================
    // 7. Grid (if enabled)
    // ============================================
    if (!dx12MinimalMode && ctx.showFlags.Grid) {
        RHI::CScopedDebugEvent evt(cmdList, L"Grid");
        // Rebind LDR RTV + HDR depth (Grid needs depth test)
        RHI::ITexture* ldrRT = m_offLDR.get();
        cmdList->SetRenderTargets(1, &ldrRT, m_offDepth.get());

        CGridPass::Instance().Render(ctx.camera.GetViewMatrix(),
                                     ctx.camera.GetProjectionMatrix(),
                                     ctx.camera.position);
    }

    // ============================================
    // 8. Copy final result to finalOutputTexture (if provided)
    // ============================================
    if (ctx.finalOutputTexture) {
        // Unbind all render targets before copy operations
        cmdList->UnbindRenderTargets();

        // Choose source based on outputFormat
        RHI::ITexture* sourceTexture = (ctx.outputFormat == RenderContext::EOutputFormat::HDR)
            ? m_offHDR.get()   // HDR linear
            : m_offLDR.get();  // LDR sRGB

        // Copy to destination texture slice
        cmdList->CopyTextureToSlice(ctx.finalOutputTexture, ctx.finalOutputArraySlice, ctx.finalOutputMipLevel, sourceTexture);
    }

    // ============================================
    // 9. Transition LDR to SRV state for ImGui viewport
    // ============================================
    cmdList->UnbindRenderTargets();
    cmdList->Barrier(m_offLDR.get(), RHI::EResourceState::RenderTarget, RHI::EResourceState::ShaderResource);
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
