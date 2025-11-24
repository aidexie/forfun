// Engine/Rendering/DebugLinePass.cpp
#include "DebugLinePass.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
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
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void CDebugLinePass::Initialize() {
    if (m_initialized) return;

    CreateShaders();
    CreateBuffers();

    m_initialized = true;
}

void CDebugLinePass::Shutdown() {
    m_vertexBuffer.Reset();
    m_cbPerFrameVS.Reset();
    m_cbPerFrameGS.Reset();
    m_vs.Reset();
    m_gs.Reset();
    m_ps.Reset();
    m_inputLayout.Reset();
    m_dynamicLines.clear();
    m_initialized = false;
}

void CDebugLinePass::CreateShaders() {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Load shader source files
    std::string vsSource = LoadShaderSource("../source/code/Shader/DebugLine.vs.hlsl");
    std::string gsSource = LoadShaderSource("../source/code/Shader/DebugLine.gs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/DebugLine.ps.hlsl");

    if (vsSource.empty() || gsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load DebugLine shader files!");
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> vsBlob, gsBlob, psBlob, err;

    // Compile Vertex Shader
    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "DebugLine.vs.hlsl",
                           nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== DEBUGLINE VERTEX SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Compile Geometry Shader
    hr = D3DCompile(gsSource.c_str(), gsSource.size(), "DebugLine.gs.hlsl",
                   nullptr, nullptr, "main", "gs_5_0", compileFlags, 0, &gsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== DEBUGLINE GEOMETRY SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Compile Pixel Shader
    hr = D3DCompile(psSource.c_str(), psSource.size(), "DebugLine.ps.hlsl",
                   nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== DEBUGLINE PIXEL SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Create shader objects
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    device->CreateGeometryShader(gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), nullptr, m_gs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    // Create Input Layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());
}

void CDebugLinePass::CreateBuffers() {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Create dynamic vertex buffer
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(LineVertex) * m_maxVertices;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&vbDesc, nullptr, m_vertexBuffer.GetAddressOf());

    // Create constant buffers
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    cbDesc.ByteWidth = sizeof(CBPerFrameVS);
    device->CreateBuffer(&cbDesc, nullptr, m_cbPerFrameVS.GetAddressOf());

    cbDesc.ByteWidth = sizeof(CBPerFrameGS);
    device->CreateBuffer(&cbDesc, nullptr, m_cbPerFrameGS.GetAddressOf());
}

void CDebugLinePass::BeginFrame() {
    m_dynamicLines.clear();
}

void CDebugLinePass::AddLine(XMFLOAT3 from, XMFLOAT3 to, XMFLOAT4 color) {
    if (m_dynamicLines.size() + 2 > m_maxVertices) {
        CFFLog::Warning("DebugLinePass vertex buffer overflow!");
        return;
    }

    m_dynamicLines.push_back({ from, color });
    m_dynamicLines.push_back({ to, color });
}

void CDebugLinePass::AddAABB(XMFLOAT3 localMin, XMFLOAT3 localMax,
                             XMMATRIX worldMatrix, XMFLOAT4 color) {
    // 8 corners of local AABB
    XMFLOAT3 corners[8] = {
        XMFLOAT3(localMin.x, localMin.y, localMin.z), // 0: left-bottom-front
        XMFLOAT3(localMax.x, localMin.y, localMin.z), // 1: right-bottom-front
        XMFLOAT3(localMax.x, localMax.y, localMin.z), // 2: right-top-front
        XMFLOAT3(localMin.x, localMax.y, localMin.z), // 3: left-top-front
        XMFLOAT3(localMin.x, localMin.y, localMax.z), // 4: left-bottom-back
        XMFLOAT3(localMax.x, localMin.y, localMax.z), // 5: right-bottom-back
        XMFLOAT3(localMax.x, localMax.y, localMax.z), // 6: right-top-back
        XMFLOAT3(localMin.x, localMax.y, localMax.z), // 7: left-top-back
    };

    // Transform corners to world space
    XMFLOAT3 worldCorners[8];
    for (int i = 0; i < 8; i++) {
        XMVECTOR localCorner = XMLoadFloat3(&corners[i]);
        XMVECTOR worldCorner = XMVector3TransformCoord(localCorner, worldMatrix);
        XMStoreFloat3(&worldCorners[i], worldCorner);
    }

    // 12 edges of AABB
    int edges[12][2] = {
        // Front face
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        // Back face
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        // Connecting edges
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    for (int i = 0; i < 12; i++) {
        AddLine(worldCorners[edges[i][0]], worldCorners[edges[i][1]], color);
    }
}

void CDebugLinePass::UpdateVertexBuffer() {
    if (m_dynamicLines.empty()) return;

    ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
    if (!context) return;

    // Map and upload vertex data
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        size_t dataSize = sizeof(LineVertex) * m_dynamicLines.size();
        memcpy(mapped.pData, m_dynamicLines.data(), dataSize);
        context->Unmap(m_vertexBuffer.Get(), 0);
    }
}

void CDebugLinePass::Render(XMMATRIX view, XMMATRIX proj,
                            UINT viewportWidth, UINT viewportHeight) {
    if (!m_initialized || m_dynamicLines.empty()) return;

    ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
    if (!context) return;

    // Update vertex buffer
    UpdateVertexBuffer();

    // Update constant buffers
    XMMATRIX viewProj = view * proj;
    CBPerFrameVS cbVS;
    cbVS.viewProj = XMMatrixTranspose(viewProj);  // HLSL expects column-major
    context->UpdateSubresource(m_cbPerFrameVS.Get(), 0, nullptr, &cbVS, 0, 0);

    CBPerFrameGS cbGS;
    cbGS.viewportSize = XMFLOAT2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
    cbGS.lineThickness = m_lineThickness;
    cbGS.padding = 0.0f;
    context->UpdateSubresource(m_cbPerFrameGS.Get(), 0, nullptr, &cbGS, 0, 0);

    // Set pipeline state
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    UINT stride = sizeof(LineVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_cbPerFrameVS.GetAddressOf());

    context->GSSetShader(m_gs.Get(), nullptr, 0);
    context->GSSetConstantBuffers(0, 1, m_cbPerFrameGS.GetAddressOf());

    context->PSSetShader(m_ps.Get(), nullptr, 0);

    // Draw lines
    UINT vertexCount = static_cast<UINT>(m_dynamicLines.size());
    context->Draw(vertexCount, 0);

    // Unbind geometry shader (important!)
    context->GSSetShader(nullptr, nullptr, 0);
}
