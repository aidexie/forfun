#pragma once
#include "../RHIResources.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>

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
//
// Design: Stores TextureDesc for metadata, creates Views on-demand and caches them.
// Views are internal implementation details, accessed only by CDX11CommandList.
// ============================================
class CDX11Texture : public ITexture {
public:
    // Primary constructor - takes desc and native resource
    CDX11Texture(const TextureDesc& desc, ID3D11Texture2D* texture, ID3D11Device* device, ID3D11DeviceContext* context)
        : m_desc(desc), m_texture2D(texture), m_device(device), m_context(context) {}

    // Constructor for 3D textures
    CDX11Texture(const TextureDesc& desc, ID3D11Texture3D* texture, ID3D11Device* device, ID3D11DeviceContext* context)
        : m_desc(desc), m_texture3D(texture), m_device(device), m_context(context) {}

    ~CDX11Texture() override = default;

    // ============================================
    // ITexture interface
    // ============================================
    const TextureDesc& GetDesc() const override { return m_desc; }

    void* GetNativeHandle() override {
        if (m_desc.dimension == ETextureDimension::Tex3D) {
            return m_texture3D.Get();
        }
        return m_texture2D.Get();
    }

    MappedTexture Map(uint32_t arraySlice = 0, uint32_t mipLevel = 0) override {
        MappedTexture result;
        if (!m_context || !(m_desc.usage & ETextureUsage::Staging)) return result;

        UINT subresource = D3D11CalcSubresource(mipLevel, arraySlice, m_desc.mipLevels);
        D3D11_MAPPED_SUBRESOURCE mapped;
        D3D11_MAP mapType = (m_desc.cpuAccess == ECPUAccess::Write) ? D3D11_MAP_WRITE : D3D11_MAP_READ;
        HRESULT hr = m_context->Map(GetD3D11Resource(), subresource, mapType, 0, &mapped);
        if (SUCCEEDED(hr)) {
            result.pData = mapped.pData;
            result.rowPitch = mapped.RowPitch;
            result.depthPitch = mapped.DepthPitch;
        }
        return result;
    }

    void Unmap(uint32_t arraySlice = 0, uint32_t mipLevel = 0) override {
        if (!m_context || !(m_desc.usage & ETextureUsage::Staging)) return;
        UINT subresource = D3D11CalcSubresource(mipLevel, arraySlice, m_desc.mipLevels);
        m_context->Unmap(GetD3D11Resource(), subresource);
    }

    // ============================================
    // View accessors (internal use by CDX11CommandList)
    // GetOrCreate pattern: creates on first access, caches for reuse
    // ============================================

    // Get default SRV (all mips, all slices)
    ID3D11ShaderResourceView* GetOrCreateSRV();

    // Get SRV for specific mip/slice
    ID3D11ShaderResourceView* GetOrCreateSRVSlice(uint32_t arraySlice, uint32_t mipLevel = 0);

    // Get default RTV (mip 0, slice 0)
    ID3D11RenderTargetView* GetOrCreateRTV();

    // Get RTV for specific mip/slice
    ID3D11RenderTargetView* GetOrCreateRTVSlice(uint32_t arraySlice, uint32_t mipLevel = 0);

    // Get default DSV (slice 0)
    ID3D11DepthStencilView* GetOrCreateDSV();

    // Get DSV for specific slice
    ID3D11DepthStencilView* GetOrCreateDSVSlice(uint32_t arraySlice);

    // Get default UAV (mip 0)
    ID3D11UnorderedAccessView* GetOrCreateUAV();

    // ============================================
    // Legacy setters (for CreateTexture during migration)
    // TODO: Remove these after full migration
    // ============================================
    void SetSRV(ID3D11ShaderResourceView* srv) { m_defaultSRV = srv; }
    void SetRTV(ID3D11RenderTargetView* rtv) { m_defaultRTV = rtv; }
    void SetDSV(ID3D11DepthStencilView* dsv) { m_defaultDSV = dsv; }
    void SetUAV(ID3D11UnorderedAccessView* uav) { m_defaultUAV = uav; }
    void SetSliceRTV(uint32_t index, ID3D11RenderTargetView* rtv);
    void SetSliceDSV(uint32_t index, ID3D11DepthStencilView* dsv);

    // ============================================
    // Internal helpers
    // ============================================
    ID3D11Resource* GetD3D11Resource() {
        if (m_desc.dimension == ETextureDimension::Tex3D) return m_texture3D.Get();
        return m_texture2D.Get();
    }

    ID3D11Texture2D* GetD3D11Texture2D() { return m_texture2D.Get(); }
    ID3D11Texture3D* GetD3D11Texture3D() { return m_texture3D.Get(); }

private:
    // View cache key
    struct ViewKey {
        uint32_t mipLevel;
        uint32_t arraySlice;

        bool operator==(const ViewKey& other) const {
            return mipLevel == other.mipLevel && arraySlice == other.arraySlice;
        }
    };

    struct ViewKeyHash {
        size_t operator()(const ViewKey& key) const {
            return std::hash<uint64_t>{}((uint64_t)key.mipLevel << 32 | key.arraySlice);
        }
    };

    // View creation helpers
    ComPtr<ID3D11ShaderResourceView> createSRV(uint32_t mipLevel, uint32_t numMips, uint32_t arraySlice, uint32_t numSlices);
    ComPtr<ID3D11RenderTargetView> createRTV(uint32_t mipLevel, uint32_t arraySlice);
    ComPtr<ID3D11DepthStencilView> createDSV(uint32_t arraySlice);
    ComPtr<ID3D11UnorderedAccessView> createUAV(uint32_t mipLevel);

private:
    TextureDesc m_desc;

    // Native resource (one of these is used based on dimension)
    ComPtr<ID3D11Texture2D> m_texture2D;
    ComPtr<ID3D11Texture3D> m_texture3D;

    // Device/context for view creation and mapping
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Default views (created on first access)
    ComPtr<ID3D11ShaderResourceView> m_defaultSRV;
    ComPtr<ID3D11RenderTargetView> m_defaultRTV;
    ComPtr<ID3D11DepthStencilView> m_defaultDSV;
    ComPtr<ID3D11UnorderedAccessView> m_defaultUAV;

    // View caches for slice/mip-specific views
    mutable std::unordered_map<ViewKey, ComPtr<ID3D11ShaderResourceView>, ViewKeyHash> m_srvCache;
    mutable std::unordered_map<ViewKey, ComPtr<ID3D11RenderTargetView>, ViewKeyHash> m_rtvCache;
    mutable std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilView>> m_dsvCache;  // keyed by arraySlice
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
