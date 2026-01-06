#include "SSAOPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <cmath>
#include <random>

using namespace DirectX;
using namespace RHI;

// ============================================
// Lifecycle
// ============================================

bool CSSAOPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[SSAOPass] Initializing...");

    createShaders();
    createSamplers();
    createNoiseTexture();

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

    // Check if SSAO is enabled
    if (!m_settings.enabled) {
        return;
    }

    // Ensure textures are properly sized
    if (width != m_fullWidth || height != m_fullHeight) {
        createTextures(width, height);
    }

    // Guard against invalid state
    if (!m_ssaoPSO || !m_ssaoRaw || !depthBuffer || !normalBuffer) {
        return;
    }

    // 1. Downsample depth to half-res
    {
        CScopedDebugEvent evt(cmdList, L"SSAO Depth Downsample");
        dispatchDownsampleDepth(cmdList, depthBuffer);
    }

    // 2. GTAO compute at half-res
    {
        CScopedDebugEvent evt(cmdList, L"SSAO GTAO Compute");
        dispatchSSAO(cmdList, depthBuffer, normalBuffer, view, proj, nearZ, farZ);
    }

    // 3. Horizontal bilateral blur
    {
        CScopedDebugEvent evt(cmdList, L"SSAO Blur H");
        dispatchBlurH(cmdList);
    }

    // 4. Vertical bilateral blur
    {
        CScopedDebugEvent evt(cmdList, L"SSAO Blur V");
        dispatchBlurV(cmdList);
    }

    // 5. Bilateral upsample to full-res
    {
        CScopedDebugEvent evt(cmdList, L"SSAO Upsample");
        dispatchUpsample(cmdList, depthBuffer);
    }
}

// ============================================
// Shader Creation
// ============================================

void CSSAOPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/SSAO.cs.hlsl";

    // GTAO main compute shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSMain", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSAOPass] CSMain compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Compute;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "SSAO_CSMain";
        m_ssaoCS.reset(ctx->CreateShader(desc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_ssaoCS.get();
        psoDesc.debugName = "SSAO_Main_PSO";
        m_ssaoPSO.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Horizontal blur
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSBlurH", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSAOPass] CSBlurH compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Compute;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "SSAO_CSBlurH";
        m_blurHCS.reset(ctx->CreateShader(desc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_blurHCS.get();
        psoDesc.debugName = "SSAO_BlurH_PSO";
        m_blurHPSO.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Vertical blur
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSBlurV", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSAOPass] CSBlurV compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Compute;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "SSAO_CSBlurV";
        m_blurVCS.reset(ctx->CreateShader(desc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_blurVCS.get();
        psoDesc.debugName = "SSAO_BlurV_PSO";
        m_blurVPSO.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Bilateral upsample
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSBilateralUpsample", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSAOPass] CSBilateralUpsample compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Compute;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "SSAO_CSBilateralUpsample";
        m_upsampleCS.reset(ctx->CreateShader(desc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_upsampleCS.get();
        psoDesc.debugName = "SSAO_BilateralUpsample_PSO";
        m_upsamplePSO.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Depth downsample
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSDownsampleDepth", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[SSAOPass] CSDownsampleDepth compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Compute;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "SSAO_CSDownsampleDepth";
        m_downsampleCS.reset(ctx->CreateShader(desc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_downsampleCS.get();
        psoDesc.debugName = "SSAO_DepthDownsample_PSO";
        m_downsamplePSO.reset(ctx->CreateComputePipelineState(psoDesc));
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

    // Half-res raw SSAO output
    {
        TextureDesc desc;
        desc.width = m_halfWidth;
        desc.height = m_halfHeight;
        desc.format = ETextureFormat::R8_UNORM;
        desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
        desc.clearColor[0] = 1.0f;  // Default to fully lit
        desc.debugName = "SSAO_Raw";
        m_ssaoRaw.reset(ctx->CreateTexture(desc, nullptr));
    }

    // Half-res blur temp
    {
        TextureDesc desc;
        desc.width = m_halfWidth;
        desc.height = m_halfHeight;
        desc.format = ETextureFormat::R8_UNORM;
        desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
        desc.clearColor[0] = 1.0f;
        desc.debugName = "SSAO_BlurTemp";
        m_ssaoBlurTemp.reset(ctx->CreateTexture(desc, nullptr));
    }

    // Half-res blurred SSAO
    {
        TextureDesc desc;
        desc.width = m_halfWidth;
        desc.height = m_halfHeight;
        desc.format = ETextureFormat::R8_UNORM;
        desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
        desc.clearColor[0] = 1.0f;
        desc.debugName = "SSAO_HalfBlurred";
        m_ssaoHalfBlurred.reset(ctx->CreateTexture(desc, nullptr));
    }

    // Half-res depth for bilateral upsample
    {
        TextureDesc desc;
        desc.width = m_halfWidth;
        desc.height = m_halfHeight;
        desc.format = ETextureFormat::R32_FLOAT;
        desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
        desc.clearColor[0] = 1.0f;
        desc.debugName = "SSAO_DepthHalfRes";
        m_depthHalfRes.reset(ctx->CreateTexture(desc, nullptr));
    }

    // Full-res final output
    {
        TextureDesc desc;
        desc.width = fullWidth;
        desc.height = fullHeight;
        desc.format = ETextureFormat::R8_UNORM;
        desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;
        desc.clearColor[0] = 1.0f;
        desc.debugName = "SSAO_Final";
        m_ssaoFinal.reset(ctx->CreateTexture(desc, nullptr));
    }

    CFFLog::Info("[SSAOPass] Textures resized: Full=%ux%u, Half=%ux%u",
                 fullWidth, fullHeight, m_halfWidth, m_halfHeight);
}

// ============================================
// Noise Texture
// ============================================

void CSSAOPass::createNoiseTexture() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Generate 4x4 random rotation vectors
    constexpr uint32_t noiseSize = SSAOConfig::NOISE_TEXTURE_SIZE;
    std::vector<uint8_t> noiseData(noiseSize * noiseSize * 4);  // R8G8B8A8

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (uint32_t i = 0; i < noiseSize * noiseSize; ++i) {
        // Random rotation angle [0, 2*PI)
        float angle = dist(rng) * 2.0f * 3.14159265f;

        // Store cos and sin as [0,255] (remapped from [-1,1])
        noiseData[i * 4 + 0] = static_cast<uint8_t>((std::cos(angle) * 0.5f + 0.5f) * 255.0f);
        noiseData[i * 4 + 1] = static_cast<uint8_t>((std::sin(angle) * 0.5f + 0.5f) * 255.0f);
        noiseData[i * 4 + 2] = 128;  // B unused (0.5)
        noiseData[i * 4 + 3] = 255;  // A unused (1.0)
    }

    TextureDesc desc;
    desc.width = noiseSize;
    desc.height = noiseSize;
    desc.format = ETextureFormat::R8G8B8A8_UNORM;
    desc.usage = ETextureUsage::ShaderResource;
    desc.debugName = "SSAO_Noise";

    m_noiseTexture.reset(ctx->CreateTexture(desc, noiseData.data()));

    CFFLog::Info("[SSAOPass] Noise texture created (%ux%u)", noiseSize, noiseSize);
}

// ============================================
// Samplers
// ============================================

void CSSAOPass::createSamplers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Point sampler for depth/AO
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipPoint;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        m_pointSampler.reset(ctx->CreateSampler(desc));
    }

    // Linear sampler for upsample
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        m_linearSampler.reset(ctx->CreateSampler(desc));
    }
}

// ============================================
// Dispatch Helpers
// ============================================

void CSSAOPass::dispatchDownsampleDepth(ICommandList* cmdList, ITexture* depthFullRes) {
    if (!m_downsamplePSO || !m_depthHalfRes) return;

    // Constant buffer for downsample
    struct CB_Downsample {
        float texelSizeX, texelSizeY;
        float _pad[2];
    } cb;

    cb.texelSizeX = 1.0f / static_cast<float>(m_fullWidth);
    cb.texelSizeY = 1.0f / static_cast<float>(m_fullHeight);

    cmdList->SetPipelineState(m_downsamplePSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Compute, 0, depthFullRes);
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());
    cmdList->SetSampler(EShaderStage::Compute, 1, m_linearSampler.get());  // Must bind both samplers
    cmdList->SetUnorderedAccessTexture(0, m_depthHalfRes.get());

    uint32_t groupsX = (m_halfWidth + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (m_halfHeight + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    // Unbind UAV
    cmdList->SetUnorderedAccessTexture(0, nullptr);
}

void CSSAOPass::dispatchSSAO(ICommandList* cmdList,
                              ITexture* depthBuffer,
                              ITexture* normalBuffer,
                              const XMMATRIX& view,
                              const XMMATRIX& proj,
                              float nearZ, float farZ) {
    if (!m_ssaoPSO || !m_ssaoRaw) return;

    // Build constant buffer
    CB_SSAO cb{};
    XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(view));

    cb.texelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.texelSize.y = 1.0f / static_cast<float>(m_halfHeight);

    // Noise tiling: resolution / 4 (noise is 4x4)
    cb.noiseScale.x = static_cast<float>(m_halfWidth) / static_cast<float>(SSAOConfig::NOISE_TEXTURE_SIZE);
    cb.noiseScale.y = static_cast<float>(m_halfHeight) / static_cast<float>(SSAOConfig::NOISE_TEXTURE_SIZE);

    cb.radius = m_settings.radius;
    cb.intensity = m_settings.intensity;
    cb.falloffStart = m_settings.falloffStart;
    cb.falloffEnd = m_settings.falloffEnd;
    cb.numSlices = m_settings.numSlices;
    cb.numSteps = m_settings.numSteps;
    cb.thicknessHeuristic = m_settings.thicknessHeuristic;

    cmdList->SetPipelineState(m_ssaoPSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Compute, 0, m_depthHalfRes.get());
    cmdList->SetShaderResource(EShaderStage::Compute, 1, normalBuffer);
    cmdList->SetShaderResource(EShaderStage::Compute, 2, m_noiseTexture.get());
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());
    cmdList->SetSampler(EShaderStage::Compute, 1, m_linearSampler.get());  // Must bind both samplers
    cmdList->SetUnorderedAccessTexture(0, m_ssaoRaw.get());

    uint32_t groupsX = (m_halfWidth + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (m_halfHeight + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    // Unbind
    cmdList->SetUnorderedAccessTexture(0, nullptr);
    cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 3);
}

void CSSAOPass::dispatchBlurH(ICommandList* cmdList) {
    if (!m_blurHPSO || !m_ssaoBlurTemp) return;

    CB_SSAOBlur cb{};
    cb.blurDirection = XMFLOAT2(1.0f, 0.0f);
    cb.texelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.texelSize.y = 1.0f / static_cast<float>(m_halfHeight);
    cb.depthSigma = m_settings.depthSigma;
    cb.blurRadius = m_settings.blurRadius;

    cmdList->SetPipelineState(m_blurHPSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Compute, 0, m_ssaoRaw.get());
    cmdList->SetShaderResource(EShaderStage::Compute, 1, m_depthHalfRes.get());
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());
    cmdList->SetSampler(EShaderStage::Compute, 1, m_linearSampler.get());  // Must bind both samplers
    cmdList->SetUnorderedAccessTexture(0, m_ssaoBlurTemp.get());

    uint32_t groupsX = (m_halfWidth + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (m_halfHeight + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    cmdList->SetUnorderedAccessTexture(0, nullptr);
    cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 2);
}

void CSSAOPass::dispatchBlurV(ICommandList* cmdList) {
    if (!m_blurVPSO || !m_ssaoHalfBlurred) return;

    CB_SSAOBlur cb{};
    cb.blurDirection = XMFLOAT2(0.0f, 1.0f);
    cb.texelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.texelSize.y = 1.0f / static_cast<float>(m_halfHeight);
    cb.depthSigma = m_settings.depthSigma;
    cb.blurRadius = m_settings.blurRadius;

    cmdList->SetPipelineState(m_blurVPSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Compute, 0, m_ssaoBlurTemp.get());
    cmdList->SetShaderResource(EShaderStage::Compute, 1, m_depthHalfRes.get());
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());
    cmdList->SetSampler(EShaderStage::Compute, 1, m_linearSampler.get());  // Must bind both samplers
    cmdList->SetUnorderedAccessTexture(0, m_ssaoHalfBlurred.get());

    uint32_t groupsX = (m_halfWidth + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (m_halfHeight + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    cmdList->SetUnorderedAccessTexture(0, nullptr);
    cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 2);
}

void CSSAOPass::dispatchUpsample(ICommandList* cmdList, ITexture* depthFullRes) {
    if (!m_upsamplePSO || !m_ssaoFinal) return;

    CB_SSAOUpsample cb{};
    cb.fullResTexelSize.x = 1.0f / static_cast<float>(m_fullWidth);
    cb.fullResTexelSize.y = 1.0f / static_cast<float>(m_fullHeight);
    cb.halfResTexelSize.x = 1.0f / static_cast<float>(m_halfWidth);
    cb.halfResTexelSize.y = 1.0f / static_cast<float>(m_halfHeight);
    cb.depthSigma = m_settings.depthSigma;

    cmdList->SetPipelineState(m_upsamplePSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Compute, 0, m_ssaoHalfBlurred.get());
    cmdList->SetShaderResource(EShaderStage::Compute, 1, m_depthHalfRes.get());
    cmdList->SetShaderResource(EShaderStage::Compute, 2, depthFullRes);
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());
    cmdList->SetSampler(EShaderStage::Compute, 1, m_linearSampler.get());
    cmdList->SetUnorderedAccessTexture(0, m_ssaoFinal.get());

    uint32_t groupsX = (m_fullWidth + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (m_fullHeight + SSAOConfig::THREAD_GROUP_SIZE - 1) / SSAOConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    cmdList->SetUnorderedAccessTexture(0, nullptr);
    cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 3);
}
