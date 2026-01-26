#include "DeferredRenderPipeline.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerFrameSlots.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include "Engine/Rendering/ReflectionProbeManager.h"

using namespace DirectX;
using namespace RHI;

// ============================================
// Debug Visualization Shaders
// ============================================
namespace {

static const char* kDebugVisualizationVS = R"(
    struct VSOut {
        float4 posH : SV_Position;
        float2 uv : TEXCOORD0;
    };

    VSOut main(uint vertexID : SV_VertexID) {
        VSOut o;
        // Full-screen triangle
        o.uv = float2((vertexID << 1) & 2, vertexID & 2);
        o.posH = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
        return o;
    }
)";

static const char* kDebugVisualizationPS = R"(
    Texture2D gRT0 : register(t0);  // WorldPosition + Metallic
    Texture2D gRT1 : register(t1);  // Normal + Roughness
    Texture2D gRT2 : register(t2);  // Albedo + AO
    Texture2D gRT3 : register(t3);  // Emissive + MaterialID
    Texture2D gRT4 : register(t4);  // Velocity
    Texture2D gDepth : register(t5); // Depth
    Texture2D gSSAO : register(t6);  // Screen-Space Ambient Occlusion
    Texture2D gHiZ : register(t7);   // Hi-Z Pyramid
    Texture2D gSSR : register(t8);   // Screen-Space Reflections

    SamplerState gSamp : register(s0);

    cbuffer CB_Debug : register(b0) {
        int gDebugMode;
        float3 _pad;
    }

    struct PSIn {
        float4 posH : SV_Position;
        float2 uv : TEXCOORD0;
    };

    float4 main(PSIn i) : SV_Target {
        float4 rt0 = gRT0.Sample(gSamp, i.uv);
        float4 rt1 = gRT1.Sample(gSamp, i.uv);
        float4 rt2 = gRT2.Sample(gSamp, i.uv);
        float4 rt3 = gRT3.Sample(gSamp, i.uv);
        float2 rt4 = gRT4.Sample(gSamp, i.uv).xy;
        float depth = gDepth.Sample(gSamp, i.uv).r;
        float ssao = gSSAO.Sample(gSamp, i.uv).r;

        float3 color = float3(0, 0, 0);

        switch (gDebugMode) {
            case 1:  // WorldPosition
                color = frac(rt0.xyz * 0.1);  // Scale and wrap for visualization
                break;
            case 2:  // Normal
                color = rt1.xyz * 0.5 + 0.5;  // Map [-1,1] to [0,1]
                break;
            case 3:  // Albedo
                color = rt2.rgb;
                break;
            case 4:  // Metallic
                color = rt0.aaa;
                break;
            case 5:  // Roughness
                color = rt1.aaa;
                break;
            case 6:  // AO (Material AO)
                color = rt2.aaa;
                break;
            case 7:  // Emissive
                color = rt3.rgb;
                break;
            case 8:  // MaterialID
                color = rt3.aaa * 255.0 / 10.0;  // Scale for visibility
                break;
            case 9:  // Velocity
                color = float3(rt4.xy * 10.0 + 0.5, 0.5);  // Scale for visibility
                break;
            case 10: // Depth
                color = float3(depth*10,depth*10,depth*10);  // Non-linear for better visibility
                break;
            case 11: // SSAO
                color = ssao.xxx;
                break;
            case 12: // Hi-Z Mip 0
                color = gHiZ.SampleLevel(gSamp, i.uv, 0).rrr * 10.0;
                break;
            case 13: // Hi-Z Mip 1
                color = gHiZ.SampleLevel(gSamp, i.uv, 1).rrr * 10.0;
                break;
            case 14: // Hi-Z Mip 2
                color = gHiZ.SampleLevel(gSamp, i.uv, 2).rrr * 10.0;
                break;
            case 15: // Hi-Z Mip 3
                color = gHiZ.SampleLevel(gSamp, i.uv, 3).rrr * 10.0;
                break;
            case 16: // Hi-Z Mip 4
                color = gHiZ.SampleLevel(gSamp, i.uv, 4).rrr * 10.0;
                break;
            case 17: // SSR Result
                color = gSSR.Sample(gSamp, i.uv).rgb;
                break;
            case 18: // SSR Confidence
                color = gSSR.Sample(gSamp, i.uv).aaa;
                break;
            default:
                color = rt2.rgb;  // Default to albedo
                break;
        }

        return float4(color, 1.0);
    }
)";

} // anonymous namespace

bool CDeferredRenderPipeline::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Initialize passes
    if (!m_depthPrePass.Initialize()) {
        CFFLog::Error("Failed to initialize DepthPrePass");
        return false;
    }

    if (!m_gbufferPass.Initialize()) {
        CFFLog::Error("Failed to initialize GBufferPass");
        return false;
    }

    if (!m_shadowPass.Initialize()) {
        CFFLog::Error("Failed to initialize ShadowPass");
        return false;
    }

    if (!m_lightingPass.Initialize()) {
        CFFLog::Error("Failed to initialize DeferredLightingPass");
        return false;
    }

    if (!m_transparentPass.Initialize()) {
        CFFLog::Error("Failed to initialize TransparentForwardPass");
        return false;
    }

    m_clusteredLighting.Initialize();
    m_ssaoPass.Initialize();
    m_hiZPass.Initialize();
    m_ssrPass.Initialize();
    m_autoExposurePass.Initialize();
    m_taaPass.Initialize();
    m_fsr2Pass.Initialize();  // FSR 2.0 (DX12 only, no-op on DX11)
    m_aaPass.Initialize();

    m_bloomPass.Initialize();
    m_motionBlurPass.Initialize();
    m_dofPass.Initialize();
    m_postProcess.Initialize();
    m_debugLinePass.Initialize();
    CGridPass::Instance().Initialize();

    // Initialize debug visualization
    initDebugVisualization();

    // Create PerFrame descriptor set for descriptor set-based passes
    createPerFrameDescriptorSet();

    CFFLog::Info("DeferredRenderPipeline initialized");
    return true;
}

void CDeferredRenderPipeline::Shutdown()
{
    m_depthPrePass.Shutdown();
    m_gbufferPass.Shutdown();
    m_shadowPass.Shutdown();
    m_lightingPass.Shutdown();
    m_transparentPass.Shutdown();
    m_clusteredLighting.Shutdown();
    m_ssaoPass.Shutdown();
    m_hiZPass.Shutdown();
    m_ssrPass.Shutdown();
    m_autoExposurePass.Shutdown();
    m_taaPass.Shutdown();
    m_fsr2Pass.Shutdown();
    m_aaPass.Shutdown();
    m_bloomPass.Shutdown();
    m_motionBlurPass.Shutdown();
    m_dofPass.Shutdown();
    m_postProcess.Shutdown();
    m_debugLinePass.Shutdown();
    CGridPass::Instance().Shutdown();
    m_gbuffer.Shutdown();

    m_debugPSO.reset();
    m_debugVS.reset();
    m_debugPS.reset();
    m_debugSampler.reset();

    // Cleanup PerFrame descriptor set
    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perFrameSet) {
            ctx->FreeDescriptorSet(m_perFrameSet);
            m_perFrameSet = nullptr;
        }
        if (m_perFrameLayout) {
            ctx->DestroyDescriptorSetLayout(m_perFrameLayout);
            m_perFrameLayout = nullptr;
        }
    }
    m_linearClampSampler.reset();
    m_linearWrapSampler.reset();
    m_pointClampSampler.reset();
    m_shadowCmpSampler.reset();
    m_anisoSampler.reset();

    m_offHDR.reset();
    m_offLDR.reset();
    m_offscreenWidth = 0;
    m_offscreenHeight = 0;
}

void CDeferredRenderPipeline::initDebugVisualization()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile debug visualization shaders
    SCompiledShader vsCompiled = CompileShaderFromSource(kDebugVisualizationVS, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("Debug visualization VS error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    SCompiledShader psCompiled = CompileShaderFromSource(kDebugVisualizationPS, "main", "ps_5_0", nullptr, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("Debug visualization PS error: %s", psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "GBufferDebug_VS";
    m_debugVS.reset(ctx->CreateShader(vsDesc));

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "GBufferDebug_PS";
    m_debugPS.reset(ctx->CreateShader(psDesc));

    // Create PSO
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_debugVS.get();
    psoDesc.pixelShader = m_debugPS.get();
    psoDesc.inputLayout = {};  // No vertex input (full-screen triangle from vertexID)
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.blend.blendEnable = false;
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoDesc.depthStencilFormat = ETextureFormat::Unknown;
    psoDesc.debugName = "GBufferDebug_PSO";
    m_debugPSO.reset(ctx->CreatePipelineState(psoDesc));

    // Create sampler
    SamplerDesc sampDesc;
    sampDesc.filter = EFilter::MinMagMipPoint;
    sampDesc.addressU = ETextureAddressMode::Clamp;
    sampDesc.addressV = ETextureAddressMode::Clamp;
    m_debugSampler.reset(ctx->CreateSampler(sampDesc));
}

void CDeferredRenderPipeline::Render(const RenderContext& ctx)
{
    IRenderContext* rhiCtx = CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) return;

    ICommandList* cmdList = rhiCtx->GetCommandList();
    if (!cmdList) return;

    // ============================================
    // 0. Unbind resources to avoid hazards
    // ============================================
    cmdList->UnbindShaderResources(EShaderStage::Vertex, 0, 8);
    cmdList->UnbindShaderResources(EShaderStage::Pixel, 0, 8);
    cmdList->UnbindRenderTargets();

    // ============================================
    // 1. Ensure offscreen targets and G-Buffer are ready
    // ============================================
    ensureOffscreen(ctx.width, ctx.height);
    m_gbuffer.Resize(ctx.width, ctx.height);

    // ============================================
    // 1.5. Enable/disable camera jitter for TAA
    // ============================================
    // Note: We need to cast away const because camera jitter state needs to be updated
    // Only enable jitter when TAA is on AND algorithm actually does temporal work
    CCamera& camera = const_cast<CCamera&>(ctx.camera);
    bool taaActive = ctx.showFlags.TAA && m_taaPass.GetSettings().algorithm != ETAAAlgorithm::Off;
    camera.SetTAAEnabled(taaActive);
    camera.SetJitterSampleCount(m_taaPass.GetSettings().jitter_samples);

    // ============================================
    // 2. Depth Pre-Pass
    // ============================================
    {
        m_depthPrePass.Render(ctx.camera, ctx.scene, m_gbuffer.GetDepthBuffer(), ctx.width, ctx.height);
    }

    // ============================================
    // 3. G-Buffer Pass
    // ============================================
    {
        m_gbufferPass.Render(ctx.camera, ctx.scene, m_gbuffer, m_viewProjPrev, ctx.width, ctx.height);
    }

    // Advance jitter for next frame (if TAA enabled)
    camera.AdvanceJitter();

    // ============================================
    // 3.5. Hi-Z Pass (Hierarchical-Z Depth Pyramid)
    // ============================================
    if (ctx.showFlags.HiZ) {
        CScopedDebugEvent evt(cmdList, L"Hi-Z Build");
        m_hiZPass.BuildPyramid(cmdList,
                               m_gbuffer.GetDepthBuffer(),
                               ctx.width, ctx.height);
    }

    // ============================================
    // 4. Shadow Pass (if enabled)
    // ============================================
    const CShadowPass::Output* shadowData = nullptr;
    if (ctx.showFlags.Shadows) {
        SDirectionalLight* dirLight = nullptr;
        for (auto& objPtr : ctx.scene.GetWorld().Objects()) {
            dirLight = objPtr->GetComponent<SDirectionalLight>();
            if (dirLight) break;
        }

        if (dirLight) {
            CScopedDebugEvent evt(cmdList, L"Shadow Pass");
            m_shadowPass.Render(ctx.scene, dirLight,
                               ctx.camera.GetViewMatrix(),
                               ctx.camera.GetProjectionMatrix());
            shadowData = &m_shadowPass.GetOutput();
        }
    }

    // ============================================
    // 5. Clustered Lighting Compute (build light grid)
    // ============================================
    {
        CScopedDebugEvent evt(cmdList, L"Clustered Lighting Compute");
        m_clusteredLighting.Resize(ctx.width, ctx.height);
        m_clusteredLighting.BuildClusterGrid(cmdList,
                                             ctx.camera.GetProjectionMatrix(),
                                             ctx.camera.nearZ, ctx.camera.farZ);
        m_clusteredLighting.CullLights(cmdList, &ctx.scene, ctx.camera.GetViewMatrix());
    }

    // ============================================
    // 5.5. SSAO Pass (Screen-Space Ambient Occlusion)
    // ============================================
    if (ctx.showFlags.SSAO) {
        CScopedDebugEvent evt(cmdList, L"SSAO Pass");
        m_ssaoPass.Render(cmdList,
                          m_gbuffer.GetDepthBuffer(),
                          m_gbuffer.GetNormalRoughness(),
                          ctx.width, ctx.height,
                          ctx.camera.GetViewMatrix(),
                          ctx.camera.GetProjectionMatrix(),
                          ctx.camera.nearZ, ctx.camera.farZ);
    }
    // Always get SSAO texture (returns white fallback when disabled)
    RHI::ITexture* ssaoTexture = m_ssaoPass.GetSSAOTexture();

    // ============================================
    // 6. Deferred Lighting Pass
    // ============================================
    {
        EGBufferDebugMode debugMode = CScene::Instance().GetLightSettings().gBufferDebugMode;

        // SSR debug modes require full lighting pipeline to have valid HDR data
        bool isSSRDebug = (debugMode == EGBufferDebugMode::SSR_Result ||
                          debugMode == EGBufferDebugMode::SSR_Confidence);
        bool runLighting = (debugMode == EGBufferDebugMode::None || isSSRDebug);

        if (runLighting) {
            // Use descriptor set API if available (DX12), otherwise fall back to legacy
            if (m_perFrameSet && m_lightingPass.IsDescriptorSetModeAvailable()) {
                // Populate PerFrame set with current frame data
                populatePerFrameSet(ctx, shadowData);

                // Full deferred lighting with descriptor sets
                m_lightingPass.Render(ctx.camera, ctx.scene, m_gbuffer,
                                      m_offHDR.get(), ctx.width, ctx.height,
                                      &m_shadowPass, m_perFrameSet,
                                      ssaoTexture);
            } else {
                // Legacy path (DX11 or fallback)
                m_lightingPass.Render(ctx.camera, ctx.scene, m_gbuffer,
                                      m_offHDR.get(), ctx.width, ctx.height,
                                      &m_shadowPass, &m_clusteredLighting,
                                      ssaoTexture);
            }
        } else {
            // Non-SSR debug modes: clear HDR to black
            ITexture* hdrRT = m_offHDR.get();
            cmdList->SetRenderTargets(1, &hdrRT, nullptr);
            cmdList->SetViewport(0, 0, (float)ctx.width, (float)ctx.height);
            cmdList->SetScissorRect(0, 0, ctx.width, ctx.height);
            const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            cmdList->ClearRenderTarget(m_offHDR.get(), clearColor);
        }
    }

    // ============================================
    // 6.5. Skybox Pass
    // ============================================
    // Render skybox after deferred lighting, before transparent objects
    // Skybox renders at depth=1.0 with LessEqual test
    {
        RHI::CScopedDebugEvent evt(cmdList, L"Skybox");
        // Bind HDR RT + depth for skybox rendering
        ITexture* hdrRT = m_offHDR.get();
        cmdList->SetRenderTargets(1, &hdrRT, m_gbuffer.GetDepthBuffer());
        cmdList->SetViewport(0, 0, (float)ctx.width, (float)ctx.height, 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, ctx.width, ctx.height);
        ctx.scene.GetSkybox().Render(ctx.camera.GetViewMatrix(), ctx.camera.GetProjectionMatrix());
    }

    // ============================================
    // 6.6. Transparent Forward Pass
    // ============================================
    // Render transparent objects using forward shading
    // (cannot be deferred due to blending requirements)
    {
        m_transparentPass.Render(ctx.camera, ctx.scene,
                                 m_offHDR.get(), m_gbuffer.GetDepthBuffer(),
                                 ctx.width, ctx.height,
                                 shadowData, &m_clusteredLighting);
    }

    // ============================================
    // 6.7. SSR Pass (Screen-Space Reflections)
    // ============================================
    // Traces against HDR color buffer using Hi-Z acceleration
    if (ctx.showFlags.SSR && ctx.showFlags.HiZ) {
        CScopedDebugEvent evt(cmdList, L"SSR Pass");
        m_ssrPass.Render(cmdList,
                         m_gbuffer.GetDepthBuffer(),
                         m_gbuffer.GetNormalRoughness(),
                         m_hiZPass.GetHiZTexture(),
                         m_offHDR.get(),
                         ctx.width, ctx.height,
                         m_hiZPass.GetMipCount(),
                         ctx.camera.GetViewMatrix(),
                         ctx.camera.GetProjectionMatrix(),
                         ctx.camera.nearZ, ctx.camera.farZ);

        // Composite SSR results into HDR buffer
        CScopedDebugEvent compEvt(cmdList, L"SSR Composite");
        m_ssrPass.Composite(cmdList,
                            m_offHDR.get(),
                            m_gbuffer.GetWorldPosMetallic(),
                            m_gbuffer.GetNormalRoughness(),
                            ctx.width, ctx.height,
                            ctx.camera.position);
    }

    // ============================================
    // 6.8. Debug Visualization (after SSR for valid SSR debug modes)
    // ============================================
    {
        EGBufferDebugMode debugMode = CScene::Instance().GetLightSettings().gBufferDebugMode;
        if (debugMode != EGBufferDebugMode::None) {
            ITexture* hdrRT = m_offHDR.get();
            cmdList->SetRenderTargets(1, &hdrRT, nullptr);
            cmdList->SetViewport(0, 0, (float)ctx.width, (float)ctx.height);
            cmdList->SetScissorRect(0, 0, ctx.width, ctx.height);
            renderDebugVisualization(ctx.width, ctx.height);
        }
    }

    // ============================================
    // 6.9. TAA Pass / FSR2 Pass (Temporal Anti-Aliasing / Upscaling)
    // ============================================
    // TAA/FSR2 runs in HDR space, after SSR and before Auto Exposure
    // FSR2 replaces TAA when enabled (provides both temporal AA and upscaling)
    ITexture* hdrAfterTAA = m_offHDR.get();
    const auto& fsr2Settings = ctx.scene.GetLightSettings().fsr2;

    if (fsr2Settings.enabled && m_fsr2Pass.IsSupported()) {
        // FSR 2.0 Path
        CScopedDebugEvent evt(cmdList, L"FSR2 Pass");

        // Ensure FSR2 resources are ready
        m_fsr2Pass.EnsureResources(ctx.width, ctx.height, fsr2Settings);

        if (m_fsr2Pass.IsReady()) {
            // Get frame index for jitter
            uint32_t frameIndex = ctx.camera.GetJitterFrameIndex();

            // FSR2 needs delta time in milliseconds
            float deltaTimeMs = ctx.deltaTime * 1000.0f;

            // Render FSR2 (in-place for NativeAA mode, upscaling for other modes)
            // For now, we use the same HDR buffer as input/output (native resolution)
            m_fsr2Pass.Render(cmdList,
                              m_offHDR.get(),
                              m_gbuffer.GetDepthBuffer(),
                              m_gbuffer.GetVelocity(),
                              m_offHDR.get(),  // Output same buffer for now
                              ctx.camera,
                              deltaTimeMs,
                              frameIndex,
                              fsr2Settings);

            hdrAfterTAA = m_offHDR.get();
        }
    } else if (ctx.showFlags.TAA) {
        // TAA Path (fallback when FSR2 disabled or unsupported)
        CScopedDebugEvent evt(cmdList, L"TAA Pass");

        // Get current jitter offset
        XMFLOAT2 currentJitter = ctx.camera.GetJitterOffset();

        // Get current view-projection matrix (with jitter if TAA enabled)
        XMMATRIX viewProj = XMMatrixMultiply(ctx.camera.GetViewMatrix(),
                                              ctx.camera.GetJitteredProjectionMatrix(ctx.width, ctx.height));

        m_taaPass.Render(cmdList,
                         m_offHDR.get(),
                         m_gbuffer.GetVelocity(),
                         m_gbuffer.GetDepthBuffer(),
                         ctx.width, ctx.height,
                         viewProj,
                         m_viewProjPrev,
                         currentJitter,
                         m_prevJitterOffset);

        // Use TAA output for subsequent passes
        hdrAfterTAA = m_taaPass.GetOutput();

        // Store jitter for next frame
        m_prevJitterOffset = currentJitter;
    }

    // Store current VP matrix for next frame's velocity calculation
    // Must be updated AFTER TAA uses m_viewProjPrev, not before
    m_viewProjPrev = XMMatrixMultiply(ctx.camera.GetViewMatrix(),
                                       ctx.camera.GetJitteredProjectionMatrix(ctx.width, ctx.height));

    // ============================================
    // 7. Auto Exposure (HDR luminance analysis)
    // ============================================
    RHI::IBuffer* exposureBuffer = nullptr;
    if (ctx.showFlags.AutoExposure) {
        const auto& aeSettings = ctx.scene.GetLightSettings().autoExposure;
        CScopedDebugEvent evt(cmdList, L"Auto Exposure");
        m_autoExposurePass.Render(cmdList, hdrAfterTAA, ctx.width, ctx.height,
                                  ctx.deltaTime, aeSettings);
        exposureBuffer = m_autoExposurePass.GetExposureBuffer();
    }

    // ============================================
    // 8. Motion Blur Pass (HDR -> motion-blurred HDR)
    // ============================================
    ITexture* hdrAfterMotionBlur = hdrAfterTAA;
    if (ctx.showFlags.MotionBlur) {
        const auto& mbSettings = ctx.scene.GetLightSettings().motionBlur;
        CScopedDebugEvent evt(cmdList, L"Motion Blur");
        hdrAfterMotionBlur = m_motionBlurPass.Render(
            hdrAfterTAA, m_gbuffer.GetVelocity(),
            ctx.width, ctx.height, mbSettings);
    }

    // ============================================
    // 8.5. Depth of Field Pass (HDR -> focus-blurred HDR)
    // ============================================
    ITexture* hdrAfterDoF = hdrAfterMotionBlur;
    if (ctx.showFlags.DepthOfField) {
        const auto& dofSettings = ctx.scene.GetLightSettings().depthOfField;
        CScopedDebugEvent evt(cmdList, L"Depth of Field");
        hdrAfterDoF = m_dofPass.Render(
            hdrAfterMotionBlur, m_gbuffer.GetDepthBuffer(),
            ctx.camera.nearZ, ctx.camera.farZ,
            ctx.width, ctx.height, dofSettings);
    }

    // ============================================
    // 9. Bloom Pass (HDR -> half-res bloom texture)
    // ============================================
    ITexture* bloomResult = nullptr;
    if (ctx.showFlags.Bloom) {
        const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
        CScopedDebugEvent evt(cmdList, L"Bloom");
        bloomResult = m_bloomPass.Render(
            hdrAfterDoF, ctx.width, ctx.height, bloomSettings);
    }

    // ============================================
    // 10. Post-Processing (HDR -> LDR)
    // ============================================
    // Determine if AA is enabled to decide output target
    const auto& aaSettings = ctx.scene.GetLightSettings().antiAliasing;
    bool aaEnabled = ctx.showFlags.AntiAliasing && m_aaPass.IsEnabled(aaSettings);
    ITexture* postProcessOutput = aaEnabled ? m_offLDR_PreAA.get() : m_offLDR.get();

    if (ctx.showFlags.PostProcessing) {
        CScopedDebugEvent evt(cmdList, L"Post-Processing");
        const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
        float bloomIntensity = (ctx.showFlags.Bloom && bloomResult) ? bloomSettings.intensity : 0.0f;
        m_postProcess.Render(hdrAfterDoF, bloomResult, postProcessOutput,
                             ctx.width, ctx.height, 1.0f, exposureBuffer, bloomIntensity,
                             &ctx.scene.GetLightSettings().colorGrading,
                             ctx.showFlags.ColorGrading);
    } else {
        ITexture* ldrRT = postProcessOutput;
        cmdList->SetRenderTargets(1, &ldrRT, nullptr);
        const float ldrClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        cmdList->ClearRenderTarget(postProcessOutput, ldrClearColor);
    }

    // ============================================
    // 10.5. Anti-Aliasing (FXAA/SMAA)
    // ============================================
    if (aaEnabled) {
        CScopedDebugEvent evt(cmdList, L"Anti-Aliasing");
        m_aaPass.Render(m_offLDR_PreAA.get(), m_offLDR.get(),
                        ctx.width, ctx.height, aaSettings);
    }

    // ============================================
    // 11. Debug Lines (if enabled)
    // ============================================
    if (ctx.showFlags.DebugLines) {
        CScopedDebugEvent evt(cmdList, L"Debug Lines");
        ITexture* ldrRT = m_offLDR.get();
        cmdList->SetRenderTargets(1, &ldrRT, m_gbuffer.GetDepthBuffer());
        m_debugLinePass.Render(ctx.camera.GetViewMatrix(),
                              ctx.camera.GetProjectionMatrix(),
                              ctx.width, ctx.height);
    }

    // ============================================
    // 11. Grid (if enabled)
    // ============================================
    if (ctx.showFlags.Grid) {
        CScopedDebugEvent evt(cmdList, L"Grid");
        ITexture* ldrRT = m_offLDR.get();
        cmdList->SetRenderTargets(1, &ldrRT, m_gbuffer.GetDepthBuffer());
        CGridPass::Instance().Render(ctx.camera.GetViewMatrix(),
                                     ctx.camera.GetProjectionMatrix(),
                                     ctx.camera.position);
    }

    // ============================================
    // 12. Auto Exposure Debug Overlay (if enabled)
    // ============================================
    if (ctx.showFlags.AutoExposure) {
        CScopedDebugEvent evt(cmdList, L"Auto Exposure Debug");
        m_autoExposurePass.RenderDebugOverlay(cmdList, m_offLDR.get(), ctx.width, ctx.height);
    }

    // ============================================
    // 13. Copy to final output (if provided)
    // ============================================
    if (ctx.finalOutputTexture) {
        cmdList->UnbindRenderTargets();
        ITexture* sourceTexture = (ctx.outputFormat == RenderContext::EOutputFormat::HDR)
            ? m_offHDR.get()
            : m_offLDR.get();
        cmdList->CopyTextureToSlice(ctx.finalOutputTexture, ctx.finalOutputArraySlice, ctx.finalOutputMipLevel, sourceTexture);
    }

    // ============================================
    // 14. Transition LDR to SRV state
    // ============================================
    cmdList->UnbindRenderTargets();
    cmdList->Barrier(m_offLDR.get(), EResourceState::RenderTarget, EResourceState::ShaderResource);
}

void CDeferredRenderPipeline::renderDebugVisualization(uint32_t width, uint32_t height)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    if (!m_debugPSO) return;

    cmdList->SetPipelineState(m_debugPSO.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Bind G-Buffer textures
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, m_gbuffer.GetWorldPosMetallic());
    cmdList->SetShaderResource(EShaderStage::Pixel, 1, m_gbuffer.GetNormalRoughness());
    cmdList->SetShaderResource(EShaderStage::Pixel, 2, m_gbuffer.GetAlbedoAO());
    cmdList->SetShaderResource(EShaderStage::Pixel, 3, m_gbuffer.GetEmissiveMaterialID());
    cmdList->SetShaderResource(EShaderStage::Pixel, 4, m_gbuffer.GetVelocity());
    cmdList->SetShaderResource(EShaderStage::Pixel, 5, m_gbuffer.GetDepthBuffer());
    cmdList->SetShaderResource(EShaderStage::Pixel, 6, m_ssaoPass.GetSSAOTexture());
    cmdList->SetShaderResource(EShaderStage::Pixel, 7, m_hiZPass.GetHiZTexture());
    cmdList->SetShaderResource(EShaderStage::Pixel, 8, m_ssrPass.GetSSRTexture());
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_debugSampler.get());

    // Set debug mode
    struct {
        int debugMode;
        float _pad[3];
    } debugData = { static_cast<int>(CScene::Instance().GetLightSettings().gBufferDebugMode), {0, 0, 0} };
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &debugData, sizeof(debugData));

    // Draw full-screen triangle (3 vertices, no vertex buffer)
    cmdList->Draw(3, 0);
}

void CDeferredRenderPipeline::ensureOffscreen(unsigned int w, unsigned int h)
{
    IRenderContext* rhiCtx = CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) return;

    if (w == 0 || h == 0) return;
    if (m_offHDR && w == m_offscreenWidth && h == m_offscreenHeight) return;

    m_offscreenWidth = w;
    m_offscreenHeight = h;

    // HDR Render Target (with UAV for SSR composite)
    {
        TextureDesc desc = TextureDesc::RenderTarget(w, h, ETextureFormat::R16G16B16A16_FLOAT);
        desc.usage = desc.usage | ETextureUsage::UnorderedAccess;  // For SSR composite
        desc.debugName = "Deferred_HDR_RT";
        // Set optimized clear color (matches ClearRenderTarget calls)
        desc.clearColor[0] = 0.0f;
        desc.clearColor[1] = 0.0f;
        desc.clearColor[2] = 0.0f;
        desc.clearColor[3] = 1.0f;
        m_offHDR.reset(rhiCtx->CreateTexture(desc, nullptr));
    }

    // LDR sRGB Render Target
    {
        TextureDesc desc = TextureDesc::LDRRenderTarget(w, h);
        desc.debugName = "Deferred_LDR_RT";
        // Set optimized clear color (matches ClearRenderTarget calls)
        desc.clearColor[0] = 0.0f;
        desc.clearColor[1] = 0.0f;
        desc.clearColor[2] = 0.0f;
        desc.clearColor[3] = 1.0f;
        m_offLDR.reset(rhiCtx->CreateTexture(desc, nullptr));
    }

    // LDR Pre-AA Render Target (for AA input/output swap)
    // Uses sRGB SRV so AA shaders read linear values (automatic sRGB→linear conversion)
    // This prevents double gamma encoding: PostProcess→sRGB storage→linear read→AA→sRGB output
    {
        TextureDesc desc = TextureDesc::LDRRenderTarget(w, h);
        desc.debugName = "Deferred_LDR_PreAA_RT";
        desc.srvFormat = ETextureFormat::R8G8B8A8_UNORM_SRGB;  // Read as linear for correct AA processing
        desc.clearColor[0] = 0.0f;
        desc.clearColor[1] = 0.0f;
        desc.clearColor[2] = 0.0f;
        desc.clearColor[3] = 1.0f;
        m_offLDR_PreAA.reset(rhiCtx->CreateTexture(desc, nullptr));
    }
}

void CDeferredRenderPipeline::createPerFrameDescriptorSet()
{
    using namespace PerFrameSlots;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[DeferredRenderPipeline] DX11 mode - descriptor sets not supported, skipping PerFrame set");
        return;
    }

    // Create samplers for PerFrame set
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        m_linearClampSampler.reset(ctx->CreateSampler(desc));
    }
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Wrap;
        desc.addressV = ETextureAddressMode::Wrap;
        desc.addressW = ETextureAddressMode::Wrap;
        m_linearWrapSampler.reset(ctx->CreateSampler(desc));
    }
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipPoint;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        m_pointClampSampler.reset(ctx->CreateSampler(desc));
    }
    {
        SamplerDesc desc;
        desc.filter = EFilter::ComparisonMinMagMipLinear;
        desc.addressU = ETextureAddressMode::Border;
        desc.addressV = ETextureAddressMode::Border;
        desc.addressW = ETextureAddressMode::Border;
        desc.borderColor[0] = 1.0f;
        desc.borderColor[1] = 1.0f;
        desc.borderColor[2] = 1.0f;
        desc.borderColor[3] = 1.0f;
        desc.comparisonFunc = EComparisonFunc::LessEqual;
        m_shadowCmpSampler.reset(ctx->CreateSampler(desc));
    }
    {
        SamplerDesc desc;
        desc.filter = EFilter::Anisotropic;
        desc.maxAnisotropy = 16;
        desc.addressU = ETextureAddressMode::Wrap;
        desc.addressV = ETextureAddressMode::Wrap;
        desc.addressW = ETextureAddressMode::Wrap;
        m_anisoSampler.reset(ctx->CreateSampler(desc));
    }

    // Create PerFrame layout matching PerFrameSlots.h
    BindingLayoutDesc layoutDesc("PerFrame");

    // Constant buffers (b0-b3)
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(CB::PerFrame, sizeof(RHI::CB_PerFrame)));
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(CB::Clustered, 64));  // CB_ClusteredParams
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(CB::Volumetric, sizeof(CB_VolumetricLightmap)));
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(CB::ReflectionProbe, sizeof(CReflectionProbeManager::CB_Probes)));

    // Global textures (t0-t3)
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::ShadowMapArray));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::BrdfLUT));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::IrradianceArray));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::PrefilteredArray));

    // Clustered lighting (t4-t6)
    layoutDesc.AddItem(BindingLayoutItem::Buffer_SRV(Tex::Clustered_LightIndexList));
    layoutDesc.AddItem(BindingLayoutItem::Buffer_SRV(Tex::Clustered_LightGrid));
    layoutDesc.AddItem(BindingLayoutItem::Buffer_SRV(Tex::Clustered_LightData));

    // Volumetric lightmap (t8-t11)
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::Volumetric_SH_R));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::Volumetric_SH_G));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::Volumetric_SH_B));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(Tex::Volumetric_Octree));

    // Note: ReflectionProbe_Array (t13) and ReflectionProbe_Indices (t14) are reserved
    // for future per-object probe selection but not yet implemented.
    // They are intentionally omitted from the layout to avoid null descriptor handles.

    // Samplers (s0-s4)
    layoutDesc.AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp));
    layoutDesc.AddItem(BindingLayoutItem::Sampler(Samp::LinearWrap));
    layoutDesc.AddItem(BindingLayoutItem::Sampler(Samp::PointClamp));
    layoutDesc.AddItem(BindingLayoutItem::Sampler(Samp::ShadowCmp));
    layoutDesc.AddItem(BindingLayoutItem::Sampler(Samp::Aniso));

    m_perFrameLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perFrameLayout) {
        CFFLog::Error("[DeferredRenderPipeline] Failed to create PerFrame layout");
        return;
    }

    m_perFrameSet = ctx->AllocateDescriptorSet(m_perFrameLayout);
    if (!m_perFrameSet) {
        CFFLog::Error("[DeferredRenderPipeline] Failed to allocate PerFrame set");
        return;
    }

    // Bind static samplers
    m_perFrameSet->Bind({
        BindingSetItem::Sampler(Samp::LinearClamp, m_linearClampSampler.get()),
        BindingSetItem::Sampler(Samp::LinearWrap, m_linearWrapSampler.get()),
        BindingSetItem::Sampler(Samp::PointClamp, m_pointClampSampler.get()),
        BindingSetItem::Sampler(Samp::ShadowCmp, m_shadowCmpSampler.get()),
        BindingSetItem::Sampler(Samp::Aniso, m_anisoSampler.get())
    });

    CFFLog::Info("[DeferredRenderPipeline] PerFrame descriptor set created");

    // Now that PerFrame layout is available, create PSOs for passes that need both layouts
    m_lightingPass.CreatePSOWithLayouts(m_perFrameLayout);
}

void CDeferredRenderPipeline::populatePerFrameSet(const RenderContext& ctx, const CShadowPass::Output* shadowData)
{
    using namespace PerFrameSlots;

    if (!m_perFrameSet) return;

    // Build CB_PerFrame
    RHI::CB_PerFrame cb = {};

    XMMATRIX view = ctx.camera.GetViewMatrix();
    XMMATRIX proj = ctx.camera.GetProjectionMatrix();
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    cb.view = XMMatrixTranspose(view);
    cb.proj = XMMatrixTranspose(proj);
    cb.viewProj = XMMatrixTranspose(viewProj);
    cb.invView = XMMatrixTranspose(XMMatrixInverse(nullptr, view));
    cb.invProj = XMMatrixTranspose(XMMatrixInverse(nullptr, proj));
    cb.invViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj));
    cb.cameraPos = ctx.camera.position;
    cb.time = 0.0f;  // TODO: Get from time manager
    cb.screenSize = { (float)ctx.width, (float)ctx.height };
    cb.nearZ = ctx.camera.nearZ;
    cb.farZ = ctx.camera.farZ;

    m_perFrameSet->Bind(BindingSetItem::VolatileCBV(CB::PerFrame, &cb, sizeof(cb)));

    // Bind shadow map
    if (shadowData && shadowData->shadowMapArray) {
        m_perFrameSet->Bind(BindingSetItem::Texture_SRV(Tex::ShadowMapArray, shadowData->shadowMapArray));
    }

    // Let subsystems populate their bindings
    m_clusteredLighting.PopulatePerFrameSet(m_perFrameSet);
    ctx.scene.GetVolumetricLightmap().PopulatePerFrameSet(m_perFrameSet);
    ctx.scene.GetProbeManager().PopulatePerFrameSet(m_perFrameSet);
}
