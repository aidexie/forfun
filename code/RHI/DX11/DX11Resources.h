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
// DX11 Texture (supports 2D and 3D textures)
// ============================================
class CDX11Texture : public ITexture {
public:
    // Owning constructor for 2D textures (takes ownership via ComPtr)
    CDX11Texture(ID3D11Texture2D* texture, uint32_t width, uint32_t height, ETextureFormat format,
                 uint32_t arraySize = 1, uint32_t mipLevels = 1, ID3D11DeviceContext* context = nullptr)
        : m_texture2D(texture), m_rawTexture(nullptr), m_width(width), m_height(height), m_depth(1),
          m_format(format), m_arraySize(arraySize), m_mipLevels(mipLevels), m_owning(true), m_is3D(false), m_context(context) {}

    // Owning constructor for 3D textures
    CDX11Texture(ID3D11Texture3D* texture, uint32_t width, uint32_t height, uint32_t depth, ETextureFormat format,
                 uint32_t mipLevels = 1, ID3D11DeviceContext* context = nullptr)
        : m_texture3D(texture), m_rawTexture(nullptr), m_width(width), m_height(height), m_depth(depth),
          m_format(format), m_arraySize(1), m_mipLevels(mipLevels), m_owning(true), m_is3D(true), m_context(context) {}

    // Non-owning constructor for 2D textures
    CDX11Texture(ID3D11Texture2D* texture, uint32_t width, uint32_t height, ETextureFormat format, bool owning,
                 ID3D11DeviceContext* context = nullptr)
        : m_rawTexture(owning ? nullptr : texture), m_width(width), m_height(height), m_depth(1),
          m_format(format), m_arraySize(1), m_mipLevels(1), m_owning(owning), m_is3D(false), m_context(context)
    {
        if (owning) {
            m_texture2D = texture;
        }
    }

    ~CDX11Texture() override = default;

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return m_depth; }
    uint32_t GetArraySize() const override { return m_arraySize; }
    uint32_t GetMipLevels() const override { return m_mipLevels; }
    ETextureFormat GetFormat() const override { return m_format; }

    void* GetNativeHandle() override {
        if (m_is3D) {
            return m_texture3D.Get();
        }
        return m_owning ? m_texture2D.Get() : m_rawTexture;
    }
    void* GetRTV() override { return m_rtv.Get(); }
    void* GetDSV() override { return m_dsv.Get(); }
    void* GetSRV() override { return m_srv.Get(); }
    void* GetUAV() override { return m_uav.Get(); }

    // Per-slice DSV for texture arrays (CSM shadow mapping)
    void* GetDSVSlice(uint32_t arrayIndex) override {
        if (arrayIndex >= m_arraySize || arrayIndex >= MAX_SLICE_VIEWS) return nullptr;
        return m_sliceDSVs[arrayIndex].Get();
    }

    // Per-slice RTV for texture arrays/cubemaps (face rendering)
    void* GetRTVSlice(uint32_t arrayIndex) override {
        if (arrayIndex >= m_arraySize || arrayIndex >= MAX_SLICE_VIEWS) return nullptr;
        return m_sliceRTVs[arrayIndex].Get();
    }

    // Per-slice SRV for texture arrays/cubemaps (debug visualization)
    // Created on-demand and cached
    void* GetSRVSlice(uint32_t arrayIndex, uint32_t mipLevel = 0) override;

    // CPU Access (for Staging textures)
    MappedTexture Map(uint32_t arraySlice = 0, uint32_t mipLevel = 0) override {
        MappedTexture result;
        if (!m_context || !m_isStaging) return result;

        UINT subresource = D3D11CalcSubresource(mipLevel, arraySlice, m_mipLevels);
        D3D11_MAPPED_SUBRESOURCE mapped;
        D3D11_MAP mapType = (m_cpuAccess == ECPUAccess::Write) ? D3D11_MAP_WRITE : D3D11_MAP_READ;
        HRESULT hr = m_context->Map(GetD3D11Resource(), subresource, mapType, 0, &mapped);
        if (SUCCEEDED(hr)) {
            result.pData = mapped.pData;
            result.rowPitch = mapped.RowPitch;
            result.depthPitch = mapped.DepthPitch;
        }
        return result;
    }

    void Unmap(uint32_t arraySlice = 0, uint32_t mipLevel = 0) override {
        if (!m_context || !m_isStaging) return;
        UINT subresource = D3D11CalcSubresource(mipLevel, arraySlice, m_mipLevels);
        m_context->Unmap(GetD3D11Resource(), subresource);
    }

    void SetRTV(ID3D11RenderTargetView* rtv) { m_rtv = rtv; }
    void SetDSV(ID3D11DepthStencilView* dsv) { m_dsv = dsv; }
    void SetSRV(ID3D11ShaderResourceView* srv) { m_srv = srv; }
    void SetUAV(ID3D11UnorderedAccessView* uav) { m_uav = uav; }
    void SetSliceDSV(uint32_t index, ID3D11DepthStencilView* dsv) { if (index < MAX_SLICE_VIEWS) m_sliceDSVs[index] = dsv; }
    void SetSliceRTV(uint32_t index, ID3D11RenderTargetView* rtv) { if (index < MAX_SLICE_VIEWS) m_sliceRTVs[index] = rtv; }
    void SetArraySize(uint32_t arraySize) { m_arraySize = arraySize; }
    void SetMipLevels(uint32_t mipLevels) { m_mipLevels = mipLevels; }
    void SetIsStaging(bool isStaging) { m_isStaging = isStaging; }
    void SetContext(ID3D11DeviceContext* context) { m_context = context; }
    void SetCPUAccess(ECPUAccess cpuAccess) { m_cpuAccess = cpuAccess; }
    void SetIsCubemapArray(bool isCubemapArray) { m_isCubemapArray = isCubemapArray; }
    void SetCubeCount(uint32_t cubeCount) { m_cubeCount = cubeCount; }
    void SetDevice(ID3D11Device* device) { m_device = device; }

    ID3D11Texture2D* GetD3D11Texture() { return m_owning ? m_texture2D.Get() : m_rawTexture; }
    ID3D11Texture3D* GetD3D11Texture3D() { return m_texture3D.Get(); }
    ID3D11Resource* GetD3D11Resource() {
        if (m_is3D) return m_texture3D.Get();
        return m_owning ? m_texture2D.Get() : m_rawTexture;
    }
    bool Is3D() const { return m_is3D; }
    bool IsCubemapArray() const { return m_isCubemapArray; }
    uint32_t GetCubeCount() const { return m_cubeCount; }

private:
    static const uint32_t MAX_SLICE_VIEWS = 6;  // Max for cubemap faces
    static const uint32_t MAX_MIP_LEVELS = 16;  // Max mip levels for SRV slice cache

    ComPtr<ID3D11Texture2D> m_texture2D;    // Owning reference for 2D textures
    ComPtr<ID3D11Texture3D> m_texture3D;    // Owning reference for 3D textures
    ID3D11Texture2D* m_rawTexture;          // Non-owning raw pointer (2D only)
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    ComPtr<ID3D11UnorderedAccessView> m_uav;
    ComPtr<ID3D11DepthStencilView> m_sliceDSVs[MAX_SLICE_VIEWS];  // Per-slice DSVs for array textures
    ComPtr<ID3D11RenderTargetView> m_sliceRTVs[MAX_SLICE_VIEWS];  // Per-slice RTVs for cubemap faces
    // SRV slice cache: [arrayIndex * MAX_MIP_LEVELS + mipLevel] -> SRV
    mutable std::unordered_map<uint32_t, ComPtr<ID3D11ShaderResourceView>> m_sliceSRVCache;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_depth;
    uint32_t m_arraySize;
    uint32_t m_mipLevels;
    uint32_t m_cubeCount = 0;       // For cubemap arrays: number of cubes
    ETextureFormat m_format;
    ECPUAccess m_cpuAccess = ECPUAccess::None;  // CPU access mode for staging textures
    bool m_owning;      // If true, texture is owned by ComPtr; if false, m_rawTexture is used
    bool m_is3D;        // If true, m_texture3D is used; if false, m_texture2D/m_rawTexture is used
    bool m_isStaging = false;       // If true, this is a staging texture for CPU access
    bool m_isCubemapArray = false;  // If true, this is a cubemap array texture
    ID3D11DeviceContext* m_context = nullptr;  // Non-owning, for Map/Unmap operations
    ID3D11Device* m_device = nullptr;  // Non-owning, for creating SRV slices on-demand
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
