#include "HiZPass.h"
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
#include <cmath>

using namespace RHI;

// ============================================
// Lifecycle
// ============================================

bool CHiZPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[HiZPass] Initializing...");

    createShaders();
    createSamplers();
    initDescriptorSets();

    m_initialized = true;
    CFFLog::Info("[HiZPass] Initialized successfully");
    return true;
}

void CHiZPass::Shutdown() {
    m_copyDepthCS.reset();
    m_buildMipCS.reset();
    m_copyDepthPSO.reset();
    m_buildMipPSO.reset();
    m_hiZTexture.reset();
    m_pointSampler.reset();

    // Cleanup DS resources
    m_copyDepthCS_ds.reset();
    m_buildMipCS_ds.reset();
    m_copyDepthPSO_ds.reset();
    m_buildMipPSO_ds.reset();

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
    m_mipCount = 0;
    m_initialized = false;

    CFFLog::Info("[HiZPass] Shutdown");
}

// ============================================
// Shader Creation
// ============================================

void CHiZPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/HiZ.cs.hlsl";

    // Compile CSCopyDepth shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSCopyDepth", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[HiZPass] CSCopyDepth compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = "HiZ_CopyDepth_CS";
        m_copyDepthCS.reset(ctx->CreateShader(shaderDesc));

        if (!m_copyDepthCS) {
            CFFLog::Error("[HiZPass] Failed to create CSCopyDepth shader");
            return;
        }

        // Create PSO
        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_copyDepthCS.get();
        psoDesc.debugName = "HiZ_CopyDepth_PSO";
        m_copyDepthPSO.reset(ctx->CreateComputePipelineState(psoDesc));

        if (!m_copyDepthPSO) {
            CFFLog::Error("[HiZPass] Failed to create CSCopyDepth PSO");
            return;
        }
    }

    // Compile CSBuildMip shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSBuildMip", "cs_5_0", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[HiZPass] CSBuildMip compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = "HiZ_BuildMip_CS";
        m_buildMipCS.reset(ctx->CreateShader(shaderDesc));

        if (!m_buildMipCS) {
            CFFLog::Error("[HiZPass] Failed to create CSBuildMip shader");
            return;
        }

        // Create PSO
        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_buildMipCS.get();
        psoDesc.debugName = "HiZ_BuildMip_PSO";
        m_buildMipPSO.reset(ctx->CreateComputePipelineState(psoDesc));

        if (!m_buildMipPSO) {
            CFFLog::Error("[HiZPass] Failed to create CSBuildMip PSO");
            return;
        }
    }

    CFFLog::Info("[HiZPass] Shaders compiled successfully");
}

void CHiZPass::createSamplers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    SamplerDesc sampDesc;
    sampDesc.filter = EFilter::MinMagMipPoint;
    sampDesc.addressU = ETextureAddressMode::Clamp;
    sampDesc.addressV = ETextureAddressMode::Clamp;
    sampDesc.addressW = ETextureAddressMode::Clamp;
    m_pointSampler.reset(ctx->CreateSampler(sampDesc));
}

void CHiZPass::createTextures(uint32_t width, uint32_t height) {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    if (width == 0 || height == 0) return;

    // Calculate mip count (full chain to 1x1)
    uint32_t maxDim = std::max(width, height);
    m_mipCount = 1;
    while (maxDim > 1) {
        maxDim >>= 1;
        m_mipCount++;
    }

    m_width = width;
    m_height = height;

    // Create Hi-Z pyramid texture
    // R32_FLOAT for full depth precision
    // ShaderResource + UnorderedAccess for compute shader access
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = ETextureFormat::R32_FLOAT;
    desc.mipLevels = m_mipCount;
    desc.usage = ETextureUsage::ShaderResource | ETextureUsage::UnorderedAccess;
    desc.dimension = ETextureDimension::Tex2D;
    desc.debugName = "HiZ_Pyramid";

    m_hiZTexture.reset(ctx->CreateTexture(desc, nullptr));

    if (!m_hiZTexture) {
        CFFLog::Error("[HiZPass] Failed to create Hi-Z pyramid texture");
        return;
    }

    CFFLog::Info("[HiZPass] Created Hi-Z pyramid: %ux%u, %u mips", width, height, m_mipCount);
}

// ============================================
// Rendering
// ============================================

void CHiZPass::BuildPyramid(ICommandList* cmdList,
                             ITexture* depthBuffer,
                             uint32_t width, uint32_t height) {
    if (!m_initialized || !cmdList || !depthBuffer) return;

#ifdef FF_LEGACY_BINDING_DISABLED
    CFFLog::Warning("[HiZPass] BuildPyramid called but legacy binding is disabled and descriptor set path not implemented");
    return;
#else
    // Ensure textures are properly sized
    if (width != m_width || height != m_height) {
        createTextures(width, height);
    }

    // Guard against invalid state
    if (!m_copyDepthPSO || !m_buildMipPSO || !m_hiZTexture) {
        return;
    }

    // Transition Hi-Z texture to UAV state for writing
    // (It's in ShaderResource state from previous frame, or COMMON state on first use)
    cmdList->Barrier(m_hiZTexture.get(), EResourceState::ShaderResource, EResourceState::UnorderedAccess);

    // Step 1: Copy depth buffer to mip 0
    {
        CScopedDebugEvent evt(cmdList, L"HiZ Copy Depth");
        dispatchCopyDepth(cmdList, depthBuffer);
    }

    // Step 2: Build mip chain (mip 1 to mipCount-1)
    // Each dispatch reads from previous mip and writes to current mip
    // Need UAV barrier between dispatches to ensure writes are visible
    for (uint32_t mip = 1; mip < m_mipCount; ++mip) {
        // UAV barrier to ensure previous mip write is complete before reading
        cmdList->Barrier(m_hiZTexture.get(), EResourceState::UnorderedAccess, EResourceState::UnorderedAccess);
        dispatchBuildMip(cmdList, mip);
    }

    // Final barrier to SRV state for use by SSR
    cmdList->Barrier(m_hiZTexture.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
#endif // FF_LEGACY_BINDING_DISABLED
}

void CHiZPass::dispatchCopyDepth(ICommandList* cmdList, ITexture* depthBuffer) {
#ifndef FF_LEGACY_BINDING_DISABLED
    // Set PSO
    cmdList->SetPipelineState(m_copyDepthPSO.get());

    // Bind depth buffer as SRV (t0)
    cmdList->SetShaderResource(EShaderStage::Compute, 0, depthBuffer);

    // Bind mip 0 as UAV (u0)
    cmdList->SetUnorderedAccessTextureMip(0, m_hiZTexture.get(), 0);

    // Set constant buffer
    CB_HiZ cb;
    cb.srcMipSizeX = m_width;
    cb.srcMipSizeY = m_height;
    cb.dstMipSizeX = m_width;
    cb.dstMipSizeY = m_height;
    cb.srcMipLevel = 0;
    cb._pad[0] = cb._pad[1] = cb._pad[2] = 0;
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

    // Bind sampler
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());

    // Dispatch
    uint32_t groupsX = (m_width + HiZConfig::THREAD_GROUP_SIZE - 1) / HiZConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (m_height + HiZConfig::THREAD_GROUP_SIZE - 1) / HiZConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    // UAV barrier before next pass reads this mip
    cmdList->Barrier(m_hiZTexture.get(), EResourceState::UnorderedAccess, EResourceState::UnorderedAccess);
#endif // FF_LEGACY_BINDING_DISABLED
}

void CHiZPass::dispatchBuildMip(ICommandList* cmdList, uint32_t mipLevel) {
#ifndef FF_LEGACY_BINDING_DISABLED
    // Calculate source and destination dimensions
    uint32_t srcWidth = std::max(1u, m_width >> (mipLevel - 1));
    uint32_t srcHeight = std::max(1u, m_height >> (mipLevel - 1));
    uint32_t dstWidth = std::max(1u, m_width >> mipLevel);
    uint32_t dstHeight = std::max(1u, m_height >> mipLevel);

    // Set PSO
    cmdList->SetPipelineState(m_buildMipPSO.get());

    // Bind previous mip as UAV for reading (u1)
    // Using UAV read avoids SRV/UAV state conflict - texture stays in UAV state
    cmdList->SetUnorderedAccessTextureMip(1, m_hiZTexture.get(), mipLevel - 1);

    // Bind current mip as UAV for writing (u0)
    cmdList->SetUnorderedAccessTextureMip(0, m_hiZTexture.get(), mipLevel);

    // Set constant buffer
    CB_HiZ cb;
    cb.srcMipSizeX = srcWidth;
    cb.srcMipSizeY = srcHeight;
    cb.dstMipSizeX = dstWidth;
    cb.dstMipSizeY = dstHeight;
    cb.srcMipLevel = mipLevel - 1;
    cb._pad[0] = cb._pad[1] = cb._pad[2] = 0;
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

    // Bind sampler
    cmdList->SetSampler(EShaderStage::Compute, 0, m_pointSampler.get());

    // Dispatch
    uint32_t groupsX = (dstWidth + HiZConfig::THREAD_GROUP_SIZE - 1) / HiZConfig::THREAD_GROUP_SIZE;
    uint32_t groupsY = (dstHeight + HiZConfig::THREAD_GROUP_SIZE - 1) / HiZConfig::THREAD_GROUP_SIZE;
    cmdList->Dispatch(groupsX, groupsY, 1);

    // UAV barrier before next mip level reads this one
    //cmdList->Barrier(m_hiZTexture.get(), EResourceState::UnorderedAccess, EResourceState::UnorderedAccess);
#endif // FF_LEGACY_BINDING_DISABLED
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CHiZPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[HiZPass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/HiZ_DS.cs.hlsl";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Create unified compute layout
    m_computePerPassLayout = ComputePassLayout::CreateComputePerPassLayout(ctx);
    if (!m_computePerPassLayout) {
        CFFLog::Error("[HiZPass] Failed to create compute PerPass layout");
        return;
    }

    // Allocate descriptor set
    m_perPassSet = ctx->AllocateDescriptorSet(m_computePerPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[HiZPass] Failed to allocate PerPass descriptor set");
        return;
    }

    // Bind static sampler
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Point, m_pointSampler.get()));

    // Compile SM 5.1 shaders
    struct ShaderDef {
        const char* entryPoint;
        const char* shaderName;
        const char* psoName;
        ShaderPtr* shader;
        PipelineStatePtr* pso;
    };

    ShaderDef shaders[] = {
        {"CSCopyDepth", "HiZ_DS_CSCopyDepth", "HiZ_DS_CopyDepth_PSO", &m_copyDepthCS_ds, &m_copyDepthPSO_ds},
        {"CSBuildMip",  "HiZ_DS_CSBuildMip",  "HiZ_DS_BuildMip_PSO",  &m_buildMipCS_ds,  &m_buildMipPSO_ds},
    };

    for (const auto& def : shaders) {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, def.entryPoint, "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[HiZPass] %s (SM 5.1) compilation failed: %s", def.entryPoint, compiled.errorMessage.c_str());
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
        psoDesc.setLayouts[1] = m_computePerPassLayout;
        psoDesc.debugName = def.psoName;
        def.pso->reset(ctx->CreateComputePipelineState(psoDesc));
    }

    CFFLog::Info("[HiZPass] Descriptor set resources initialized");
}
