#include "Skybox.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "Core/Loader/HdrLoader.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/FFLog.h"
#include <d3dcompiler.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using namespace RHI;
using Microsoft::WRL::ComPtr;

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
};

bool CSkybox::Initialize(const std::string& hdrPath, int cubemapSize) {
    if (m_initialized) return true;

    // Convert equirectangular HDR to cubemap (legacy D3D11 path)
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

    void* mappedData = m_constantBuffer->Map();
    if (mappedData) {
        memcpy(mappedData, &cb, sizeof(CB_SkyboxTransform));
        m_constantBuffer->Unmap();
    }

    // Set pipeline state (includes rasterizer, depth stencil, blend states)
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Set vertex and index buffers
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SkyboxVertex), 0);
    cmdList->SetIndexBuffer(m_indexBuffer.get(), EIndexFormat::UInt32, 0);

    // Set constant buffer
    cmdList->SetConstantBuffer(EShaderStage::Vertex, 0, m_constantBuffer.get());

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

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err = nullptr;

    // Compile Vertex Shader
    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "Skybox.vs.hlsl", nullptr, nullptr,
                           "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== SKYBOX VERTEX SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
            err->Release();
        }
        return;
    }

    // Compile Pixel Shader
    hr = D3DCompile(psSource.c_str(), psSource.size(), "Skybox.ps.hlsl", nullptr, nullptr,
                   "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== SKYBOX PIXEL SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
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
    psoDesc.depthStencil.depthFunc = EComparisonFunc::LessEqual;

    // Blend state: no blending
    psoDesc.blend.blendEnable = false;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

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
// Legacy D3D11 Path: HDR to Cubemap Conversion
// This will be migrated in Phase 6 when per-slice RTV support is added
// ============================================

void CSkybox::convertEquirectToCubemapLegacy(const std::string& hdrPath, int size) {
    ID3D11Device* device = static_cast<ID3D11Device*>(CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(CRHIManager::Instance().GetRenderContext()->GetNativeContext());
    if (!device || !context) return;

    // Load HDR file
    SHdrImage hdrImage;
    if (!LoadHdrFile(hdrPath, hdrImage)) {
        return;
    }

    // Create equirectangular texture
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = hdrImage.width;
    texDesc.Height = hdrImage.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Convert RGB to RGBA
    std::vector<float> rgba(hdrImage.width * hdrImage.height * 4);
    for (int i = 0; i < hdrImage.width * hdrImage.height; ++i) {
        rgba[i * 4 + 0] = hdrImage.data[i * 3 + 0];
        rgba[i * 4 + 1] = hdrImage.data[i * 3 + 1];
        rgba[i * 4 + 2] = hdrImage.data[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = rgba.data();
    initData.SysMemPitch = hdrImage.width * sizeof(float) * 4;

    ComPtr<ID3D11Texture2D> equirectTexture;
    device->CreateTexture2D(&texDesc, &initData, equirectTexture.GetAddressOf());

    ComPtr<ID3D11ShaderResourceView> equirectSRV;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(equirectTexture.Get(), &srvDesc, equirectSRV.GetAddressOf());

    // Create cubemap texture with mipmaps
    D3D11_TEXTURE2D_DESC cubeDesc{};
    cubeDesc.Width = size;
    cubeDesc.Height = size;
    cubeDesc.MipLevels = 0;  // 0 = auto-generate full mip chain
    cubeDesc.ArraySize = 6;
    cubeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cubeDesc.SampleDesc.Count = 1;
    cubeDesc.Usage = D3D11_USAGE_DEFAULT;
    cubeDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    cubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> cubeTexture;
    device->CreateTexture2D(&cubeDesc, nullptr, cubeTexture.GetAddressOf());

    // Create SRV for cubemap (all mip levels)
    D3D11_SHADER_RESOURCE_VIEW_DESC cubeSRVDesc{};
    cubeSRVDesc.Format = cubeDesc.Format;
    cubeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cubeSRVDesc.TextureCube.MipLevels = -1;  // -1 = all mip levels

    ComPtr<ID3D11ShaderResourceView> cubeSRV;
    device->CreateShaderResourceView(cubeTexture.Get(), &cubeSRVDesc, cubeSRV.GetAddressOf());

    // Load conversion shaders from files
    std::string convVsSource = LoadShaderSource("../source/code/Shader/EquirectToCubemap.vs.hlsl");
    std::string convPsSource = LoadShaderSource("../source/code/Shader/EquirectToCubemap.ps.hlsl");

    if (convVsSource.empty() || convPsSource.empty()) {
        CFFLog::Error("Failed to load EquirectToCubemap shader files!");
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> vsBlob, psBlob, err;

    // Compile conversion vertex shader
    HRESULT hr = D3DCompile(convVsSource.c_str(), convVsSource.size(), "EquirectToCubemap.vs.hlsl",
                           nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== EQUIRECT CONVERSION VS COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Compile conversion pixel shader
    hr = D3DCompile(convPsSource.c_str(), convPsSource.size(), "EquirectToCubemap.ps.hlsl",
                   nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== EQUIRECT CONVERSION PS COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    ComPtr<ID3D11VertexShader> convVS;
    ComPtr<ID3D11PixelShader> convPS;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, convVS.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, convPS.GetAddressOf());

    // Create sampler for conversion
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> convSampler;
    device->CreateSamplerState(&sd, convSampler.GetAddressOf());

    // Render to each cubemap face
    XMMATRIX captureProjection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, 10.0f);
    XMMATRIX captureViews[] = {
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 1, 0, 0,1), XMVectorSet(0, 1, 0,1)),  // +X
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet(-1, 0, 0,1), XMVectorSet(0, 1, 0,1)),  // -X
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0, 1, 0,1), XMVectorSet(0, 0,-1,1)),  // +Y
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0,-1, 0,1), XMVectorSet(0, 0, 1,1)),  // -Y
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0, 0, 1,1), XMVectorSet(0, 1, 0,1)),  // +Z
        XMMatrixLookAtLH(XMVectorSet(0,0,0,1), XMVectorSet( 0, 0,-1,1), XMVectorSet(0, 1, 0,1))   // -Z
    };

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

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{ vertices, 0, 0 };
    ComPtr<ID3D11Buffer> tempVB;
    device->CreateBuffer(&vbDesc, &vbData, tempVB.GetAddressOf());

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData{ indices, 0, 0 };
    ComPtr<ID3D11Buffer> tempIB;
    device->CreateBuffer(&ibDesc, &ibData, tempIB.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ComPtr<ID3D11InputLayout> tempLayout;
    device->CreateInputLayout(layout, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), tempLayout.GetAddressOf());

    ComPtr<ID3D11Buffer> tempCB;
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(XMMATRIX);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cbDesc, nullptr, tempCB.GetAddressOf());

    // Render each face
    for (int face = 0; face < 6; ++face) {
        // Create RTV for this face
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = cubeDesc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.ArraySize = 1;

        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateRenderTargetView(cubeTexture.Get(), &rtvDesc, rtv.GetAddressOf());

        // Set render target
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        // Set viewport
        D3D11_VIEWPORT vp{};
        vp.Width = (float)size;
        vp.Height = (float)size;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        // Clear
        float clearColor[] = { 0, 0, 0, 1 };
        context->ClearRenderTargetView(rtv.Get(), clearColor);

        // Update transform
        XMMATRIX vp_mat = XMMatrixTranspose(captureViews[face] * captureProjection);
        context->UpdateSubresource(tempCB.Get(), 0, nullptr, &vp_mat, 0, 0);

        // Set pipeline
        context->IASetInputLayout(tempLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = 12, offset = 0;
        context->IASetVertexBuffers(0, 1, tempVB.GetAddressOf(), &stride, &offset);
        context->IASetIndexBuffer(tempIB.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(convVS.Get(), nullptr, 0);
        context->PSSetShader(convPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, tempCB.GetAddressOf());
        context->PSSetShaderResources(0, 1, equirectSRV.GetAddressOf());
        context->PSSetSamplers(0, 1, convSampler.GetAddressOf());

        // Draw
        context->DrawIndexed(36, 0, 0);
    }

    // Unbind resources
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);

    // Calculate mip level count before generation
    D3D11_TEXTURE2D_DESC finalDesc;
    cubeTexture->GetDesc(&finalDesc);
    int mipCount = finalDesc.MipLevels;

    CFFLog::Info("Skybox: Generating mipmaps for %dx%d cubemap (%d levels)...", size, size, mipCount);

    // Generate mipmaps for the cubemap
    context->GenerateMips(cubeSRV.Get());

    CFFLog::Info("Skybox: Environment cubemap ready (%dx%d, %d mip levels)", size, size, mipCount);

    // Wrap the D3D11 texture in RHI (transfers ownership)
    IRenderContext* rhiCtx = CRHIManager::Instance().GetRenderContext();
    m_envTexture.reset(rhiCtx->WrapNativeTexture(
        cubeTexture.Detach(),
        cubeSRV.Detach(),
        size,
        size,
        ETextureFormat::R16G16B16A16_FLOAT
    ));
}
