#include "PostProcessPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

struct FullscreenVertex {
    float x, y;       // Position (NDC space)
    float u, v;       // UV
};

bool CPostProcessPass::Initialize() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return false;

    createFullscreenQuad();
    createShaders();

    // Create sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, m_sampler.GetAddressOf());

    // Create rasterizer state
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = FALSE;
    device->CreateRasterizerState(&rd, m_rasterState.GetAddressOf());

    // Create depth stencil state (no depth test)
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, m_depthState.GetAddressOf());

    // Create constant buffer for exposure
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;  // Align to 16 bytes (float4)
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cbd, nullptr, m_constantBuffer.GetAddressOf());

    return true;
}

void CPostProcessPass::Shutdown() {
    m_vs.Reset();
    m_ps.Reset();
    m_inputLayout.Reset();
    m_vertexBuffer.Reset();
    m_sampler.Reset();
    m_rasterState.Reset();
    m_depthState.Reset();
}

void CPostProcessPass::Render(ID3D11ShaderResourceView* hdrInput,
                             ID3D11RenderTargetView* ldrOutput,
                             UINT width, UINT height,
                             float exposure) {
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());
    if (!context || !hdrInput || !ldrOutput) return;

    // Update constant buffer with exposure
    float cbData[4] = { exposure, 0.0f, 0.0f, 0.0f };  // Align to float4
    context->UpdateSubresource(m_constantBuffer.Get(), 0, nullptr, cbData, 0, 0);

    // Set render target
    context->OMSetRenderTargets(1, &ldrOutput, nullptr);

    // Set viewport
    D3D11_VIEWPORT vp{};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // Set render states
    context->RSSetState(m_rasterState.Get());
    context->OMSetDepthStencilState(m_depthState.Get(), 0);

    // Set pipeline
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    UINT stride = sizeof(FullscreenVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);

    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    context->PSSetShaderResources(0, 1, &hdrInput);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Draw fullscreen quad
    context->Draw(4, 0);

    // Unbind ALL resources to prevent hazards
    ID3D11ShaderResourceView* nullSRVs[8] = { nullptr };
    context->PSSetShaderResources(0, 8, nullSRVs);
    context->VSSetShaderResources(0, 8, nullSRVs);

    // Unbind render target to prevent hazards
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

void CPostProcessPass::createFullscreenQuad() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return;

    // Fullscreen quad in NDC space (triangle strip)
    FullscreenVertex vertices[] = {
        { -1.0f,  1.0f,  0.0f, 0.0f },  // Top-left
        {  1.0f,  1.0f,  1.0f, 0.0f },  // Top-right
        { -1.0f, -1.0f,  0.0f, 1.0f },  // Bottom-left
        {  1.0f, -1.0f,  1.0f, 1.0f }   // Bottom-right
    };

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(vertices);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initData{ vertices, 0, 0 };
    device->CreateBuffer(&bd, &initData, m_vertexBuffer.GetAddressOf());
}

void CPostProcessPass::createShaders() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return;

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

        // Reinhard Tone Mapping (simple)
        float3 Reinhard(float3 x) {
            return x / (x + 1.0);
        }

        float4 main(PSIn input) : SV_Target {
            // Sample HDR input (linear space)
            float3 hdrColor = hdrTexture.Sample(samp, input.uv).rgb;

            // Apply exposure (adjust brightness before tone mapping)
            hdrColor *= gExposure;

            // Tone mapping: HDR → LDR [0, 1] (still linear space)
            float3 ldrColor = ACESFilm(hdrColor);
            // Alternative: float3 ldrColor = Reinhard(hdrColor);

            // Gamma correction: Linear → sRGB
            // Since output RT is UNORM_SRGB, GPU will do this automatically
            // If output RT is UNORM, uncomment the line below:
            // ldrColor = pow(ldrColor, float3(1.0/2.2, 1.0/2.2, 1.0/2.2));

            return float4(ldrColor, 1.0);
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());
}
