#include "Skybox.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"
#include "Core/Loader/HdrLoader.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Core/PathManager.h"
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>

using namespace DirectX;
using namespace RHI;

// Helper function to load shader source from file
static std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct SkyboxVertex {
    XMFLOAT3 position;
};

struct CB_SkyboxTransform {
    XMMATRIX viewProj;
    uint32_t useReversedZ;
    uint32_t padding[3];  // Align to 16 bytes
};

bool CSkybox::Initialize(const std::string& hdrPath, int cubemapSize) {
    if (m_initialized) return true;

    // Convert equirectangular HDR to cubemap using RHI
    convertEquirectToCubemapLegacy(hdrPath, cubemapSize);
    if (!m_envTexture) return false;

    // Create cube mesh
    createCubeMesh();

    // Create shaders
    createShaders();

    // Create pipeline state
    createPipelineState();

    // Create constant buffer
    createConstantBuffer();

    // Create sampler
    createSampler();

    // Initialize descriptor sets (DX12)
    initDescriptorSets();

    m_initialized = true;
    return true;
}

bool CSkybox::InitializeFromKTX2(const std::string& ktx2Path) {
    if (m_initialized) return true;

    m_envPathKTX2 = ktx2Path;

    // Load cubemap from KTX2 (returns RHI texture with SRV)
    RHI::ITexture* rhiTexture = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!rhiTexture) {
        CFFLog::Error("Skybox: Failed to load KTX2 cubemap from %s", ktx2Path.c_str());
        return false;
    }

    m_envTexture.reset(rhiTexture);

    // Create cube mesh
    createCubeMesh();

    // Create shaders
    createShaders();

    // Create pipeline state
    createPipelineState();

    // Create constant buffer
    createConstantBuffer();

    // Create sampler
    createSampler();

    // Initialize descriptor sets (DX12)
    initDescriptorSets();

    m_initialized = true;
    CFFLog::Info("Skybox: Initialized from KTX2 (%dx%d)", m_envTexture->GetWidth(), m_envTexture->GetHeight());

    return true;
}

void CSkybox::Shutdown() {
    // Clean up descriptor set resources
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        m_perPassSet.reset();
        if (m_perPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_perPassLayout);
            m_perPassLayout = nullptr;
        }
        // Clean up conversion pass descriptor set resources
        m_convSet.reset();
        if (m_convLayout) {
            ctx->DestroyDescriptorSetLayout(m_convLayout);
            m_convLayout = nullptr;
        }
    }
    m_pso_ds.reset();
    m_vs_ds.reset();
    m_ps_ds.reset();
    m_convPSO_ds.reset();
    m_convVS_ds.reset();
    m_convPS_ds.reset();

    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_vertexBuffer.reset();
    m_indexBuffer.reset();
    m_constantBuffer.reset();
    m_sampler.reset();
    m_envTexture.reset();
    m_initialized = false;
}

void CSkybox::Render(const XMMATRIX& view, const XMMATRIX& proj) {
    if (!m_initialized || !m_envTexture) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // Remove translation from view matrix
    XMMATRIX viewNoTranslation = view;
    viewNoTranslation.r[3] = XMVectorSet(0, 0, 0, 1);

    // Update constant buffer
    CB_SkyboxTransform cb;
    cb.viewProj = XMMatrixTranspose(viewNoTranslation * proj);
    cb.useReversedZ = UseReversedZ() ? 1 : 0;
    cb.padding[0] = cb.padding[1] = cb.padding[2] = 0;

    // Use descriptor set path if available (DX12)
    if (m_pso_ds && m_perPassSet) {
        cmdList->SetPipelineState(m_pso_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

        // Set vertex and index buffers
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SkyboxVertex), 0);
        cmdList->SetIndexBuffer(m_indexBuffer.get(), EIndexFormat::UInt32, 0);

        // Bind descriptor set with volatile CBV, texture, and sampler
        m_perPassSet->Bind({
            BindingSetItem::VolatileCBV(0, &cb, sizeof(cb)),
            BindingSetItem::Texture_SRV(0, m_envTexture.get()),
            BindingSetItem::Sampler(0, m_sampler.get()),
        });
        cmdList->BindDescriptorSet(1, m_perPassSet.get());

        // Draw
        cmdList->DrawIndexed(m_indexCount, 0, 0);
        return;
    }

    // Fallback: descriptor set path not available
    CFFLog::Warning("[Skybox] Descriptor set path not available, skipping render");
}

void CSkybox::createCubeMesh() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Cube vertices (positions only)
    SkyboxVertex vertices[] = {
        // Front face
        {{-1.0f, -1.0f, -1.0f}}, {{ 1.0f, -1.0f, -1.0f}}, {{ 1.0f,  1.0f, -1.0f}}, {{-1.0f,  1.0f, -1.0f}},
        // Back face
        {{-1.0f, -1.0f,  1.0f}}, {{ 1.0f, -1.0f,  1.0f}}, {{ 1.0f,  1.0f,  1.0f}}, {{-1.0f,  1.0f,  1.0f}},
        // Left face
        {{-1.0f, -1.0f, -1.0f}}, {{-1.0f,  1.0f, -1.0f}}, {{-1.0f,  1.0f,  1.0f}}, {{-1.0f, -1.0f,  1.0f}},
        // Right face
        {{ 1.0f, -1.0f, -1.0f}}, {{ 1.0f,  1.0f, -1.0f}}, {{ 1.0f,  1.0f,  1.0f}}, {{ 1.0f, -1.0f,  1.0f}},
        // Top face
        {{-1.0f,  1.0f, -1.0f}}, {{ 1.0f,  1.0f, -1.0f}}, {{ 1.0f,  1.0f,  1.0f}}, {{-1.0f,  1.0f,  1.0f}},
        // Bottom face
        {{-1.0f, -1.0f, -1.0f}}, {{ 1.0f, -1.0f, -1.0f}}, {{ 1.0f, -1.0f,  1.0f}}, {{-1.0f, -1.0f,  1.0f}}
    };

    uint32_t indices[] = {
        0, 1, 2,  0, 2, 3,    // Front
        4, 6, 5,  4, 7, 6,    // Back
        8, 9,10,  8,10,11,    // Left
        12,14,13, 12,15,14,   // Right
        16,17,18, 16,18,19,   // Top
        20,22,21, 20,23,22    // Bottom
    };

    m_indexCount = 36;

    // Create vertex buffer
    BufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.usage = EBufferUsage::Vertex;
    vbDesc.cpuAccess = ECPUAccess::None;
    m_vertexBuffer.reset(ctx->CreateBuffer(vbDesc, vertices));

    // Create index buffer
    BufferDesc ibDesc;
    ibDesc.size = sizeof(indices);
    ibDesc.usage = EBufferUsage::Index;
    ibDesc.cpuAccess = ECPUAccess::None;
    m_indexBuffer.reset(ctx->CreateBuffer(ibDesc, indices));
}

void CSkybox::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Load shader source from files (paths relative to E:\forfun\assets working directory)
    std::string vsSource = LoadShaderSource("../source/code/Shader/Skybox.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/Skybox.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load Skybox shader files!");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile Vertex Shader
    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("=== SKYBOX VERTEX SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", vsCompiled.errorMessage.c_str());
        return;
    }

    // Compile Pixel Shader
    SCompiledShader psCompiled = CompileShaderFromSource(psSource, "main", "ps_5_0", nullptr, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("=== SKYBOX PIXEL SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", psCompiled.errorMessage.c_str());
        return;
    }

    // Create shader objects using RHI
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    m_vs.reset(ctx->CreateShader(vsDesc));

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    m_ps.reset(ctx->CreateShader(psDesc));
}

void CSkybox::createPipelineState() {
    if (!m_vs || !m_ps) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();

    // Input layout: POSITION only
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 }
    };

    // Rasterizer state: no culling for skybox
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.depthClipEnable = true;

    // Depth stencil state: depth test but no write (skybox at far plane)
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual

    // Blend state: no blending
    psoDesc.blend.blendEnable = false;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    // Render target format: HDR R16G16B16A16_FLOAT for skybox
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };

    // Depth stencil format (D32_FLOAT to match GBuffer depth for deferred rendering)
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;
    psoDesc.debugName = "Skybox_PSO";

    m_pso.reset(ctx->CreatePipelineState(psoDesc));
}

void CSkybox::createConstantBuffer() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_SkyboxTransform);
    cbDesc.usage = EBufferUsage::Constant;
    cbDesc.cpuAccess = ECPUAccess::Write;
    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));
}

void CSkybox::createSampler() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Wrap;
    samplerDesc.addressV = ETextureAddressMode::Wrap;
    samplerDesc.addressW = ETextureAddressMode::Wrap;

    m_sampler.reset(ctx->CreateSampler(samplerDesc));
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CSkybox::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx || ctx->GetBackend() != EBackend::DX12) return;

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile SM 5.1 shaders
    std::string vsSource = LoadShaderSource(shaderDir + "Skybox_DS.vs.hlsl");
    std::string psSource = LoadShaderSource(shaderDir + "Skybox_DS.ps.hlsl");
    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Warning("[Skybox] Failed to load DS shaders");
        return;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_1", nullptr, debugShaders);
    if (!vsCompiled.success || !psCompiled.success) {
        CFFLog::Error("[Skybox] DS shader compile error: %s %s",
                      vsCompiled.errorMessage.c_str(), psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc(EShaderType::Vertex, vsCompiled.bytecode.data(), vsCompiled.bytecode.size());
    vsDesc.debugName = "Skybox_DS_VS";
    ShaderDesc psDesc(EShaderType::Pixel, psCompiled.bytecode.data(), psCompiled.bytecode.size());
    psDesc.debugName = "Skybox_DS_PS";
    m_vs_ds.reset(ctx->CreateShader(vsDesc));
    m_ps_ds.reset(ctx->CreateShader(psDesc));

    if (!m_vs_ds || !m_ps_ds) {
        CFFLog::Error("[Skybox] Failed to create DS shaders");
        return;
    }

    // Create PerPass layout (Set 1): CBV + SRV + Sampler
    BindingLayoutDesc layoutDesc("Skybox_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_SkyboxTransform)));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));
    layoutDesc.AddItem(BindingLayoutItem::Sampler(0));

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[Skybox] Failed to create descriptor set layout");
        return;
    }

    auto* set = ctx->AllocateDescriptorSet(m_perPassLayout);
    m_perPassSet = std::unique_ptr<IDescriptorSet, std::function<void(IDescriptorSet*)>>(
        set, [ctx](IDescriptorSet* s) { ctx->FreeDescriptorSet(s); });

    if (!m_perPassSet) {
        CFFLog::Error("[Skybox] Failed to allocate descriptor set");
        return;
    }

    // Create PSO with descriptor set layout
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs_ds.get();
    psoDesc.pixelShader = m_ps_ds.get();
    psoDesc.inputLayout = {{ EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 }};
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.depthClipEnable = true;
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);
    psoDesc.blend.blendEnable = false;
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;
    psoDesc.setLayouts[1] = m_perPassLayout;
    psoDesc.debugName = "Skybox_DS_PSO";

    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));
    if (!m_pso_ds) {
        CFFLog::Error("[Skybox] Failed to create DS PSO");
        return;
    }

    CFFLog::Info("[Skybox] Descriptor set path initialized");
}

// ============================================
// HDR to Cubemap Conversion using RHI
// ============================================

void CSkybox::convertEquirectToCubemapLegacy(const std::string& hdrPath, int size) {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();
    if (!ctx || !cmdList) return;

    // Load HDR file
    SHdrImage hdrImage;
    if (!LoadHdrFile(hdrPath, hdrImage)) {
        CFFLog::Error("Skybox: Failed to load HDR file: %s", hdrPath.c_str());
        return;
    }

    // Convert RGB to RGBA
    std::vector<float> rgba(hdrImage.width * hdrImage.height * 4);
    for (int i = 0; i < hdrImage.width * hdrImage.height; ++i) {
        rgba[i * 4 + 0] = hdrImage.data[i * 3 + 0];
        rgba[i * 4 + 1] = hdrImage.data[i * 3 + 1];
        rgba[i * 4 + 2] = hdrImage.data[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }

    // Create equirectangular texture
    TextureDesc equirectDesc;
    equirectDesc.width = hdrImage.width;
    equirectDesc.height = hdrImage.height;
    equirectDesc.mipLevels = 1;
    equirectDesc.format = ETextureFormat::R32G32B32A32_FLOAT;
    equirectDesc.usage = ETextureUsage::ShaderResource;
    equirectDesc.debugName = "Skybox_EquirectHDR";

    TexturePtr equirectTexture(ctx->CreateTexture(equirectDesc, rgba.data()));
    if (!equirectTexture) {
        CFFLog::Error("Skybox: Failed to create equirectangular texture");
        return;
    }

    // Create cubemap texture with mipmaps (mipLevels = 0 for auto-generate)
    TextureDesc cubeDesc;
    cubeDesc.width = size;
    cubeDesc.height = size;
    cubeDesc.mipLevels = 0;  // Auto-generate full mip chain
    cubeDesc.arraySize = 1;
    cubeDesc.format = ETextureFormat::R16G16B16A16_FLOAT;
    cubeDesc.usage = ETextureUsage::RenderTarget | ETextureUsage::ShaderResource;
    cubeDesc.isCubemap = true;
    cubeDesc.debugName = "Skybox_EnvCubemap";

    m_envTexture.reset(ctx->CreateTexture(cubeDesc, nullptr));
    if (!m_envTexture) {
        CFFLog::Error("Skybox: Failed to create cubemap texture");
        return;
    }

    // Create sampler for conversion
    SamplerDesc convSamplerDesc;
    convSamplerDesc.filter = EFilter::MinMagMipLinear;
    convSamplerDesc.addressU = ETextureAddressMode::Clamp;
    convSamplerDesc.addressV = ETextureAddressMode::Clamp;
    convSamplerDesc.addressW = ETextureAddressMode::Clamp;
    SamplerPtr convSampler(ctx->CreateSampler(convSamplerDesc));

    // Create temporary cube mesh for conversion
    float vertices[] = {
        -1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1,  // Front
        -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,  // Back
        -1,-1,-1, -1, 1,-1, -1, 1, 1, -1,-1, 1,  // Left
         1,-1,-1,  1, 1,-1,  1, 1, 1,  1,-1, 1,  // Right
        -1, 1,-1,  1, 1,-1,  1, 1, 1, -1, 1, 1,  // Top
        -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1   // Bottom
    };
    uint32_t indices[] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  8,9,10, 8,10,11,
        12,14,13, 12,15,14,  16,17,18, 16,18,19,  20,22,21, 20,23,22
    };

    BufferDesc tempVbDesc;
    tempVbDesc.size = sizeof(vertices);
    tempVbDesc.usage = EBufferUsage::Vertex;
    BufferPtr tempVB(ctx->CreateBuffer(tempVbDesc, vertices));

    BufferDesc tempIbDesc;
    tempIbDesc.size = sizeof(indices);
    tempIbDesc.usage = EBufferUsage::Index;
    BufferPtr tempIB(ctx->CreateBuffer(tempIbDesc, indices));

    // Setup capture views for each cubemap face
    XMMATRIX captureProjection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, 10.0f);
    XMMATRIX captureViews[] = {
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 1, 0, 0,1), XMVectorSet(0, 1, 0,1)),  // +X
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet(-1, 0, 0,1), XMVectorSet(0, 1, 0,1)),  // -X
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0, 1, 0,1), XMVectorSet(0, 0,-1,1)),  // +Y
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0,-1, 0,1), XMVectorSet(0, 0, 1,1)),  // -Y
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0, 0, 1,1), XMVectorSet(0, 1, 0,1)),  // +Z
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0, 0,-1,1), XMVectorSet(0, 1, 0,1))   // -Z
    };

    // Use descriptor set path if available (DX12)
    if (ctx->GetBackend() == EBackend::DX12) {
        std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
        bool debugShaders = true;
#else
        bool debugShaders = false;
#endif

        // Compile SM 5.1 shaders for conversion
        std::string convVsSource = LoadShaderSource(shaderDir + "EquirectToCubemap_DS.vs.hlsl");
        std::string convPsSource = LoadShaderSource(shaderDir + "EquirectToCubemap_DS.ps.hlsl");
        if (convVsSource.empty() || convPsSource.empty()) {
            CFFLog::Error("[Skybox] Failed to load EquirectToCubemap_DS shaders");
            return;
        }

        SCompiledShader convVsCompiled = CompileShaderFromSource(convVsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
        SCompiledShader convPsCompiled = CompileShaderFromSource(convPsSource.c_str(), "main", "ps_5_1", nullptr, debugShaders);
        if (!convVsCompiled.success || !convPsCompiled.success) {
            CFFLog::Error("[Skybox] EquirectToCubemap_DS shader compile error: %s %s",
                          convVsCompiled.errorMessage.c_str(), convPsCompiled.errorMessage.c_str());
            return;
        }

        ShaderDesc convVsDesc(EShaderType::Vertex, convVsCompiled.bytecode.data(), convVsCompiled.bytecode.size());
        convVsDesc.debugName = "Skybox_Conv_DS_VS";
        ShaderDesc convPsDesc(EShaderType::Pixel, convPsCompiled.bytecode.data(), convPsCompiled.bytecode.size());
        convPsDesc.debugName = "Skybox_Conv_DS_PS";
        m_convVS_ds.reset(ctx->CreateShader(convVsDesc));
        m_convPS_ds.reset(ctx->CreateShader(convPsDesc));

        if (!m_convVS_ds || !m_convPS_ds) {
            CFFLog::Error("[Skybox] Failed to create conversion DS shaders");
            return;
        }

        // Create conversion layout (Set 1): VolatileCBV + SRV + Sampler
        BindingLayoutDesc convLayoutDesc("Skybox_Conv");
        convLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(XMMATRIX)));
        convLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));
        convLayoutDesc.AddItem(BindingLayoutItem::Sampler(0));

        m_convLayout = ctx->CreateDescriptorSetLayout(convLayoutDesc);
        if (!m_convLayout) {
            CFFLog::Error("[Skybox] Failed to create conversion descriptor set layout");
            return;
        }

        auto* convSet = ctx->AllocateDescriptorSet(m_convLayout);
        m_convSet = std::unique_ptr<IDescriptorSet, std::function<void(IDescriptorSet*)>>(
            convSet, [ctx](IDescriptorSet* s) { ctx->FreeDescriptorSet(s); });

        if (!m_convSet) {
            CFFLog::Error("[Skybox] Failed to allocate conversion descriptor set");
            return;
        }

        // Create conversion PSO with descriptor set layout
        PipelineStateDesc convPsoDesc;
        convPsoDesc.vertexShader = m_convVS_ds.get();
        convPsoDesc.pixelShader = m_convPS_ds.get();
        convPsoDesc.inputLayout = {{ EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 }};
        convPsoDesc.rasterizer.cullMode = ECullMode::None;
        convPsoDesc.rasterizer.fillMode = EFillMode::Solid;
        convPsoDesc.depthStencil.depthEnable = false;
        convPsoDesc.blend.blendEnable = false;
        convPsoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
        convPsoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        convPsoDesc.depthStencilFormat = ETextureFormat::Unknown;
        convPsoDesc.setLayouts[1] = m_convLayout;
        convPsoDesc.debugName = "Skybox_Conv_DS_PSO";

        m_convPSO_ds.reset(ctx->CreatePipelineState(convPsoDesc));
        if (!m_convPSO_ds) {
            CFFLog::Error("[Skybox] Failed to create conversion DS PSO");
            return;
        }

        // Render to each cubemap face using descriptor sets
        for (int face = 0; face < 6; ++face) {
            // Set render target to this face
            cmdList->SetRenderTargetSlice(m_envTexture.get(), face, nullptr);

            // Set viewport and scissor rect (DX12 requires both)
            cmdList->SetViewport(0, 0, (float)size, (float)size, 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, size, size);

            // Clear
            float clearColor[] = { 0, 0, 0, 1 };
            cmdList->ClearRenderTarget(m_envTexture.get(), clearColor);

            // Update transform constant buffer
            XMMATRIX vp_mat = XMMatrixTranspose(captureViews[face] * captureProjection);

            // Set pipeline state and resources
            cmdList->SetPipelineState(m_convPSO_ds.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);
            cmdList->SetVertexBuffer(0, tempVB.get(), 12, 0);  // 12 = 3 floats * 4 bytes
            cmdList->SetIndexBuffer(tempIB.get(), EIndexFormat::UInt32, 0);

            // Bind descriptor set with volatile CBV, texture, and sampler
            m_convSet->Bind({
                BindingSetItem::VolatileCBV(0, &vp_mat, sizeof(XMMATRIX)),
                BindingSetItem::Texture_SRV(0, equirectTexture.get()),
                BindingSetItem::Sampler(0, convSampler.get()),
            });
            cmdList->BindDescriptorSet(1, m_convSet.get());

            // Draw
            cmdList->DrawIndexed(36, 0, 0);
        }
    } else {
        // DX11 fallback - legacy binding not supported
        CFFLog::Error("[Skybox] HDR conversion requires DX12 backend (legacy binding disabled)");
        return;
    }

    // Unbind render target
    cmdList->UnbindRenderTargets();

    CFFLog::Info("Skybox: Environment cubemap ready (%dx%d with mipmaps)", size, size);
}
