#include "Panels.h"
#include "imgui.h"
#include "Camera.h"
#include "Offscreen.h"
#include "DX11Context.h"
#include <d3dcompiler.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace {
    struct Pipeline {
        bool inited=false;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader>  ps;
        ComPtr<ID3D11InputLayout>  il;
        ComPtr<ID3D11Buffer>       vb;
        ComPtr<ID3D11Buffer>       cb; // time
    } g;

    const char* kVS = R"(
cbuffer CB : register(b0) { float t; float3 pad; };
struct VSIn { float2 pos:POSITION; float3 col:COLOR; };
struct VSOut{ float4 pos:SV_POSITION; float3 col:COLOR; };
VSOut main(VSIn v){
    float c = cos(t), s = sin(t);
    float2 p = float2( v.pos.x*c - v.pos.y*s, v.pos.x*s + v.pos.y*c );
    VSOut o; o.pos=float4(p,0,1); o.col=v.col; return o;
}
)";

    const char* kPS = R"(
struct PSIn{ float4 pos:SV_POSITION; float3 col:COLOR; };
float4 main(PSIn i):SV_Target{ return float4(i.col,1); }
)";

    void EnsurePipeline(ID3D11Device* dev) {
        if (g.inited) return;
        ComPtr<ID3DBlob> vsb, psb, err;
        D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, vsb.GetAddressOf(), err.GetAddressOf());
        D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, psb.GetAddressOf(), err.GetAddressOf());
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, g.vs.GetAddressOf());
        dev->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, g.ps.GetAddressOf());

        D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",   0,DXGI_FORMAT_R32G32B32_FLOAT,0, 8,  D3D11_INPUT_PER_VERTEX_DATA,0},
        };
        dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), g.il.GetAddressOf());

        struct V { float x,y; float r,g,b; };
        V verts[3] = {
            { 0.0f,  0.6f, 1,0,0},
            {-0.6f, -0.6f, 0,1,0},
            { 0.6f, -0.6f, 0,0,1},
        };
        D3D11_BUFFER_DESC bd{}; bd.BindFlags=D3D11_BIND_VERTEX_BUFFER; bd.ByteWidth=sizeof(verts); bd.Usage=D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA sd{verts};
        dev->CreateBuffer(&bd, &sd, g.vb.GetAddressOf());

        bd = {}; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.ByteWidth = 16; bd.Usage=D3D11_USAGE_DEFAULT;
        dev->CreateBuffer(&bd, nullptr, g.cb.GetAddressOf());

        g.inited = true;
    }
}

// ViewportPanel.cpp  —— file-scope statics
static ImVec2 s_lastAvail = ImVec2(0, 0);

ImVec2 Panels::GetViewportLastSize() {
    return s_lastAvail;
}

void Panels::DrawViewport(CScene& scene, EditorCamera& editorCam,
    ID3D11ShaderResourceView* srv,
    size_t srcWidth, size_t srcHeight)
{
    ImGui::Begin("Viewport");

    // Measure current available size and remember it for the next frame’s render pass
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.f) avail.x = 1.f;
    if (avail.y < 1.f) avail.y = 1.f;
    s_lastAvail = avail;

    // Keep editor camera’s aspect in sync with the panel (optional)
    editorCam.aspect = (avail.y > 0.f) ? (avail.x / avail.y) : editorCam.aspect;

    // Draw the provided texture (no ownership). If null, show placeholder.
    if (srv && srcWidth > 0 && srcHeight > 0) {
        // (Optional) Fit preserving aspect; simplest is to just fill 'avail':
        ImGui::Image((ImTextureID)srv, avail, ImVec2(0, 0), ImVec2(1, 1));
    }
    else {
        ImGui::TextUnformatted("No viewport image.");
    }

    ImGui::End();
}

