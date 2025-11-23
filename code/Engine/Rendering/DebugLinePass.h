// Engine/Rendering/DebugLinePass.h
#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

class CDebugLinePass {
public:
    void Initialize();
    void Shutdown();

    // Clear dynamic line buffer (call at start of frame)
    void BeginFrame();

    // CPU interface for adding lines
    void AddLine(DirectX::XMFLOAT3 from, DirectX::XMFLOAT3 to, DirectX::XMFLOAT4 color);
    void AddAABB(DirectX::XMFLOAT3 localMin, DirectX::XMFLOAT3 localMax,
                 DirectX::XMMATRIX worldMatrix, DirectX::XMFLOAT4 color);

    // Render all accumulated lines
    void Render(DirectX::XMMATRIX view, DirectX::XMMATRIX proj,
                UINT viewportWidth, UINT viewportHeight);

    // Settings
    void SetLineThickness(float thickness) { m_lineThickness = thickness; }
    float GetLineThickness() const { return m_lineThickness; }

private:
    struct LineVertex {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 color;
    };

    struct CBPerFrameVS {
        DirectX::XMMATRIX viewProj;
    };

    struct CBPerFrameGS {
        DirectX::XMFLOAT2 viewportSize;
        float lineThickness;
        float padding;
    };

    void CreateShaders();
    void CreateBuffers();
    void UpdateVertexBuffer();

    std::vector<LineVertex> m_dynamicLines;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrameVS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrameGS;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> m_gs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;

    UINT m_maxVertices = 10000;  // Max vertices per frame
    float m_lineThickness = 2.0f;
    bool m_initialized = false;
};
