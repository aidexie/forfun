#include "Skybox.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/Loader/HdrLoader.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
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

    m_initialized = true;
    CFFLog::Info("Skybox: Initialized from KTX2 (%dx%d)", m_envTexture->GetWidth(), m_envTexture->GetHeight());

    return true;
}

void CSkybox::Shutdown() {
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
    if (!m_initialized || !m_envTexture || !m_pso) return;

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

    // Set pipeline state (includes rasterizer, depth stencil, blend states)
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Set vertex and index buffers
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SkyboxVertex), 0);
    cmdList->SetIndexBuffer(m_indexBuffer.get(), EIndexFormat::UInt32, 0);

    // Set constant buffer (use SetConstantBufferData for DX12 compatibility)
    cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &cb, sizeof(CB_SkyboxTransform));

    // Set texture and sampler
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, m_envTexture.get());
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

    // Draw
    cmdList->DrawIndexed(m_indexCount, 0, 0);
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

    // Load conversion shaders
    std::string convVsSource = LoadShaderSource("../source/code/Shader/EquirectToCubemap.vs.hlsl");
    std::string convPsSource = LoadShaderSource("../source/code/Shader/EquirectToCubemap.ps.hlsl");

    if (convVsSource.empty() || convPsSource.empty()) {
        CFFLog::Error("Failed to load EquirectToCubemap shader files!");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader convVsCompiled = CompileShaderFromSource(convVsSource, "main", "vs_5_0", nullptr, debugShaders);
    if (!convVsCompiled.success) {
        CFFLog::Error("EquirectToCubemap VS compile error: %s", convVsCompiled.errorMessage.c_str());
        return;
    }

    SCompiledShader convPsCompiled = CompileShaderFromSource(convPsSource, "main", "ps_5_0", nullptr, debugShaders);
    if (!convPsCompiled.success) {
        CFFLog::Error("EquirectToCubemap PS compile error: %s", convPsCompiled.errorMessage.c_str());
        return;
    }

    // Create shaders
    ShaderDesc convVsDesc;
    convVsDesc.type = EShaderType::Vertex;
    convVsDesc.bytecode = convVsCompiled.bytecode.data();
    convVsDesc.bytecodeSize = convVsCompiled.bytecode.size();
    ShaderPtr convVS(ctx->CreateShader(convVsDesc));

    ShaderDesc convPsDesc;
    convPsDesc.type = EShaderType::Pixel;
    convPsDesc.bytecode = convPsCompiled.bytecode.data();
    convPsDesc.bytecodeSize = convPsCompiled.bytecode.size();
    ShaderPtr convPS(ctx->CreateShader(convPsDesc));

    // Create conversion pipeline state
    PipelineStateDesc convPsoDesc;
    convPsoDesc.vertexShader = convVS.get();
    convPsoDesc.pixelShader = convPS.get();
    convPsoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 }
    };
    convPsoDesc.rasterizer.cullMode = ECullMode::None;
    convPsoDesc.rasterizer.fillMode = EFillMode::Solid;
    convPsoDesc.depthStencil.depthEnable = false;
    convPsoDesc.blend.blendEnable = false;
    convPsoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
    convPsoDesc.debugName = "Skybox_HDRConvert_PSO";

    PipelineStatePtr convPso(ctx->CreatePipelineState(convPsoDesc));

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

    // Create constant buffer for view-projection matrix
    BufferDesc tempCbDesc;
    tempCbDesc.size = sizeof(XMMATRIX);
    tempCbDesc.usage = EBufferUsage::Constant;
    tempCbDesc.cpuAccess = ECPUAccess::Write;
    BufferPtr tempCB(ctx->CreateBuffer(tempCbDesc, nullptr));

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

    // Render to each cubemap face using RHI
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
        cmdList->SetPipelineState(convPso.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);
        cmdList->SetVertexBuffer(0, tempVB.get(), 12, 0);  // 12 = 3 floats * 4 bytes
        cmdList->SetIndexBuffer(tempIB.get(), EIndexFormat::UInt32, 0);
        cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &vp_mat, sizeof(XMMATRIX));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, equirectTexture.get());
        cmdList->SetSampler(EShaderStage::Pixel, 0, convSampler.get());

        // Draw
        cmdList->DrawIndexed(36, 0, 0);
    }

    // Unbind render target
    cmdList->UnbindRenderTargets();

    // // Generate mipmaps
    // cmdList->GenerateMips(m_envTexture.get());

    CFFLog::Info("Skybox: Environment cubemap ready (%dx%d with mipmaps)", size, size);
}
