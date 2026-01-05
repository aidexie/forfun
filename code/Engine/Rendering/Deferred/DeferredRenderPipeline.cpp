#include "DeferredRenderPipeline.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/DirectionalLight.h"

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
            case 6:  // AO
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
                color = pow(depth, 50.0).xxx;  // Non-linear for better visibility
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

    m_bloomPass.Initialize();
    m_postProcess.Initialize();
    m_debugLinePass.Initialize();
    CGridPass::Instance().Initialize();

    // Initialize debug visualization
    initDebugVisualization();

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
    m_bloomPass.Shutdown();
    m_postProcess.Shutdown();
    m_debugLinePass.Shutdown();
    CGridPass::Instance().Shutdown();
    m_gbuffer.Shutdown();

    m_debugPSO.reset();
    m_debugVS.reset();
    m_debugPS.reset();
    m_debugSampler.reset();

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

    // Store current VP matrix for next frame's velocity calculation
    m_viewProjPrev = XMMatrixMultiply(ctx.camera.GetViewMatrix(), ctx.camera.GetProjectionMatrix());

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
    // 6. Deferred Lighting Pass
    // ============================================
    {
        // Debug visualization mode or full lighting
        if (m_debugMode != EGBufferDebugMode::None) {
            // Set HDR render target for debug vis
            ITexture* hdrRT = m_offHDR.get();
            cmdList->SetRenderTargets(1, &hdrRT, nullptr);
            cmdList->SetViewport(0, 0, (float)ctx.width, (float)ctx.height);
            cmdList->SetScissorRect(0, 0, ctx.width, ctx.height);

            const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            cmdList->ClearRenderTarget(m_offHDR.get(), clearColor);

            renderDebugVisualization(ctx.width, ctx.height);
        } else {
            // Full deferred lighting
            m_lightingPass.Render(ctx.camera, ctx.scene, m_gbuffer,
                                  m_offHDR.get(), ctx.width, ctx.height,
                                  &m_shadowPass, &m_clusteredLighting);
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
    // 7. Bloom Pass (HDR -> half-res bloom texture)
    // ============================================
    ITexture* bloomResult = nullptr;
    if (ctx.showFlags.PostProcessing) {
        const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
        if (bloomSettings.enabled) {
            CScopedDebugEvent evt(cmdList, L"Bloom");
            bloomResult = m_bloomPass.Render(
                m_offHDR.get(), ctx.width, ctx.height, bloomSettings);
        }
    }

    // ============================================
    // 8. Post-Processing (HDR -> LDR)
    // ============================================
    if (ctx.showFlags.PostProcessing) {
        CScopedDebugEvent evt(cmdList, L"Post-Processing");
        const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
        float bloomIntensity = (bloomSettings.enabled && bloomResult) ? bloomSettings.intensity : 0.0f;
        m_postProcess.Render(m_offHDR.get(), bloomResult, m_offLDR.get(),
                             ctx.width, ctx.height, 1.0f, bloomIntensity);
    } else {
        ITexture* ldrRT = m_offLDR.get();
        cmdList->SetRenderTargets(1, &ldrRT, nullptr);
        const float ldrClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        cmdList->ClearRenderTarget(m_offLDR.get(), ldrClearColor);
    }

    // ============================================
    // 9. Debug Lines (if enabled)
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
    // 10. Grid (if enabled)
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
    // 11. Copy to final output (if provided)
    // ============================================
    if (ctx.finalOutputTexture) {
        cmdList->UnbindRenderTargets();
        ITexture* sourceTexture = (ctx.outputFormat == RenderContext::EOutputFormat::HDR)
            ? m_offHDR.get()
            : m_offLDR.get();
        cmdList->CopyTextureToSlice(ctx.finalOutputTexture, ctx.finalOutputArraySlice, ctx.finalOutputMipLevel, sourceTexture);
    }

    // ============================================
    // 12. Transition LDR to SRV state
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
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_debugSampler.get());

    // Set debug mode
    struct {
        int debugMode;
        float _pad[3];
    } debugData = { static_cast<int>(m_debugMode), {0, 0, 0} };
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

    // HDR Render Target
    {
        TextureDesc desc = TextureDesc::RenderTarget(w, h, ETextureFormat::R16G16B16A16_FLOAT);
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
}
