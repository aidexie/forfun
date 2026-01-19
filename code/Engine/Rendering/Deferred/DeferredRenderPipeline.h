#pragma once
#include "Engine/Rendering/RenderPipeline.h"
#include "Engine/SceneLightSettings.h"
#include "GBuffer.h"
#include "DepthPrePass.h"
#include "GBufferPass.h"
#include "DeferredLightingPass.h"
#include "TransparentForwardPass.h"
#include "Engine/Rendering/ShadowPass.h"
#include "Engine/Rendering/ClusteredLightingPass.h"
#include "Engine/Rendering/PostProcessPass.h"
#include "Engine/Rendering/BloomPass.h"
#include "Engine/Rendering/MotionBlurPass.h"
#include "Engine/Rendering/DepthOfFieldPass.h"
#include "Engine/Rendering/DebugLinePass.h"
#include "Engine/Rendering/GridPass.h"
#include "Engine/Rendering/SSAOPass.h"
#include "Engine/Rendering/HiZPass.h"
#include "Engine/Rendering/SSRPass.h"
#include "Engine/Rendering/AutoExposurePass.h"
#include "Engine/Rendering/TAAPass.h"
#include "Engine/Rendering/FSR2Pass.h"
#include "Engine/Rendering/AntiAliasingPass.h"
#include "RHI/RHIPointers.h"
#include "RHI/RHIHelpers.h"
#include <DirectXMath.h>

// ============================================
// CDeferredRenderPipeline - Deferred Rendering Pipeline
// ============================================
// True deferred rendering pipeline with depth pre-pass to eliminate overdraw.
//
// Pipeline Flow:
// 1. Depth Pre-Pass (LESS test, write ON) - Populate depth buffer
// 2. G-Buffer Pass (EQUAL test, write OFF) - Fill G-Buffer with geometry data
// 3. Shadow Pass - CSM for directional light
// 4. Deferred Lighting Pass - Screen-space lighting evaluation
// 5. Transparent Forward Pass - Forward render transparent objects
// 6. Post-Processing - Tone mapping, gamma correction
// 7. Debug/Editor overlays - Grid, debug lines
//
// Benefits:
// - Zero G-Buffer overdraw (each pixel processed exactly once)
// - Per-pixel lighting evaluation (100+ lights efficient)
// - Natural fit for screen-space effects (SSAO, SSR, etc.)
// ============================================
class CDeferredRenderPipeline : public CRenderPipeline
{
public:
    CDeferredRenderPipeline() = default;
    ~CDeferredRenderPipeline() override = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize() override;
    void Shutdown() override;

    // ============================================
    // Core Rendering
    // ============================================
    void Render(const RenderContext& ctx) override;

    // ============================================
    // Accessors
    // ============================================
    void* GetOffscreenSRV() const override {
        return m_offLDR ? RHI::GetNativeSRV(m_offLDR.get()) : nullptr;
    }
    void* GetOffscreenTexture() const override {
        return m_offLDR ? m_offLDR->GetNativeHandle() : nullptr;
    }
    RHI::ITexture* GetOffscreenTextureRHI() const override {
        return m_offLDR.get();
    }
    unsigned int GetOffscreenWidth() const override { return m_offscreenWidth; }
    unsigned int GetOffscreenHeight() const override { return m_offscreenHeight; }

    // Access internal passes
    CDebugLinePass& GetDebugLinePass() override { return m_debugLinePass; }
    CClusteredLightingPass& GetClusteredLightingPass() override { return m_clusteredLighting; }
    CSSAOPass& GetSSAOPass() { return m_ssaoPass; }
    CHiZPass& GetHiZPass() { return m_hiZPass; }
    CSSRPass& GetSSRPass() { return m_ssrPass; }
    CAutoExposurePass& GetAutoExposurePass() { return m_autoExposurePass; }
    CMotionBlurPass& GetMotionBlurPass() { return m_motionBlurPass; }
    CDepthOfFieldPass& GetDepthOfFieldPass() { return m_dofPass; }
    CTAAPass& GetTAAPass() { return m_taaPass; }
    CFSR2Pass& GetFSR2Pass() { return m_fsr2Pass; }
    CAntiAliasingPass& GetAAPass() { return m_aaPass; }
    CGBuffer& GetGBuffer() { return m_gbuffer; }

private:
    void ensureOffscreen(unsigned int w, unsigned int h);

    // ============================================
    // Render Passes
    // ============================================
    CDepthPrePass m_depthPrePass;
    CGBufferPass m_gbufferPass;
    CShadowPass m_shadowPass;
    CDeferredLightingPass m_lightingPass;
    CTransparentForwardPass m_transparentPass;
    CClusteredLightingPass m_clusteredLighting;
    CSSAOPass m_ssaoPass;
    CHiZPass m_hiZPass;
    CSSRPass m_ssrPass;
    CAutoExposurePass m_autoExposurePass;
    CTAAPass m_taaPass;
    CFSR2Pass m_fsr2Pass;
    CAntiAliasingPass m_aaPass;
    CBloomPass m_bloomPass;
    CMotionBlurPass m_motionBlurPass;
    CDepthOfFieldPass m_dofPass;
    CPostProcessPass m_postProcess;
    CDebugLinePass m_debugLinePass;

    // G-Buffer
    CGBuffer m_gbuffer;

    // ============================================
    // Offscreen Targets
    // ============================================
    RHI::TexturePtr m_offHDR;       // HDR intermediate (R16G16B16A16_FLOAT)
    RHI::TexturePtr m_offLDR;       // LDR final output (R8G8B8A8_TYPELESS)
    RHI::TexturePtr m_offLDR_PreAA; // LDR before AA (for AA input/output swap)
    unsigned int m_offscreenWidth = 0;
    unsigned int m_offscreenHeight = 0;

    // ============================================
    // Frame State
    // ============================================
    DirectX::XMMATRIX m_viewProjPrev = DirectX::XMMatrixIdentity();  // Previous frame VP matrix
    DirectX::XMFLOAT2 m_prevJitterOffset = {0.0f, 0.0f};             // Previous frame jitter offset (for TAA)

    // G-Buffer debug visualization resources
    RHI::ShaderPtr m_debugVS;
    RHI::ShaderPtr m_debugPS;
    RHI::PipelineStatePtr m_debugPSO;
    RHI::SamplerPtr m_debugSampler;

    void initDebugVisualization();
    void renderDebugVisualization(uint32_t width, uint32_t height);
};
