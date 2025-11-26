// Engine/Rendering/GridPass.h
#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

// CGridPass: Renders an infinite procedural grid on the XZ plane at Y=0
// Uses shader-based rendering (full-screen quad) with depth buffer reconstruction
// Supports dual-scale grid lines, distance fading, and view angle fading
class CGridPass {
public:
    static CGridPass& Instance();

    void Initialize();
    void Shutdown();

    // Render the grid
    // Note: Uses GPU depth test with currently bound DSV (no depth SRV needed)
    // Render after all game content (opaque, skybox, transparent) to match Unity/UE
    void Render(DirectX::XMMATRIX view, DirectX::XMMATRIX proj,
                DirectX::XMFLOAT3 cameraPos);

    // Settings
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    
    void SetGridColor(DirectX::XMFLOAT3 color) { m_gridColor = color; }
    DirectX::XMFLOAT3 GetGridColor() const { return m_gridColor; }

    void SetFadeDistance(float start, float end) {
        m_fadeStart = start;
        m_fadeEnd = end;
    }

private:
    CGridPass() = default;
    ~CGridPass() = default;
    CGridPass(const CGridPass&) = delete;
    CGridPass& operator=(const CGridPass&) = delete;

    void CreateShaders();
    void CreateBuffers();
    void CreateStates();

    struct CBPerFrame {
        DirectX::XMMATRIX viewProj;
        DirectX::XMMATRIX invViewProj;
        DirectX::XMFLOAT3 cameraPos;
        float fadeStart;
        float fadeEnd;
        DirectX::XMFLOAT3 padding;
    };

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrame;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;

    bool m_initialized = false;
    bool m_enabled = true;

    // Grid settings
    DirectX::XMFLOAT3 m_gridColor = DirectX::XMFLOAT3(0.5f, 0.5f, 0.55f);
    float m_fadeStart = 50.0f;   // Start fading at 50m
    float m_fadeEnd = 100.0f;    // Fully fade at 100m
};
