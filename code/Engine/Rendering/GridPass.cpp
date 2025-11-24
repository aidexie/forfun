// Engine/Rendering/GridPass.cpp
#include "GridPass.h"
#include "Core/DX11Context.h"
#include "Editor/DiagnosticLog.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Helper function to load shader source from file
static std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CDiagnosticLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

CGridPass& CGridPass::Instance() {
    static CGridPass instance;
    return instance;
}

void CGridPass::Initialize() {
    if (m_initialized) return;

    CreateShaders();
    CreateBuffers();
    CreateStates();

    m_initialized = true;
}

void CGridPass::Shutdown() {
    m_vs.Reset();
    m_ps.Reset();
    m_cbPerFrame.Reset();
    m_blendState.Reset();
    m_depthState.Reset();
    m_samplerState.Reset();
    m_initialized = false;
}

void CGridPass::CreateShaders() {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Load shader source files
    std::string vsSource = LoadShaderSource("../source/code/Shader/Grid.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/Grid.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CDiagnosticLog::Error("Failed to load Grid shader files!");
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> vsBlob, psBlob, err;

    // Compile Vertex Shader
    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "Grid.vs.hlsl",
                           nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CDiagnosticLog::Error("=== GRID VERTEX SHADER COMPILATION ERROR ===");
            CDiagnosticLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Compile Pixel Shader
    hr = D3DCompile(psSource.c_str(), psSource.size(), "Grid.ps.hlsl",
                   nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CDiagnosticLog::Error("=== GRID PIXEL SHADER COMPILATION ERROR ===");
            CDiagnosticLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Create shader objects
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());
}

void CGridPass::CreateBuffers() {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(CBPerFrame);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cbDesc, nullptr, m_cbPerFrame.GetAddressOf());
}

void CGridPass::CreateStates() {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Blend State: Alpha blending
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blendDesc, m_blendState.GetAddressOf());

    // Depth Stencil State: Read depth but don't write
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // Don't write depth
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    depthDesc.StencilEnable = FALSE;
    device->CreateDepthStencilState(&depthDesc, m_depthState.GetAddressOf());

    // Sampler State: Point sampling for depth texture
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    device->CreateSamplerState(&samplerDesc, m_samplerState.GetAddressOf());
}

void CGridPass::Render(XMMATRIX view, XMMATRIX proj, XMFLOAT3 cameraPos,
                       ID3D11ShaderResourceView* depthSRV,
                       UINT viewportWidth, UINT viewportHeight) {
    if (!m_initialized || !m_enabled || !depthSRV) return;

    ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
    if (!context) return;

    // Update constant buffer
    XMMATRIX viewProj = view * proj;
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    CBPerFrame cb;
    cb.viewProj = XMMatrixTranspose(viewProj);       // HLSL expects column-major
    cb.invViewProj = XMMatrixTranspose(invViewProj);
    cb.cameraPos = cameraPos;
    cb.fadeStart = m_fadeStart;
    cb.fadeEnd = m_fadeEnd;
    cb.padding = XMFLOAT3(0, 0, 0);

    context->UpdateSubresource(m_cbPerFrame.Get(), 0, nullptr, &cb, 0, 0);

    // Set pipeline state
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetInputLayout(nullptr);  // No input layout needed (vertex shader generates quad)

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_cbPerFrame.GetAddressOf());

    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, m_cbPerFrame.GetAddressOf());
    context->PSSetShaderResources(0, 1, &depthSRV);
    context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

    // Set blend state and depth stencil state
    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthState.Get(), 0);

    // Draw full-screen quad (4 vertices, triangle strip)
    context->Draw(4, 0);

    // Unbind resources
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);

    // Restore default blend/depth state
    context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
}
