#include "SSRPass.h"
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
#include <algorithm>
#include <vector>
#include <cmath>

using namespace DirectX;
using namespace RHI;

// ============================================
// Internal Helpers
// ============================================

namespace {

// Compile a compute shader and create its PSO
// Returns true on success, false on failure
bool CreateComputeShaderAndPSO(const std::string& shaderPath,
                                const char* entryPoint,
                                const char* shaderDebugName,
                                const char* psoDebugName,
                                ShaderPtr& outShader,
                                PipelineStatePtr& outPSO)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader compiled = CompileShaderFromFile(shaderPath, entryPoint, "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[SSRPass] Shader compilation failed (%s): %s", shaderDebugName, compiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    shaderDesc.debugName = shaderDebugName;
    outShader.reset(ctx->CreateShader(shaderDesc));

    if (!outShader) {
        CFFLog::Error("[SSRPass] Failed to create shader: %s", shaderDebugName);
        return false;
    }

    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = outShader.get();
    psoDesc.debugName = psoDebugName;
    outPSO.reset(ctx->CreateComputePipelineState(psoDesc));

    if (!outPSO) {
        CFFLog::Error("[SSRPass] Failed to create PSO: %s", psoDebugName);
        return false;
    }

    return true;
}

} // anonymous namespace

// ============================================
// Lifecycle
// ============================================

bool CSSRPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[SSRPass] Initializing...");

#ifndef FF_LEGACY_BINDING_DISABLED
    createShaders();
    createCompositeShader();
#endif
    createSamplers();
    createFallbackTexture();
    createBlueNoiseTexture();
    initDescriptorSets();

    m_initialized = true;
    CFFLog::Info("[SSRPass] Initialized successfully");
    return true;
}

void CSSRPass::Shutdown() {
#ifndef FF_LEGACY_BINDING_DISABLED
    m_ssrCS.reset();
    m_ssrPSO.reset();
    m_compositeCS.reset();
    m_compositePSO.reset();
#endif

    m_ssrResult.reset();
    m_ssrHistory.reset();
    m_blueNoise.reset();
    m_blackFallback.reset();

    m_pointSampler.reset();
    m_linearSampler.reset();

    // Cleanup DS resources
    m_ssrCS_ds.reset();
    m_compositeCS_ds.reset();
    m_ssrPSO_ds.reset();
    m_compositePSO_ds.reset();

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

    m_width = 0;
    m_height = 0;
    m_currentScale = 1.0f;
    m_initialized = false;
    m_frameIndex = 0;
    m_prevViewProj = XMMatrixIdentity();

    CFFLog::Info("[SSRPass] Shutdown");
}

// ============================================
// Rendering
// ============================================

void CSSRPass::Render(ICommandList* cmdList,
                       ITexture* depthBuffer,
                       ITexture* normalBuffer,
                       ITexture* hiZTexture,
                       ITexture* sceneColor,
                       uint32_t width, uint32_t height,
                       uint32_t hiZMipCount,
                       const XMMATRIX& view,
                       const XMMATRIX& proj,
                       float nearZ, float farZ) {
    if (!m_initialized || !cmdList) return;

    // Validate inputs
    if (!depthBuffer || !normalBuffer || !hiZTexture || !sceneColor) {
        CFFLog::Warning("[SSRPass] Missing required input textures");
        return;
    }

    // Ensure textures are properly sized (check both resolution and scale changes)
    float scale = std::clamp(m_settings.resolutionScale, 0.25f, 1.0f);
    if (width != m_width || height != m_height || scale != m_currentScale) {
        m_currentScale = scale;
        createTextures(width, height);
    }

    // Guard against invalid state
    if (!m_ssrResult) {
        return;
    }

    // Calculate inverse matrices
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMMATRIX invView = XMMatrixInverse(nullptr, view);

    // Calculate scaled resolution for SSR (use scale from earlier check)
    uint32_t scaledWidth = std::max(1u, static_cast<uint32_t>(width * m_currentScale));
    uint32_t scaledHeight = std::max(1u, static_cast<uint32_t>(height * m_currentScale));

    // Fill constant buffer
    CB_SSR cb;
    XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.invView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&cb.prevViewProj, XMMatrixTranspose(m_prevViewProj));

    cb.screenSize = XMFLOAT2(static_cast<float>(scaledWidth), static_cast<float>(scaledHeight));
    cb.texelSize = XMFLOAT2(1.0f / scaledWidth, 1.0f / scaledHeight);
    cb.maxDistance = m_settings.maxDistance;
    cb.thickness = m_settings.thickness;
    cb.stride = m_settings.stride;
    cb.strideZCutoff = m_settings.strideZCutoff;
    cb.maxSteps = m_settings.maxSteps;
    cb.binarySearchSteps = m_settings.binarySearchSteps;
    cb.jitterOffset = m_settings.jitterOffset;
    cb.fadeStart = m_settings.fadeStart;
    cb.fadeEnd = m_settings.fadeEnd;
    cb.roughnessFade = m_settings.roughnessFade;
    cb.nearZ = nearZ;
    cb.farZ = farZ;
    cb.hiZMipCount = static_cast<int>(hiZMipCount);
    cb.useReversedZ = 1;  // Always use reversed-Z (project default)
    cb.ssrMode = static_cast<int>(m_settings.mode);
    cb.numRays = m_settings.numRays;
    cb.brdfBias = m_settings.brdfBias;
    cb.temporalBlend = m_settings.temporalBlend;
    cb.motionThreshold = m_settings.motionThreshold;
    cb.frameIndex = m_frameIndex++;
    // Stochastic SSR improvements
    cb.useAdaptiveRays = m_settings.useAdaptiveRays ? 1 : 0;
    cb.fireflyClampThreshold = m_settings.fireflyClampThreshold;
    cb.fireflyMultiplier = m_settings.fireflyMultiplier;
    cb._pad = 0.0f;

    // Store current view-proj for next frame
    m_prevViewProj = XMMatrixMultiply(view, proj);

    // Dispatch compute shader at scaled resolution
    uint32_t groupsX = (scaledWidth + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (scaledHeight + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;

    // Transition SSR result to UAV state
    cmdList->Barrier(m_ssrResult.get(), EResourceState::ShaderResource, EResourceState::UnorderedAccess);

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        cmdList->SetPipelineState(m_ssrPSO_ds.get());

        // Bind PerPass descriptor set
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(CB_SSR)));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, depthBuffer));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(1, normalBuffer));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(2, hiZTexture));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(3, sceneColor));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(4, m_blueNoise.get()));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(5, m_ssrHistory.get()));
        m_perPassSet->Bind(BindingSetItem::Texture_UAV(0, m_ssrResult.get()));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        cmdList->Dispatch(groupsX, groupsY, 1);
    } else {
#ifndef FF_LEGACY_BINDING_DISABLED
        // Legacy path for DX11
        if (!m_ssrPSO) {
            cmdList->Barrier(m_ssrResult.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
            return;
        }

        cmdList->SetPipelineState(m_ssrPSO.get());

        // Bind resources
        cmdList->SetShaderResource(EShaderStage::Compute, 0, depthBuffer);
        cmdList->SetShaderResource(EShaderStage::Compute, 1, normalBuffer);
        cmdList->SetShaderResource(EShaderStage::Compute, 2, hiZTexture);
        cmdList->SetShaderResource(EShaderStage::Compute, 3, sceneColor);
        cmdList->SetShaderResource(EShaderStage::Compute, 4, m_blueNoise.get());
        cmdList->SetShaderResource(EShaderStage::Compute, 5, m_ssrHistory.get());

        cmdList->SetUnorderedAccessTexture(0, m_ssrResult.get());

        cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());
        cmdList->SetSampler(EShaderStage::Compute, 1, m_linearSampler.get());

        cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

        cmdList->Dispatch(groupsX, groupsY, 1);
#else
        CFFLog::Warning("[SSRPass] Legacy binding disabled but descriptor set mode not available");
#endif
    }

    // Transition SSR result back to SRV state
    cmdList->Barrier(m_ssrResult.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

// ============================================
// Shader Creation (Legacy SM 5.0)
// ============================================

#ifndef FF_LEGACY_BINDING_DISABLED
void CSSRPass::createShaders() {
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSR.cs.hlsl";

    if (CreateComputeShaderAndPSO(shaderPath, "CSMain", "SSR_CS", "SSR_PSO", m_ssrCS, m_ssrPSO)) {
        CFFLog::Info("[SSRPass] SSR shader compiled successfully");
    }
}

void CSSRPass::createCompositeShader() {
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSRComposite.cs.hlsl";

    if (CreateComputeShaderAndPSO(shaderPath, "CSMain", "SSRComposite_CS", "SSRComposite_PSO", m_compositeCS, m_compositePSO)) {
        CFFLog::Info("[SSRPass] Composite shader compiled successfully");
    }
}
#endif

void CSSRPass::createTextures(uint32_t width, uint32_t height) {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    if (width == 0 || height == 0) return;

    m_width = width;
    m_height = height;

    // Apply resolution scale
    float scale = std::clamp(m_settings.resolutionScale, 0.25f, 1.0f);
    uint32_t scaledWidth = std::max(1u, static_cast<uint32_t>(width * scale));
    uint32_t scaledHeight = std::max(1u, static_cast<uint32_t>(height * scale));

    // Create SSR result texture
    // R16G16B16A16_FLOAT: rgb = reflection color, a = confidence
    TextureDesc desc;
    desc.width = scaledWidth;
    desc.height = scaledHeight;
    desc.format = ETextureFormat::R16G16B16A16_FLOAT;
    desc.mipLevels = 1;
    desc.usage = ETextureUsage::ShaderResource | ETextureUsage::UnorderedAccess;
    desc.dimension = ETextureDimension::Tex2D;
    desc.debugName = "SSR_Result";

    m_ssrResult.reset(ctx->CreateTexture(desc, nullptr));

    if (!m_ssrResult) {
        CFFLog::Error("[SSRPass] Failed to create SSR result texture");
        return;
    }

    // Create SSR history texture for temporal accumulation
    desc.debugName = "SSR_History";
    m_ssrHistory.reset(ctx->CreateTexture(desc, nullptr));

    if (!m_ssrHistory) {
        CFFLog::Warning("[SSRPass] Failed to create SSR history texture (temporal disabled)");
    }

    CFFLog::Info("[SSRPass] Created SSR textures: %ux%u (scale: %.2f)", scaledWidth, scaledHeight, scale);
}

void CSSRPass::createSamplers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Common address mode for both samplers
    auto setClampAddressMode = [](SamplerDesc& desc) {
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
    };

    // Point sampler for depth/Hi-Z
    SamplerDesc pointDesc;
    pointDesc.filter = EFilter::MinMagMipPoint;
    setClampAddressMode(pointDesc);
    m_pointSampler.reset(ctx->CreateSampler(pointDesc));

    // Linear sampler for color
    SamplerDesc linearDesc;
    linearDesc.filter = EFilter::MinMagMipLinear;
    setClampAddressMode(linearDesc);
    m_linearSampler.reset(ctx->CreateSampler(linearDesc));
}

void CSSRPass::createFallbackTexture() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Create 1x1 black texture as fallback
    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = ETextureFormat::R16G16B16A16_FLOAT;
    desc.mipLevels = 1;
    desc.usage = ETextureUsage::ShaderResource;
    desc.dimension = ETextureDimension::Tex2D;
    desc.debugName = "SSR_BlackFallback";

    // Black with 0 alpha (no reflection, 0 confidence)
    uint16_t blackData[4] = { 0, 0, 0, 0 };
    m_blackFallback.reset(ctx->CreateTexture(desc, blackData));

    if (!m_blackFallback) {
        CFFLog::Warning("[SSRPass] Failed to create black fallback texture");
    }
}

void CSSRPass::Composite(ICommandList* cmdList,
                          ITexture* hdrBuffer,
                          ITexture* worldPosMetallic,
                          ITexture* normalRoughness,
                          uint32_t width, uint32_t height,
                          const XMFLOAT3& camPosWS) {
    if (!m_initialized || !cmdList) return;

    // Validate inputs
    if (!hdrBuffer || !m_ssrResult || !worldPosMetallic || !normalRoughness) {
        return;
    }

    // Fill constant buffer
    CB_SSRComposite cb;
    cb.screenSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
    cb.texelSize = XMFLOAT2(1.0f / width, 1.0f / height);
    cb.ssrIntensity = m_settings.intensity;
    cb.iblFallbackWeight = 1.0f;  // Keep full IBL when SSR misses
    cb.roughnessFade = m_settings.roughnessFade;
    cb._pad0 = 0.0f;
    cb.camPosWS = camPosWS;
    cb._pad1 = 0.0f;

    // Dispatch compute shader
    uint32_t groupsX = (width + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (height + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable() && m_compositePSO_ds) {
        cmdList->SetPipelineState(m_compositePSO_ds.get());

        // Bind PerPass descriptor set
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(CB_SSRComposite)));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, hdrBuffer));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(1, m_ssrResult.get()));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(2, worldPosMetallic));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(3, normalRoughness));
        m_perPassSet->Bind(BindingSetItem::Texture_UAV(0, hdrBuffer));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        cmdList->Dispatch(groupsX, groupsY, 1);
    } else {
#ifndef FF_LEGACY_BINDING_DISABLED
        // Legacy path for DX11
        if (!m_compositePSO) {
            return;
        }

        // Transition HDR buffer to UAV state for read-modify-write
        // cmdList->Barrier(hdrBuffer, EResourceState::RenderTarget, EResourceState::UnorderedAccess);

        // Set PSO
        cmdList->SetPipelineState(m_compositePSO.get());

        // Bind resources
        cmdList->SetShaderResource(EShaderStage::Compute, 0, hdrBuffer);  // Will be bound as UAV for output
        cmdList->SetShaderResource(EShaderStage::Compute, 1, m_ssrResult.get());
        cmdList->SetShaderResource(EShaderStage::Compute, 2, worldPosMetallic);
        cmdList->SetShaderResource(EShaderStage::Compute, 3, normalRoughness);

        cmdList->SetUnorderedAccessTexture(0, hdrBuffer);

        cmdList->SetSampler(EShaderStage::Compute, 0, m_linearSampler.get());
        cmdList->SetSampler(EShaderStage::Compute, 1, m_pointSampler.get());

        cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

        cmdList->Dispatch(groupsX, groupsY, 1);
#else
        CFFLog::Warning("[SSRPass] Legacy binding disabled but descriptor set mode not available for Composite");
#endif
    }

    // Transition HDR buffer back to RTV/SRV state
    cmdList->Barrier(hdrBuffer, EResourceState::UnorderedAccess, EResourceState::RenderTarget);
}

void CSSRPass::createBlueNoiseTexture() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Generate 64x64 blue noise texture using procedural LDS (Low Discrepancy Sequence)
    // This is a simplified approach; production would use precomputed blue noise
    constexpr uint32_t NOISE_SIZE = 64;
    std::vector<uint8_t> noiseData(NOISE_SIZE * NOISE_SIZE * 4);  // RGBA8

    // Use R2 sequence for low-discrepancy 2D sampling
    // R2 is based on generalized golden ratio: alpha = 0.7548776662...
    const float g = 1.32471795724f;  // Plastic constant
    const float a1 = 1.0f / g;
    const float a2 = 1.0f / (g * g);

    for (uint32_t y = 0; y < NOISE_SIZE; ++y) {
        for (uint32_t x = 0; x < NOISE_SIZE; ++x) {
            uint32_t idx = (y * NOISE_SIZE + x) * 4;
            uint32_t n = y * NOISE_SIZE + x;

            // R2 sequence
            float r1 = fmodf(0.5f + a1 * n, 1.0f);
            float r2 = fmodf(0.5f + a2 * n, 1.0f);

            // Additional randomness using simple hash
            uint32_t hash = n * 747796405u + 2891336453u;
            hash = ((hash >> ((hash >> 28) + 4)) ^ hash) * 277803737u;
            float r3 = (hash & 0xFFFFu) / 65535.0f;
            float r4 = ((hash >> 16) & 0xFFFFu) / 65535.0f;

            noiseData[idx + 0] = static_cast<uint8_t>(r1 * 255.0f);
            noiseData[idx + 1] = static_cast<uint8_t>(r2 * 255.0f);
            noiseData[idx + 2] = static_cast<uint8_t>(r3 * 255.0f);
            noiseData[idx + 3] = static_cast<uint8_t>(r4 * 255.0f);
        }
    }

    TextureDesc desc;
    desc.width = NOISE_SIZE;
    desc.height = NOISE_SIZE;
    desc.format = ETextureFormat::R8G8B8A8_UNORM;
    desc.mipLevels = 1;
    desc.usage = ETextureUsage::ShaderResource;
    desc.dimension = ETextureDimension::Tex2D;
    desc.debugName = "SSR_BlueNoise";

    m_blueNoise.reset(ctx->CreateTexture(desc, noiseData.data()));

    if (!m_blueNoise) {
        CFFLog::Warning("[SSRPass] Failed to create blue noise texture (stochastic mode may have artifacts)");
    } else {
        CFFLog::Info("[SSRPass] Blue noise texture created (64x64)");
    }
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CSSRPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[SSRPass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSR_DS.cs.hlsl";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Create unified compute layout
    m_computePerPassLayout = ComputePassLayout::CreateComputePerPassLayout(ctx);
    if (!m_computePerPassLayout) {
        CFFLog::Error("[SSRPass] Failed to create compute PerPass layout");
        return;
    }

    // Allocate descriptor set
    m_perPassSet = ctx->AllocateDescriptorSet(m_computePerPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[SSRPass] Failed to allocate PerPass descriptor set");
        return;
    }

    // Bind static samplers
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Point, m_pointSampler.get()));
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Linear, m_linearSampler.get()));

    // Compile SM 5.1 SSR shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSMain", "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSRPass] CSMain (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = "SSR_DS_CS";
        m_ssrCS_ds.reset(ctx->CreateShader(shaderDesc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_ssrCS_ds.get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = "SSR_DS_PSO";
        m_ssrPSO_ds.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Compile SM 5.1 Composite shader
    {
        std::string compositePath = FFPath::GetSourceDir() + "/Shader/SSRComposite_DS.cs.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(compositePath, "CSMain", "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Warning("[SSRPass] Composite (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            // Composite is optional, continue without it
        } else {
            ShaderDesc shaderDesc;
            shaderDesc.type = EShaderType::Compute;
            shaderDesc.bytecode = compiled.bytecode.data();
            shaderDesc.bytecodeSize = compiled.bytecode.size();
            shaderDesc.debugName = "SSRComposite_DS_CS";
            m_compositeCS_ds.reset(ctx->CreateShader(shaderDesc));

            ComputePipelineDesc psoDesc;
            psoDesc.computeShader = m_compositeCS_ds.get();
            psoDesc.setLayouts[1] = m_computePerPassLayout;
            psoDesc.debugName = "SSRComposite_DS_PSO";
            m_compositePSO_ds.reset(ctx->CreateComputePipelineState(psoDesc));
        }
    }

    CFFLog::Info("[SSRPass] Descriptor set resources initialized");
}
