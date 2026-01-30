#include "SSAOPass.h"
#include "ComputePassLayout.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/RenderConfig.h"
#include <cmath>
#include <random>

using namespace DirectX;
using namespace RHI;

namespace {

// Helper to calculate dispatch group count
uint32_t calcDispatchGroups(uint32_t size) {
    return (size + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
}

// Helper to compile a compute shader and create its PSO
bool createComputeShaderAndPSO(IRenderContext* ctx,
                                const std::string& shaderPath,
                                const char* entryPoint,
                                const char* shaderDebugName,
                                const char* psoDebugName,
                                bool debugShaders,
                                ShaderPtr& outShader,
                                PipelineStatePtr& outPSO) {
    SCompiledShader compiled = CompileShaderFromFile(shaderPath, entryPoint, "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[SSAOPass] %s compilation failed: %s", entryPoint, compiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    shaderDesc.debugName = shaderDebugName;
    outShader.reset(ctx->CreateShader(shaderDesc));

    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = outShader.get();
    psoDesc.debugName = psoDebugName;
    outPSO.reset(ctx->CreateComputePipelineState(psoDesc));

    return true;
}

// Helper to create a half-res R8 texture
TexturePtr createHalfResTexture(IRenderContext* ctx,
                                 uint32_t width,
                                 uint32_t height,
                                 ETextureFormat format,
                                 const char* debugName) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
    desc.clearColor[0] = 1.0f;
    desc.debugName = debugName;
    return TexturePtr(ctx->CreateTexture(desc, nullptr));
}

}  // namespace

// ============================================
// Lifecycle
// ============================================

bool CSSAOPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[SSAOPass] Initializing...");

    createShaders();
    createSamplers();
    createNoiseTexture();
    createWhiteFallbackTexture();
    initDescriptorSets();

    m_initialized = true;
    CFFLog::Info("[SSAOPass] Initialized successfully");
    return true;
}

void CSSAOPass::Shutdown() {
    m_ssaoCS.reset();
    m_blurHCS.reset();
    m_blurVCS.reset();
    m_upsampleCS.reset();
    m_downsampleCS.reset();

    m_ssaoPSO.reset();
    m_blurHPSO.reset();
    m_blurVPSO.reset();
    m_upsamplePSO.reset();
    m_downsamplePSO.reset();

    m_ssaoRaw.reset();
    m_ssaoBlurTemp.reset();
    m_ssaoHalfBlurred.reset();
    m_depthHalfRes.reset();
    m_ssaoFinal.reset();

    m_noiseTexture.reset();
    m_pointSampler.reset();
    m_linearSampler.reset();

    // Cleanup DS resources
    m_ssaoCS_ds.reset();
    m_blurHCS_ds.reset();
    m_blurVCS_ds.reset();
    m_upsampleCS_ds.reset();
    m_downsampleCS_ds.reset();

    m_ssaoPSO_ds.reset();
    m_blurHPSO_ds.reset();
    m_blurVPSO_ds.reset();
    m_upsamplePSO_ds.reset();
    m_downsamplePSO_ds.reset();

    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perPassSet) {
            ctx->FreeDescriptorSet(m_perPassSet);
            m_perPassSet = nullptr;
        }
        if (m_computePerPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_computePerPassLayout);
            m_computePerPassLayout = nullptr;
        }
    }

    m_fullWidth = 0;
    m_fullHeight = 0;
    m_halfWidth = 0;
    m_halfHeight = 0;
    m_initialized = false;

    CFFLog::Info("[SSAOPass] Shutdown");
}

void CSSAOPass::Resize(uint32_t width, uint32_t height) {
    if (width == m_fullWidth && height == m_fullHeight) {
        return;
    }
    createTextures(width, height);
}

// ============================================
// Rendering
// ============================================

void CSSAOPass::Render(ICommandList* cmdList,
                        ITexture* depthBuffer,
                        ITexture* normalBuffer,
                        uint32_t width, uint32_t height,
                        const XMMATRIX& view,
                        const XMMATRIX& proj,
                        float nearZ, float farZ) {
    if (!m_initialized || !cmdList) return;

    // Ensure textures are properly sized
    if (width != m_fullWidth || height != m_fullHeight) {
        createTextures(width, height);
    }

    // Guard against invalid state
    if (!m_ssaoRaw || !depthBuffer || !normalBuffer) return;

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        // Pipeline: Downsample -> SSAO -> BlurH -> BlurV -> Upsample (Descriptor Set path)
        {
            CScopedDebugEvent evt(cmdList, L"SSAO Depth Downsample (DS)");
            dispatchDownsampleDepth_DS(cmdList, depthBuffer);
        }
        {
            CScopedDebugEvent evt(cmdList, L"SSAO GTAO Compute (DS)");
            dispatchSSAO_DS(cmdList, depthBuffer, normalBuffer, view, proj, nearZ, farZ);
        }
        {
            CScopedDebugEvent evt(cmdList, L"SSAO Blur H (DS)");
            dispatchBlurH_DS(cmdList);
        }
        {
            CScopedDebugEvent evt(cmdList, L"SSAO Blur V (DS)");
            dispatchBlurV_DS(cmdList);
        }
        {
            CScopedDebugEvent evt(cmdList, L"SSAO Upsample (DS)");
            dispatchUpsample_DS(cmdList, depthBuffer);
        }
    }

    // Transition SSAO output from UAV to SRV for consumers (deferred lighting)
    cmdList->Barrier(m_ssaoFinal.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

// ============================================
// Shader Creation
// ============================================

void CSSAOPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    constexpr bool kDebugShaders = true;
#else
    constexpr bool kDebugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSAO.cs.hlsl";

    // Create all compute shaders and their PSOs
    struct ShaderDef {
        const char* entryPoint;
        const char* shaderName;
        const char* psoName;
        ShaderPtr* shader;
        PipelineStatePtr* pso;
    };

    ShaderDef shaders[] = {
        {"CSMain",              "SSAO_CSMain",              "SSAO_Main_PSO",              &m_ssaoCS,       &m_ssaoPSO},
        {"CSBlurH",             "SSAO_CSBlurH",             "SSAO_BlurH_PSO",             &m_blurHCS,      &m_blurHPSO},
        {"CSBlurV",             "SSAO_CSBlurV",             "SSAO_BlurV_PSO",             &m_blurVCS,      &m_blurVPSO},
        {"CSBilateralUpsample", "SSAO_CSBilateralUpsample", "SSAO_BilateralUpsample_PSO", &m_upsampleCS,   &m_upsamplePSO},
        {"CSDownsampleDepth",   "SSAO_CSDownsampleDepth",   "SSAO_DepthDownsample_PSO",   &m_downsampleCS, &m_downsamplePSO},
    };

    for (const auto& def : shaders) {
        if (!createComputeShaderAndPSO(ctx, shaderPath, def.entryPoint, def.shaderName, def.psoName,
                                        kDebugShaders, *def.shader, *def.pso)) {
            return;
        }
    }

    CFFLog::Info("[SSAOPass] Compute shaders and PSOs created");
}

// ============================================
// Texture Creation
// ============================================

void CSSAOPass::createTextures(uint32_t fullWidth, uint32_t fullHeight) {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    m_fullWidth = fullWidth;
    m_fullHeight = fullHeight;
    m_halfWidth = (fullWidth + 1) / 2;
    m_halfHeight = (fullHeight + 1) / 2;

    // Half-res textures (R8_UNORM for AO values)
    m_ssaoRaw = createHalfResTexture(ctx, m_halfWidth, m_halfHeight, ETextureFormat::R8_UNORM, "SSAO_Raw");
    m_ssaoBlurTemp = createHalfResTexture(ctx, m_halfWidth, m_halfHeight, ETextureFormat::R8_UNORM, "SSAO_BlurTemp");
    m_ssaoHalfBlurred = createHalfResTexture(ctx, m_halfWidth, m_halfHeight, ETextureFormat::R8_UNORM, "SSAO_HalfBlurred");
    m_depthHalfRes = createHalfResTexture(ctx, m_halfWidth, m_halfHeight, ETextureFormat::R32_FLOAT, "SSAO_DepthHalfRes");

    // Full-res final output
    TextureDesc finalDesc;
    finalDesc.width = fullWidth;
    finalDesc.height = fullHeight;
    finalDesc.format = ETextureFormat::R8_UNORM;
    finalDesc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
    finalDesc.clearColor[0] = 1.0f;
    finalDesc.debugName = "SSAO_Final";
    m_ssaoFinal.reset(ctx->CreateTexture(finalDesc, nullptr));

    CFFLog::Info("[SSAOPass] Textures resized: Full=%ux%u, Half=%ux%u",
                 fullWidth, fullHeight, m_halfWidth, m_halfHeight);
}

// ============================================
// Noise Texture
// ============================================

void CSSAOPass::createNoiseTexture() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    constexpr uint32_t kNoiseSize = SSAOConfig::NOISE_TEXTURE_SIZE;
    constexpr float kPi = 3.14159265f;
    std::vector<uint8_t> noiseData(kNoiseSize * kNoiseSize * 4);

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (uint32_t i = 0; i < kNoiseSize * kNoiseSize; ++i) {
        float angle = dist(rng) * 2.0f * kPi;
        // Store cos/sin remapped from [-1,1] to [0,255]
        noiseData[i * 4 + 0] = static_cast<uint8_t>((std::cos(angle) * 0.5f + 0.5f) * 255.0f);
        noiseData[i * 4 + 1] = static_cast<uint8_t>((std::sin(angle) * 0.5f + 0.5f) * 255.0f);
        noiseData[i * 4 + 2] = 128;  // Unused
        noiseData[i * 4 + 3] = 255;  // Unused
    }

    TextureDesc desc;
    desc.width = kNoiseSize;
    desc.height = kNoiseSize;
    desc.format = ETextureFormat::R8G8B8A8_UNORM;
    desc.usage = ETextureUsage::ShaderResource;
    desc.debugName = "SSAO_Noise";
    m_noiseTexture.reset(ctx->CreateTexture(desc, noiseData.data()));

    CFFLog::Info("[SSAOPass] Noise texture created (%ux%u)", kNoiseSize, kNoiseSize);
}

// ============================================
// White Fallback Texture
// ============================================

void CSSAOPass::createWhiteFallbackTexture() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    uint8_t whitePixel = 255;

    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = ETextureFormat::R8_UNORM;
    desc.usage = ETextureUsage::ShaderResource;
    desc.debugName = "SSAO_WhiteFallback";
    m_whiteFallback.reset(ctx->CreateTexture(desc, &whitePixel));

    CFFLog::Info("[SSAOPass] White fallback texture created (1x1)");
}

// ============================================
// Samplers
// ============================================

void CSSAOPass::createSamplers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    auto createClampSampler = [ctx](EFilter filter) {
        SamplerDesc desc;
        desc.filter = filter;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        return SamplerPtr(ctx->CreateSampler(desc));
    };

    m_pointSampler = createClampSampler(EFilter::MinMagMipPoint);
    m_linearSampler = createClampSampler(EFilter::MinMagMipLinear);
}

// ============================================
// Dispatch Helpers (Descriptor Set Binding)
// ============================================

void CSSAOPass::dispatchDownsampleDepth_DS(ICommandList* cmdList, ITexture* depthFullRes) {
    if (!m_downsamplePSO_ds || !m_depthHalfRes || !m_perPassSet) return;

    struct CB_Downsample {
        float texelSizeX, texelSizeY;
        uint32_t useReversedZ;
        float _pad;
    } cb;

    cb.texelSizeX = 1.0f / static_cast<float>(m_fullWidth);
    cb.texelSizeY = 1.0f / static_cast<float>(m_fullHeight);
    cb.useReversedZ = UseReversedZ() ? 1 : 0;

    // Bind resources to descriptor set
    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cb, sizeof(cb)),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input0, depthFullRes),
        BindingSetItem::Texture_UAV(ComputePassLayout::Slots::UAV_Output0, m_depthHalfRes.get())
    });

    cmdList->SetPipelineState(m_downsamplePSO_ds.get());
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Dispatch(calcDispatchGroups(m_halfWidth), calcDispatchGroups(m_halfHeight), 1);

    // UAV barrier for next pass
    cmdList->Barrier(m_depthHalfRes.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

void CSSAOPass::dispatchSSAO_DS(ICommandList* cmdList,
                                 ITexture* depthBuffer,
                                 ITexture* normalBuffer,
                                 const XMMATRIX& view,
                                 const XMMATRIX& proj,
                                 float /*nearZ*/, float /*farZ*/) {
    if (!m_ssaoPSO_ds || !m_ssaoRaw || !m_perPassSet) return;

    CB_SSAO cb{};
    XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(view));

    cb.texelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.texelSize.y = 1.0f / static_cast<float>(m_halfHeight);
    cb.noiseScale.x = static_cast<float>(m_halfWidth) / static_cast<float>(SSAOConfig::NOISE_TEXTURE_SIZE);
    cb.noiseScale.y = static_cast<float>(m_halfHeight) / static_cast<float>(SSAOConfig::NOISE_TEXTURE_SIZE);
    cb.radius = m_settings.radius;
    cb.intensity = m_settings.intensity;
    cb.falloffStart = m_settings.falloffStart;
    cb.falloffEnd = m_settings.falloffEnd;
    cb.numSlices = m_settings.numSlices;
    cb.numSteps = m_settings.numSteps;
    cb.thicknessHeuristic = m_settings.thicknessHeuristic;
    cb.algorithm = static_cast<int>(m_settings.algorithm);
    cb.useReversedZ = UseReversedZ() ? 1 : 0;

    // Bind resources to descriptor set
    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cb, sizeof(cb)),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input0, m_depthHalfRes.get()),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input1, normalBuffer),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input2, m_noiseTexture.get()),
        BindingSetItem::Texture_UAV(ComputePassLayout::Slots::UAV_Output0, m_ssaoRaw.get())
    });

    cmdList->SetPipelineState(m_ssaoPSO_ds.get());
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Dispatch(calcDispatchGroups(m_halfWidth), calcDispatchGroups(m_halfHeight), 1);

    // UAV barrier for next pass
    cmdList->Barrier(m_ssaoRaw.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

void CSSAOPass::dispatchBlurH_DS(ICommandList* cmdList) {
    dispatchBlur_DS(cmdList, m_blurHPSO_ds.get(), m_ssaoRaw.get(), m_ssaoBlurTemp.get(), XMFLOAT2(1.0f, 0.0f));
}

void CSSAOPass::dispatchBlurV_DS(ICommandList* cmdList) {
    dispatchBlur_DS(cmdList, m_blurVPSO_ds.get(), m_ssaoBlurTemp.get(), m_ssaoHalfBlurred.get(), XMFLOAT2(0.0f, 1.0f));
}

void CSSAOPass::dispatchBlur_DS(ICommandList* cmdList,
                                 IPipelineState* pso,
                                 ITexture* inputAO,
                                 ITexture* outputAO,
                                 const XMFLOAT2& direction) {
    if (!pso || !outputAO || !m_perPassSet) return;

    CB_SSAOBlur cb{};
    cb.blurDirection = direction;
    cb.texelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.texelSize.y = 1.0f / static_cast<float>(m_halfHeight);
    cb.depthSigma = m_settings.depthSigma;
    cb.blurRadius = m_settings.blurRadius;

    // Bind resources to descriptor set
    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cb, sizeof(cb)),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input0, inputAO),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input1, m_depthHalfRes.get()),
        BindingSetItem::Texture_UAV(ComputePassLayout::Slots::UAV_Output0, outputAO)
    });

    cmdList->SetPipelineState(pso);
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Dispatch(calcDispatchGroups(m_halfWidth), calcDispatchGroups(m_halfHeight), 1);

    // UAV barrier for next pass
    cmdList->Barrier(outputAO, EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

void CSSAOPass::dispatchUpsample_DS(ICommandList* cmdList, ITexture* depthFullRes) {
    if (!m_upsamplePSO_ds || !m_ssaoFinal || !m_perPassSet) return;

    CB_SSAOUpsample cb{};
    cb.fullResTexelSize.x = 1.0f / static_cast<float>(m_fullWidth);
    cb.fullResTexelSize.y = 1.0f / static_cast<float>(m_fullHeight);
    cb.halfResTexelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.halfResTexelSize.y = 1.0f / static_cast<float>(m_halfHeight);
    cb.depthSigma = m_settings.depthSigma;

    // Bind resources to descriptor set
    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cb, sizeof(cb)),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input0, m_ssaoHalfBlurred.get()),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input1, m_depthHalfRes.get()),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input2, depthFullRes),
        BindingSetItem::Texture_UAV(ComputePassLayout::Slots::UAV_Output0, m_ssaoFinal.get())
    });

    cmdList->SetPipelineState(m_upsamplePSO_ds.get());
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Dispatch(calcDispatchGroups(m_fullWidth), calcDispatchGroups(m_fullHeight), 1);

    // Note: Final UAV->SRV barrier is done in Render() after all dispatches
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CSSAOPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[SSAOPass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSAO_DS.cs.hlsl";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Create unified compute layout
    m_computePerPassLayout = ComputePassLayout::CreateComputePerPassLayout(ctx);
    if (!m_computePerPassLayout) {
        CFFLog::Error("[SSAOPass] Failed to create compute PerPass layout");
        return;
    }

    // Allocate descriptor set
    m_perPassSet = ctx->AllocateDescriptorSet(m_computePerPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[SSAOPass] Failed to allocate PerPass descriptor set");
        return;
    }

    // Bind static samplers
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Point, m_pointSampler.get()));
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Linear, m_linearSampler.get()));

    // Compile SM 5.1 shaders
    struct ShaderDef {
        const char* entryPoint;
        const char* shaderName;
        const char* psoName;
        ShaderPtr* shader;
        PipelineStatePtr* pso;
    };

    ShaderDef shaders[] = {
        {"CSMain",              "SSAO_DS_CSMain",              "SSAO_DS_Main_PSO",              &m_ssaoCS_ds,       &m_ssaoPSO_ds},
        {"CSBlurH",             "SSAO_DS_CSBlurH",             "SSAO_DS_BlurH_PSO",             &m_blurHCS_ds,      &m_blurHPSO_ds},
        {"CSBlurV",             "SSAO_DS_CSBlurV",             "SSAO_DS_BlurV_PSO",             &m_blurVCS_ds,      &m_blurVPSO_ds},
        {"CSBilateralUpsample", "SSAO_DS_CSBilateralUpsample", "SSAO_DS_BilateralUpsample_PSO", &m_upsampleCS_ds,   &m_upsamplePSO_ds},
        {"CSDownsampleDepth",   "SSAO_DS_CSDownsampleDepth",   "SSAO_DS_DepthDownsample_PSO",   &m_downsampleCS_ds, &m_downsamplePSO_ds},
    };

    for (const auto& def : shaders) {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, def.entryPoint, "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSAOPass] %s (SM 5.1) compilation failed: %s", def.entryPoint, compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = def.shaderName;
        def.shader->reset(ctx->CreateShader(shaderDesc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = def.shader->get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = def.psoName;
        def.pso->reset(ctx->CreateComputePipelineState(psoDesc));
    }

    CFFLog::Info("[SSAOPass] Descriptor set resources initialized");
}
