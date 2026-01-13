#include "SSRPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
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
// Lifecycle
// ============================================

bool CSSRPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[SSRPass] Initializing...");

    createShaders();
    createCompositeShader();
    createSamplers();
    createFallbackTexture();
    createBlueNoiseTexture();

    m_initialized = true;
    CFFLog::Info("[SSRPass] Initialized successfully");
    return true;
}

void CSSRPass::Shutdown() {
    m_ssrCS.reset();
    m_ssrPSO.reset();
    m_compositeCS.reset();
    m_compositePSO.reset();

    m_ssrResult.reset();
    m_ssrHistory.reset();
    m_blueNoise.reset();
    m_blackFallback.reset();

    m_pointSampler.reset();
    m_linearSampler.reset();

    m_width = 0;
    m_height = 0;
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

    // Check if SSR is enabled
    if (!m_settings.enabled) {
        return;
    }

    // Validate inputs
    if (!depthBuffer || !normalBuffer || !hiZTexture || !sceneColor) {
        CFFLog::Warning("[SSRPass] Missing required input textures");
        return;
    }

    // Ensure textures are properly sized
    if (width != m_width || height != m_height) {
        createTextures(width, height);
    }

    // Guard against invalid state
    if (!m_ssrPSO || !m_ssrResult) {
        return;
    }

    // Transition SSR result to UAV state
    cmdList->Barrier(m_ssrResult.get(), EResourceState::ShaderResource, EResourceState::UnorderedAccess);

    // Set PSO
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

    // Calculate inverse matrices
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMMATRIX invView = XMMatrixInverse(nullptr, view);

    // Fill constant buffer
    CB_SSR cb;
    XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.invView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&cb.prevViewProj, XMMatrixTranspose(m_prevViewProj));
    cb.screenSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
    cb.texelSize = XMFLOAT2(1.0f / width, 1.0f / height);
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
    cb._pad[0] = cb._pad[1] = 0.0f;

    // Store current view-proj for next frame
    m_prevViewProj = XMMatrixMultiply(view, proj);

    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

    // Dispatch compute shader
    uint32_t groupsX = (width + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (height + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    // Transition SSR result back to SRV state
    cmdList->Barrier(m_ssrResult.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

// ============================================
// Shader Creation
// ============================================

void CSSRPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSR.cs.hlsl";

    // Compile SSR compute shader
    SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSMain", "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[SSRPass] SSR shader compilation failed: %s", compiled.errorMessage.c_str());
        return;
    }

    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    shaderDesc.debugName = "SSR_CS";
    m_ssrCS.reset(ctx->CreateShader(shaderDesc));

    if (!m_ssrCS) {
        CFFLog::Error("[SSRPass] Failed to create SSR shader");
        return;
    }

    // Create PSO
    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = m_ssrCS.get();
    psoDesc.debugName = "SSR_PSO";
    m_ssrPSO.reset(ctx->CreateComputePipelineState(psoDesc));

    if (!m_ssrPSO) {
        CFFLog::Error("[SSRPass] Failed to create SSR PSO");
        return;
    }

    CFFLog::Info("[SSRPass] Shaders compiled successfully");
}

void CSSRPass::createTextures(uint32_t width, uint32_t height) {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    if (width == 0 || height == 0) return;

    m_width = width;
    m_height = height;

    // Create SSR result texture
    // R16G16B16A16_FLOAT: rgb = reflection color, a = confidence
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
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

    CFFLog::Info("[SSRPass] Created SSR textures: %ux%u", width, height);
}

void CSSRPass::createSamplers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Point sampler for depth/Hi-Z
    SamplerDesc pointDesc;
    pointDesc.filter = EFilter::MinMagMipPoint;
    pointDesc.addressU = ETextureAddressMode::Clamp;
    pointDesc.addressV = ETextureAddressMode::Clamp;
    pointDesc.addressW = ETextureAddressMode::Clamp;
    m_pointSampler.reset(ctx->CreateSampler(pointDesc));

    // Linear sampler for color
    SamplerDesc linearDesc;
    linearDesc.filter = EFilter::MinMagMipLinear;
    linearDesc.addressU = ETextureAddressMode::Clamp;
    linearDesc.addressV = ETextureAddressMode::Clamp;
    linearDesc.addressW = ETextureAddressMode::Clamp;
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

void CSSRPass::createCompositeShader() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSRComposite.cs.hlsl";

    // Compile SSR composite compute shader
    SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSMain", "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[SSRPass] SSR composite shader compilation failed: %s", compiled.errorMessage.c_str());
        return;
    }

    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    shaderDesc.debugName = "SSRComposite_CS";
    m_compositeCS.reset(ctx->CreateShader(shaderDesc));

    if (!m_compositeCS) {
        CFFLog::Error("[SSRPass] Failed to create SSR composite shader");
        return;
    }

    // Create PSO
    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = m_compositeCS.get();
    psoDesc.debugName = "SSRComposite_PSO";
    m_compositePSO.reset(ctx->CreateComputePipelineState(psoDesc));

    if (!m_compositePSO) {
        CFFLog::Error("[SSRPass] Failed to create SSR composite PSO");
        return;
    }

    CFFLog::Info("[SSRPass] Composite shader compiled successfully");
}

void CSSRPass::Composite(ICommandList* cmdList,
                          ITexture* hdrBuffer,
                          ITexture* worldPosMetallic,
                          ITexture* normalRoughness,
                          uint32_t width, uint32_t height,
                          const XMFLOAT3& camPosWS) {
    if (!m_initialized || !cmdList) return;

    // Check if SSR is enabled and composite PSO exists
    if (!m_settings.enabled || !m_compositePSO) {
        return;
    }

    // Validate inputs
    if (!hdrBuffer || !m_ssrResult || !worldPosMetallic || !normalRoughness) {
        return;
    }

    // Transition HDR buffer to UAV state
    cmdList->Barrier(hdrBuffer, EResourceState::ShaderResource, EResourceState::UnorderedAccess);

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

    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

    // Dispatch compute shader
    uint32_t groupsX = (width + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (height + SSRConfig::THREAD_GROUP_SIZE - 1) / SSRConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

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
