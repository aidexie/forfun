#include "Skybox.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "Core/Loader/HdrLoader.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/FFLog.h"
#include <d3dcompiler.h>
#include <vector>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
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
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return false;

    // Convert equirectangular HDR to cubemap
    convertEquirectToCubemap(hdrPath, cubemapSize);

    // Create cube mesh
    createCubeMesh();

    // Create shaders
    createShaders();

    // Create constant buffer
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CB_SkyboxTransform);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cbd, nullptr, m_cbTransform.GetAddressOf());

    // Create sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, m_sampler.GetAddressOf());

    // Create rasterizer state (no culling for skybox)
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, m_rasterState.GetAddressOf());

    // Create depth stencil state (depth test but no write)
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // No depth write
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;  // Draw at far plane
    device->CreateDepthStencilState(&dsd, m_depthState.GetAddressOf());

    return true;
}

bool CSkybox::InitializeFromKTX2(const std::string& ktx2Path) {
    m_envPathKTX2 = ktx2Path;
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return false;

    // Load cubemap from KTX2 (returns RHI texture with SRV)
    RHI::ITexture* rhiTexture = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!rhiTexture) {
        CFFLog::Error("Skybox: Failed to load KTX2 cubemap from %s", ktx2Path.c_str());
        return false;
    }

    // Get native D3D11 resources from RHI texture
    // Note: The RHI texture owns these resources, we just borrow the pointers
    ID3D11Texture2D* texture = static_cast<ID3D11Texture2D*>(rhiTexture->GetNativeHandle());
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(rhiTexture->GetSRV());

    // Store the RHI texture to keep it alive
    m_rhiEnvTexture.reset(rhiTexture);

    // Store the native pointers for rendering
    m_envTexture = texture;
    m_envCubemap = srv;

    // Create cube mesh
    createCubeMesh();

    // Create shaders
    createShaders();

    // Create constant buffer
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CB_SkyboxTransform);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT hr = device->CreateBuffer(&cbd, nullptr, m_cbTransform.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Skybox: Failed to create constant buffer");
        return false;
    }

    // Create sampler
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&samplerDesc, m_sampler.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Skybox: Failed to create sampler");
        return false;
    }

    // Create rasterizer state (no culling for skybox)
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    hr = device->CreateRasterizerState(&rasterDesc, m_rasterState.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Skybox: Failed to create rasterizer state");
        return false;
    }

    // Create depth stencil state (skybox rendered at max depth)
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device->CreateDepthStencilState(&depthDesc, m_depthState.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Skybox: Failed to create depth stencil state");
        return false;
    }

    CFFLog::Info("Skybox: Initialized from KTX2 (%dx%d)", m_rhiEnvTexture->GetWidth(), m_rhiEnvTexture->GetHeight());

    return true;
}

void CSkybox::Shutdown() {
    // Release RHI texture (which owns the D3D11 resources if loaded from KTX2)
    m_rhiEnvTexture.reset();
    m_envTexture = nullptr;
    m_envCubemap = nullptr;

    // Release owned resources (HDR path)
    m_ownedEnvTexture.Reset();
    m_ownedEnvCubemap.Reset();

    // Release rendering resources
    m_vs.Reset();
    m_ps.Reset();
    m_inputLayout.Reset();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_cbTransform.Reset();
    m_sampler.Reset();
    m_rasterState.Reset();
    m_depthState.Reset();
}

void CSkybox::Render(const XMMATRIX& view, const XMMATRIX& proj) {
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());
    if (!context || !m_envCubemap) return;

    // Remove translation from view matrix
    XMMATRIX viewNoTranslation = view;
    viewNoTranslation.r[3] = XMVectorSet(0, 0, 0, 1);

    // Update constant buffer
    CB_SkyboxTransform cb;
    cb.viewProj = XMMatrixTranspose(viewNoTranslation * proj);
    context->UpdateSubresource(m_cbTransform.Get(), 0, nullptr, &cb, 0, 0);

    // Set render states
    context->RSSetState(m_rasterState.Get());
    context->OMSetDepthStencilState(m_depthState.Get(), 0);

    // Set pipeline
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(SkyboxVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_cbTransform.GetAddressOf());

    context->PSSetShaderResources(0, 1, &m_envCubemap);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Draw
    context->DrawIndexed(m_indexCount, 0, 0);

    // Unbind resources
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}

void CSkybox::convertEquirectToCubemap(const std::string& hdrPath, int size) {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());
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
    cubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;  // Enable mipmap generation
    device->CreateTexture2D(&cubeDesc, nullptr, m_ownedEnvTexture.GetAddressOf());

    // Create SRV for cubemap (all mip levels)
    D3D11_SHADER_RESOURCE_VIEW_DESC cubeSRVDesc{};
    cubeSRVDesc.Format = cubeDesc.Format;
    cubeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cubeSRVDesc.TextureCube.MipLevels = -1;  // -1 = all mip levels
    device->CreateShaderResourceView(m_ownedEnvTexture.Get(), &cubeSRVDesc, m_ownedEnvCubemap.GetAddressOf());

    // Set raw pointers for rendering
    m_envTexture = m_ownedEnvTexture.Get();
    m_envCubemap = m_ownedEnvCubemap.Get();

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
        device->CreateRenderTargetView(m_ownedEnvTexture.Get(), &rtvDesc, rtv.GetAddressOf());

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
        context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

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
    m_ownedEnvTexture->GetDesc(&finalDesc);
    int mipCount = finalDesc.MipLevels;

    CFFLog::Info("Skybox: Generating mipmaps for %dx%d cubemap (%d levels)...", size, size, mipCount);

    // Generate mipmaps for the cubemap
    context->GenerateMips(m_ownedEnvCubemap.Get());

    CFFLog::Info("Skybox: Environment cubemap ready (%dx%d, %d mip levels)", size, size, mipCount);
}

void CSkybox::createCubeMesh() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return;

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
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(vertices);
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{ vertices, 0, 0 };
    device->CreateBuffer(&vbd, &vbData, m_vertexBuffer.GetAddressOf());

    // Create index buffer
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = sizeof(indices);
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData{ indices, 0, 0 };
    device->CreateBuffer(&ibd, &ibData, m_indexBuffer.GetAddressOf());
}

void CSkybox::createShaders() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return;

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

    ComPtr<ID3DBlob> vsBlob, psBlob, err;

    // Compile Vertex Shader
    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "Skybox.vs.hlsl", nullptr, nullptr,
                           "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== SKYBOX VERTEX SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
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
        }
        return;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    device->CreateInputLayout(layout, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());
}
