#include "PostProcessPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include "Core/FFLog.h"
#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace RHI;

struct FullscreenVertex {
    float x, y;       // Position (NDC space)
    float u, v;       // UV
};

struct CB_PostProcess {
    float exposure;
    float _pad[3];
};

bool CPostProcessPass::Initialize() {
    if (m_initialized) return true;

    createFullscreenQuad();
    createShaders();
    createPipelineState();

    // Create sampler
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Clamp;
    samplerDesc.addressV = ETextureAddressMode::Clamp;
    samplerDesc.addressW = ETextureAddressMode::Clamp;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    m_sampler.reset(ctx->CreateSampler(samplerDesc));

    // Create constant buffer (CPU writable for updating exposure)
    BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_PostProcess);
    cbDesc.usage = EBufferUsage::Constant;
    cbDesc.cpuAccess = ECPUAccess::Write;
    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));

    m_initialized = true;
    return true;
}

void CPostProcessPass::Shutdown() {
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_vertexBuffer.reset();
    m_constantBuffer.reset();
    m_sampler.reset();
    m_initialized = false;
}

void CPostProcessPass::Render(ITexture* hdrInput,
                              ITexture* ldrOutput,
                              uint32_t width, uint32_t height,
                              float exposure) {
    if (!m_initialized || !hdrInput || !ldrOutput || !m_pso) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // Update constant buffer with exposure
    CB_PostProcess cb;
    cb.exposure = exposure;
    cb._pad[0] = cb._pad[1] = cb._pad[2] = 0.0f;

    void* mappedData = m_constantBuffer->Map();
    if (mappedData) {
        memcpy(mappedData, &cb, sizeof(CB_PostProcess));
        m_constantBuffer->Unmap();
    }

    // Set render target (no depth)
    ITexture* renderTargets[] = { ldrOutput };
    cmdList->SetRenderTargets(1, renderTargets, nullptr);

    // Set viewport
    cmdList->SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);

    // Set pipeline state (includes rasterizer, depth stencil, blend states)
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);

    // Set vertex buffer
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(FullscreenVertex), 0);

    // Set constant buffer and resources
    cmdList->SetConstantBuffer(EShaderStage::Pixel, 0, m_constantBuffer.get());
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

    // Draw fullscreen quad
    cmdList->Draw(4, 0);

    // Unbind render target to prevent hazards
    cmdList->SetRenderTargets(0, nullptr, nullptr);
}

void CPostProcessPass::createFullscreenQuad() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Fullscreen quad in NDC space (triangle strip)
    FullscreenVertex vertices[] = {
        { -1.0f,  1.0f,  0.0f, 0.0f },  // Top-left
        {  1.0f,  1.0f,  1.0f, 0.0f },  // Top-right
        { -1.0f, -1.0f,  0.0f, 1.0f },  // Bottom-left
        {  1.0f, -1.0f,  1.0f, 1.0f }   // Bottom-right
    };

    BufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.usage = EBufferUsage::Vertex;
    vbDesc.cpuAccess = ECPUAccess::None;

    m_vertexBuffer.reset(ctx->CreateBuffer(vbDesc, vertices));
}

void CPostProcessPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Vertex shader: Pass-through with UV
    const char* vsCode = R"(
        struct VSIn {
            float2 pos : POSITION;
            float2 uv : TEXCOORD0;
        };
        struct VSOut {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };
        VSOut main(VSIn input) {
            VSOut output;
            output.pos = float4(input.pos, 0.0, 1.0);
            output.uv = input.uv;
            return output;
        }
    )";

    // Pixel shader: Tone mapping + Gamma correction
    const char* psCode = R"(
        Texture2D hdrTexture : register(t0);
        SamplerState samp : register(s0);

        cbuffer CB_PostProcess : register(b0) {
            float gExposure;
            float3 _pad;
        };

        struct PSIn {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };

        // ACES Filmic Tone Mapping
        float3 ACESFilm(float3 x) {
            float a = 2.51;
            float b = 0.03;
            float c = 2.43;
            float d = 0.59;
            float e = 0.14;
            return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
        }

        float4 main(PSIn input) : SV_Target {
            // Sample HDR input (linear space)
            float3 hdrColor = hdrTexture.Sample(samp, input.uv).rgb;

            // Apply exposure (adjust brightness before tone mapping)
            hdrColor *= gExposure;

            // Tone mapping: HDR → LDR [0, 1] (still linear space)
            float3 ldrColor = ACESFilm(hdrColor);

            // Gamma correction: Linear → sRGB
            // Since output RT is UNORM_SRGB, GPU will do this automatically

            return float4(ldrColor, 1.0);
        }
    )";

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err = nullptr;

    // Compile Vertex Shader
    HRESULT hr = D3DCompile(vsCode, strlen(vsCode), "PostProcess_VS", nullptr, nullptr,
                           "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("PostProcess VS compilation error: %s", (const char*)err->GetBufferPointer());
            err->Release();
        }
        return;
    }

    // Compile Pixel Shader
    hr = D3DCompile(psCode, strlen(psCode), "PostProcess_PS", nullptr, nullptr,
                   "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("PostProcess PS compilation error: %s", (const char*)err->GetBufferPointer());
            err->Release();
        }
        vsBlob->Release();
        return;
    }

    // Create shader objects using RHI
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsBlob->GetBufferPointer();
    vsDesc.bytecodeSize = vsBlob->GetBufferSize();
    m_vs.reset(ctx->CreateShader(vsDesc));

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psBlob->GetBufferPointer();
    psDesc.bytecodeSize = psBlob->GetBufferSize();
    m_ps.reset(ctx->CreateShader(psDesc));

    vsBlob->Release();
    psBlob->Release();
}

void CPostProcessPass::createPipelineState() {
    if (!m_vs || !m_ps) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();

    // Input layout: POSITION + TEXCOORD
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 0, 8 }
    };

    // Rasterizer state: no culling, no depth clip
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.depthClipEnable = false;

    // Depth stencil state: no depth test
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.depthStencil.depthWriteEnable = false;

    // Blend state: no blending
    psoDesc.blend.blendEnable = false;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;

    m_pso.reset(ctx->CreatePipelineState(psoDesc));
}
