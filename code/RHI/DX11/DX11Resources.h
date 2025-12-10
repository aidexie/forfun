#pragma once
#include "../RHIResources.h"
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ============================================
// DX11 Resource Implementations
// ============================================

namespace RHI {
namespace DX11 {

// ============================================
// DX11 Buffer
// ============================================
class CDX11Buffer : public IBuffer {
public:
    CDX11Buffer(ID3D11Buffer* buffer, uint32_t size, ECPUAccess cpuAccess, ID3D11DeviceContext* context)
        : m_buffer(buffer), m_size(size), m_cpuAccess(cpuAccess), m_context(context) {}

    ~CDX11Buffer() override = default;

    void* Map() override {
        if (m_cpuAccess != ECPUAccess::Write) {
            return nullptr;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            return nullptr;
        }
        return mapped.pData;
    }

    void Unmap() override {
        if (m_cpuAccess == ECPUAccess::Write) {
            m_context->Unmap(m_buffer.Get(), 0);
        }
    }

    uint32_t GetSize() const override { return m_size; }
    void* GetNativeHandle() override { return m_buffer.Get(); }

    ID3D11Buffer* GetD3D11Buffer() { return m_buffer.Get(); }

private:
    ComPtr<ID3D11Buffer> m_buffer;
    uint32_t m_size;
    ECPUAccess m_cpuAccess;
    ID3D11DeviceContext* m_context;  // Non-owning
};

// ============================================
// DX11 Texture
// ============================================
class CDX11Texture : public ITexture {
public:
    CDX11Texture(ID3D11Texture2D* texture, uint32_t width, uint32_t height, ETextureFormat format)
        : m_texture(texture), m_width(width), m_height(height), m_format(format) {}

    ~CDX11Texture() override = default;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return 1; }
    ETextureFormat GetFormat() const override { return m_format; }

    void* GetNativeHandle() override { return m_texture.Get(); }
    void* GetRTV() override { return m_rtv.Get(); }
    void* GetDSV() override { return m_dsv.Get(); }
    void* GetSRV() override { return m_srv.Get(); }
    void* GetUAV() override { return m_uav.Get(); }

    void SetRTV(ID3D11RenderTargetView* rtv) { m_rtv = rtv; }
    void SetDSV(ID3D11DepthStencilView* dsv) { m_dsv = dsv; }
    void SetSRV(ID3D11ShaderResourceView* srv) { m_srv = srv; }
    void SetUAV(ID3D11UnorderedAccessView* uav) { m_uav = uav; }

    ID3D11Texture2D* GetD3D11Texture() { return m_texture.Get(); }

private:
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    ComPtr<ID3D11UnorderedAccessView> m_uav;
    uint32_t m_width;
    uint32_t m_height;
    ETextureFormat m_format;
};

// ============================================
// DX11 Sampler
// ============================================
class CDX11Sampler : public ISampler {
public:
    CDX11Sampler(ID3D11SamplerState* sampler) : m_sampler(sampler) {}
    ~CDX11Sampler() override = default;

    void* GetNativeHandle() override { return m_sampler.Get(); }
    ID3D11SamplerState* GetD3D11Sampler() { return m_sampler.Get(); }

private:
    ComPtr<ID3D11SamplerState> m_sampler;
};

// ============================================
// DX11 Shader
// ============================================
class CDX11Shader : public IShader {
public:
    CDX11Shader(EShaderType type) : m_type(type) {}
    ~CDX11Shader() override = default;

    void* GetNativeHandle() override {
        // Return the shader interface
        switch (m_type) {
            case EShaderType::Vertex: return m_vs.Get();
            case EShaderType::Pixel: return m_ps.Get();
            case EShaderType::Compute: return m_cs.Get();
            case EShaderType::Geometry: return m_gs.Get();
            case EShaderType::Hull: return m_hs.Get();
            case EShaderType::Domain: return m_ds.Get();
            default: return nullptr;
        }
    }

    EShaderType GetType() const override { return m_type; }

    void SetVertexShader(ID3D11VertexShader* vs, ID3DBlob* blob) { m_vs = vs; m_blob = blob; }
    void SetPixelShader(ID3D11PixelShader* ps) { m_ps = ps; }
    void SetComputeShader(ID3D11ComputeShader* cs) { m_cs = cs; }
    void SetGeometryShader(ID3D11GeometryShader* gs) { m_gs = gs; }
    void SetHullShader(ID3D11HullShader* hs) { m_hs = hs; }
    void SetDomainShader(ID3D11DomainShader* ds) { m_ds = ds; }

    ID3D11VertexShader* GetVertexShader() { return m_vs.Get(); }
    ID3D11PixelShader* GetPixelShader() { return m_ps.Get(); }
    ID3D11ComputeShader* GetComputeShader() { return m_cs.Get(); }
    ID3D11GeometryShader* GetGeometryShader() { return m_gs.Get(); }
    ID3D11HullShader* GetHullShader() { return m_hs.Get(); }
    ID3D11DomainShader* GetDomainShader() { return m_ds.Get(); }
    ID3DBlob* GetBlob() { return m_blob.Get(); }

private:
    EShaderType m_type;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11ComputeShader> m_cs;
    ComPtr<ID3D11GeometryShader> m_gs;
    ComPtr<ID3D11HullShader> m_hs;
    ComPtr<ID3D11DomainShader> m_ds;
    ComPtr<ID3DBlob> m_blob;  // Keep bytecode for input layout
};

// ============================================
// DX11 Pipeline State
// ============================================
class CDX11PipelineState : public IPipelineState {
public:
    CDX11PipelineState() = default;
    ~CDX11PipelineState() override = default;

    void* GetNativeHandle() override { return nullptr; }  // DX11 doesn't have unified PSO

    // Graphics pipeline state components
    void SetInputLayout(ID3D11InputLayout* layout) { m_inputLayout = layout; }
    void SetRasterizerState(ID3D11RasterizerState* state) { m_rasterizerState = state; }
    void SetDepthStencilState(ID3D11DepthStencilState* state) { m_depthStencilState = state; }
    void SetBlendState(ID3D11BlendState* state) { m_blendState = state; }
    void SetTopology(D3D11_PRIMITIVE_TOPOLOGY topology) { m_topology = topology; }

    void SetVertexShader(CDX11Shader* shader) { m_vertexShader = shader; }
    void SetPixelShader(CDX11Shader* shader) { m_pixelShader = shader; }
    void SetGeometryShader(CDX11Shader* shader) { m_geometryShader = shader; }
    void SetHullShader(CDX11Shader* shader) { m_hullShader = shader; }
    void SetDomainShader(CDX11Shader* shader) { m_domainShader = shader; }

    // Compute pipeline
    void SetComputeShader(CDX11Shader* shader) { m_computeShader = shader; }

    // Getters
    ID3D11InputLayout* GetInputLayout() { return m_inputLayout.Get(); }
    ID3D11RasterizerState* GetRasterizerState() { return m_rasterizerState.Get(); }
    ID3D11DepthStencilState* GetDepthStencilState() { return m_depthStencilState.Get(); }
    ID3D11BlendState* GetBlendState() { return m_blendState.Get(); }
    D3D11_PRIMITIVE_TOPOLOGY GetTopology() { return m_topology; }

    CDX11Shader* GetVertexShader() { return m_vertexShader; }
    CDX11Shader* GetPixelShader() { return m_pixelShader; }
    CDX11Shader* GetGeometryShader() { return m_geometryShader; }
    CDX11Shader* GetHullShader() { return m_hullShader; }
    CDX11Shader* GetDomainShader() { return m_domainShader; }
    CDX11Shader* GetComputeShader() { return m_computeShader; }

private:
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    ComPtr<ID3D11BlendState> m_blendState;
    D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // Shader references (non-owning)
    CDX11Shader* m_vertexShader = nullptr;
    CDX11Shader* m_pixelShader = nullptr;
    CDX11Shader* m_geometryShader = nullptr;
    CDX11Shader* m_hullShader = nullptr;
    CDX11Shader* m_domainShader = nullptr;
    CDX11Shader* m_computeShader = nullptr;
};

} // namespace DX11
} // namespace RHI
