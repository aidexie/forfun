#include "TAAPass.h"
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

using namespace DirectX;
using namespace RHI;

namespace {

constexpr uint32_t kThreadGroupSize = 8;

uint32_t calcDispatchGroups(uint32_t size) {
    return (size + kThreadGroupSize - 1) / kThreadGroupSize;
}

bool createComputeShaderAndPSO(IRenderContext* ctx,
                                const std::string& shader_path,
                                const char* entry_point,
                                const char* shader_name,
                                const char* pso_name,
                                bool debug_shaders,
                                ShaderPtr& out_shader,
                                PipelineStatePtr& out_pso) {
    SCompiledShader compiled = CompileShaderFromFile(shader_path, entry_point, "cs_5_0", nullptr, debug_shaders);
    if (!compiled.success) {
        CFFLog::Error("[TAAPass] %s compilation failed: %s", entry_point, compiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc desc;
    desc.type = EShaderType::Compute;
    desc.bytecode = compiled.bytecode.data();
    desc.bytecodeSize = compiled.bytecode.size();
    desc.debugName = shader_name;
    out_shader.reset(ctx->CreateShader(desc));

    ComputePipelineDesc pso_desc;
    pso_desc.computeShader = out_shader.get();
    pso_desc.debugName = pso_name;
    out_pso.reset(ctx->CreateComputePipelineState(pso_desc));

    return true;
}

}  // namespace

bool CTAAPass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[TAAPass] Initializing...");

    createShaders();

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        m_linear_sampler.reset(ctx->CreateSampler(desc));

        desc.filter = EFilter::MinMagMipPoint;
        m_point_sampler.reset(ctx->CreateSampler(desc));
    }

    initDescriptorSets();

    m_initialized = true;
    CFFLog::Info("[TAAPass] Initialized");
    return true;
}

void CTAAPass::Shutdown() {
    m_taa_cs.reset();
    m_sharpen_cs.reset();
    m_taa_pso.reset();
    m_sharpen_pso.reset();

    m_history[0].reset();
    m_history[1].reset();
    m_output.reset();
    m_sharpen_output.reset();

    m_linear_sampler.reset();
    m_point_sampler.reset();

    // Cleanup DS resources
    m_taa_cs_ds.reset();
    m_sharpen_cs_ds.reset();
    m_taa_pso_ds.reset();
    m_sharpen_pso_ds.reset();

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
    m_frame_index = 0;
    m_history_index = 0;
    m_history_valid = false;
    m_initialized = false;

    CFFLog::Info("[TAAPass] Shutdown");
}

void CTAAPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    constexpr bool kDebugShaders = true;
#else
    constexpr bool kDebugShaders = false;
#endif

    std::string taa_path = FFPath::GetSourceDir() + "/Shader/TAA.cs.hlsl";
    std::string sharpen_path = FFPath::GetSourceDir() + "/Shader/TAASharpen.cs.hlsl";

    if (!createComputeShaderAndPSO(ctx, taa_path, "CSMain", "TAA_CS", "TAA_PSO",
                                    kDebugShaders, m_taa_cs, m_taa_pso)) {
        CFFLog::Error("[TAAPass] Failed to create TAA shader");
        return;
    }

    if (!createComputeShaderAndPSO(ctx, sharpen_path, "CSMain", "TAASharpen_CS", "TAASharpen_PSO",
                                    kDebugShaders, m_sharpen_cs, m_sharpen_pso)) {
        CFFLog::Warning("[TAAPass] Failed to create sharpening shader (optional)");
    }

    CFFLog::Info("[TAAPass] Shaders created");
}

void CTAAPass::createTextures(uint32_t width, uint32_t height) {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    m_width = width;
    m_height = height;

    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = ETextureFormat::R16G16B16A16_FLOAT;
    desc.usage = ETextureUsage::UnorderedAccess | ETextureUsage::ShaderResource;

    desc.debugName = "TAA_History0";
    m_history[0].reset(ctx->CreateTexture(desc, nullptr));

    desc.debugName = "TAA_History1";
    m_history[1].reset(ctx->CreateTexture(desc, nullptr));

    desc.debugName = "TAA_Output";
    m_output.reset(ctx->CreateTexture(desc, nullptr));

    if (m_sharpen_pso) {
        desc.debugName = "TAA_SharpenOutput";
        m_sharpen_output.reset(ctx->CreateTexture(desc, nullptr));
    }

    m_history_valid = false;
    m_history_index = 0;

    CFFLog::Info("[TAAPass] Textures created: %ux%u", width, height);
}

void CTAAPass::ensureTextures(uint32_t width, uint32_t height) {
    if (width != m_width || height != m_height) {
        createTextures(width, height);
    }
}

void CTAAPass::InvalidateHistory() {
    m_history_valid = false;
    m_frame_index = 0;
}

void CTAAPass::Render(ICommandList* cmd_list,
                       ITexture* current_color,
                       ITexture* velocity_buffer,
                       ITexture* depth_buffer,
                       uint32_t width, uint32_t height,
                       const XMMATRIX& view_proj,
                       const XMMATRIX& prev_view_proj,
                       const XMFLOAT2& jitter_offset,
                       const XMFLOAT2& prev_jitter_offset) {
    if (!m_initialized || !cmd_list) return;
    if (m_settings.algorithm == ETAAAlgorithm::Off) return;

    ensureTextures(width, height);

    if (!m_taa_pso || !m_output || !current_color || !velocity_buffer || !depth_buffer) return;

    ITexture* history_read = m_history[m_history_index].get();
    ITexture* history_write = m_history[1 - m_history_index].get();

    // TAA Resolve
    {
        CScopedDebugEvent evt(cmd_list, L"TAA Resolve");

        CB_TAA cb{};
        XMStoreFloat4x4(&cb.inv_view_proj, XMMatrixTranspose(XMMatrixInverse(nullptr, view_proj)));
        XMStoreFloat4x4(&cb.prev_view_proj, XMMatrixTranspose(prev_view_proj));
        cb.screen_size = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
        cb.texel_size = XMFLOAT2(1.0f / width, 1.0f / height);
        cb.jitter_offset = jitter_offset;
        cb.prev_jitter_offset = prev_jitter_offset;
        cb.history_blend = m_settings.history_blend;
        cb.variance_clip_gamma = m_settings.variance_clip_gamma;
        cb.velocity_rejection_scale = m_settings.velocity_rejection_scale;
        cb.depth_rejection_scale = m_settings.depth_rejection_scale;
        cb.algorithm = static_cast<uint32_t>(m_settings.algorithm);
        cb.frame_index = m_frame_index;
        cb.flags = m_history_valid ? 0 : 1;

        cmd_list->SetPipelineState(m_taa_pso.get());
        cmd_list->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

        cmd_list->SetShaderResource(EShaderStage::Compute, 0, current_color);
        cmd_list->SetShaderResource(EShaderStage::Compute, 1, velocity_buffer);
        cmd_list->SetShaderResource(EShaderStage::Compute, 2, depth_buffer);
        cmd_list->SetShaderResource(EShaderStage::Compute, 3, history_read);

        cmd_list->SetSampler(EShaderStage::Compute, 0, m_linear_sampler.get());
        cmd_list->SetSampler(EShaderStage::Compute, 1, m_point_sampler.get());

        cmd_list->SetUnorderedAccessTexture(0, history_write);
        cmd_list->Dispatch(calcDispatchGroups(width), calcDispatchGroups(height), 1);

        cmd_list->SetUnorderedAccessTexture(0, nullptr);
        cmd_list->UnbindShaderResources(EShaderStage::Compute, 0, 4);
    }

    // Sharpening (Production level only)
    if (m_settings.algorithm == ETAAAlgorithm::Production &&
        m_settings.sharpening_enabled && m_sharpen_pso && m_sharpen_output) {
        CScopedDebugEvent evt(cmd_list, L"TAA Sharpen");

        CB_TAASharpen cb{};
        cb.screen_size = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
        cb.texel_size = XMFLOAT2(1.0f / width, 1.0f / height);
        cb.sharpen_strength = m_settings.sharpening_strength;

        cmd_list->SetPipelineState(m_sharpen_pso.get());
        cmd_list->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

        cmd_list->SetShaderResource(EShaderStage::Compute, 0, history_write);
        cmd_list->SetSampler(EShaderStage::Compute, 0, m_point_sampler.get());
        cmd_list->SetUnorderedAccessTexture(0, m_sharpen_output.get());

        cmd_list->Dispatch(calcDispatchGroups(width), calcDispatchGroups(height), 1);

        cmd_list->SetUnorderedAccessTexture(0, nullptr);
        cmd_list->UnbindShaderResources(EShaderStage::Compute, 0, 1);

        cmd_list->CopyTexture(m_output.get(), m_sharpen_output.get());
    } else {
        cmd_list->CopyTexture(m_output.get(), history_write);
    }

    m_history_index = 1 - m_history_index;
    m_history_valid = true;
    m_frame_index++;
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CTAAPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[TAAPass] DX11 mode - descriptor sets not supported");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Create unified compute layout
    m_computePerPassLayout = ComputePassLayout::CreateComputePerPassLayout(ctx);
    if (!m_computePerPassLayout) {
        CFFLog::Error("[TAAPass] Failed to create compute PerPass layout");
        return;
    }

    // Allocate descriptor set
    m_perPassSet = ctx->AllocateDescriptorSet(m_computePerPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[TAAPass] Failed to allocate PerPass descriptor set");
        return;
    }

    // Bind static samplers
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Point, m_point_sampler.get()));
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Linear, m_linear_sampler.get()));

    // Compile SM 5.1 TAA shader
    {
        std::string shaderPath = FFPath::GetSourceDir() + "/Shader/TAA_DS.cs.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSMain", "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[TAAPass] CSMain (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = "TAA_DS_CS";
        m_taa_cs_ds.reset(ctx->CreateShader(shaderDesc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_taa_cs_ds.get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = "TAA_DS_PSO";
        m_taa_pso_ds.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Compile SM 5.1 Sharpen shader
    {
        std::string shaderPath = FFPath::GetSourceDir() + "/Shader/TAASharpen_DS.cs.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSMain", "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Warning("[TAAPass] Sharpen (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            // Sharpen is optional, continue without it
        } else {
            ShaderDesc shaderDesc;
            shaderDesc.type = EShaderType::Compute;
            shaderDesc.bytecode = compiled.bytecode.data();
            shaderDesc.bytecodeSize = compiled.bytecode.size();
            shaderDesc.debugName = "TAASharpen_DS_CS";
            m_sharpen_cs_ds.reset(ctx->CreateShader(shaderDesc));

            ComputePipelineDesc psoDesc;
            psoDesc.computeShader = m_sharpen_cs_ds.get();
            psoDesc.setLayouts[1] = m_computePerPassLayout;
            psoDesc.debugName = "TAASharpen_DS_PSO";
            m_sharpen_pso_ds.reset(ctx->CreateComputePipelineState(psoDesc));
        }
    }

    CFFLog::Info("[TAAPass] Descriptor set resources initialized");
}
