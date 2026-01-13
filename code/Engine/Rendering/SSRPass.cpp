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

using namespace DirectX;
using namespace RHI;

// ============================================
// Lifecycle
// ============================================

bool CSSRPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[SSRPass] Initializing...");

    createShaders();
    createSamplers();
    createFallbackTexture();

    m_initialized = true;
    CFFLog::Info("[SSRPass] Initialized successfully");
    return true;
}

void CSSRPass::Shutdown() {
    m_ssrCS.reset();
    m_ssrPSO.reset();

    m_ssrResult.reset();
    m_blackFallback.reset();

    m_pointSampler.reset();
    m_linearSampler.reset();

    m_width = 0;
    m_height = 0;
    m_initialized = false;

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
    cb._pad[0] = cb._pad[1] = 0.0f;

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
