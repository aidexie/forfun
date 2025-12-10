#include "DX11RenderContext.h"
#include "DX11Utils.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace RHI {
namespace DX11 {

CDX11RenderContext::CDX11RenderContext()
{
}

CDX11RenderContext::~CDX11RenderContext() {
    Shutdown();
}

bool CDX11RenderContext::Initialize(void* nativeWindowHandle, uint32_t width, uint32_t height) {
    if (m_initialized) {
        return true;
    }

    // Initialize underlying DX11 context (singleton)
    HWND hwnd = static_cast<HWND>(nativeWindowHandle);
    CDX11Context& ctx = CDX11Context::Instance();
    if (!ctx.Initialize(hwnd, width, height)) {
        return false;
    }

    // Create command list wrapper
    m_commandList = std::make_unique<CDX11CommandList>(ctx.GetContext());

    // Wrap backbuffer - need to get actual texture from swap chain
    IDXGISwapChain* swapChain = ctx.GetSwapChain();
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr)) {
        return false;
    }

    ID3D11RenderTargetView* backbufferRTV = ctx.GetBackbufferRTV();
    m_backbufferWrapper = std::make_unique<CDX11Texture>(backbuffer, width, height, ETextureFormat::R8G8B8A8_UNORM);
    m_backbufferWrapper->SetRTV(backbufferRTV);
    backbuffer->Release();  // CDX11Texture owns it now

    // Wrap depth stencil - need to get actual texture
    // Note: CDX11Context doesn't expose the depth texture, only the DSV
    // For now, create a dummy wrapper. Real implementation should expose the texture from CDX11Context
    m_depthStencilWrapper = std::make_unique<CDX11Texture>(nullptr, width, height, ETextureFormat::D24_UNORM_S8_UINT);
    m_depthStencilWrapper->SetDSV(ctx.GetDSV());

    m_initialized = true;
    return true;
}

void CDX11RenderContext::Shutdown() {
    if (!m_initialized) {
        return;
    }

    m_backbufferWrapper.reset();
    m_depthStencilWrapper.reset();
    m_commandList.reset();
    CDX11Context::Instance().Shutdown();
    m_initialized = false;
}

void CDX11RenderContext::OnResize(uint32_t width, uint32_t height) {
    if (!m_initialized) {
        return;
    }

    CDX11Context& ctx = CDX11Context::Instance();
    ctx.OnResize(width, height);

    // Re-wrap backbuffer
    IDXGISwapChain* swapChain = ctx.GetSwapChain();
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (SUCCEEDED(hr)) {
        m_backbufferWrapper = std::make_unique<CDX11Texture>(backbuffer, width, height, ETextureFormat::R8G8B8A8_UNORM);
        m_backbufferWrapper->SetRTV(ctx.GetBackbufferRTV());
        backbuffer->Release();
    }

    // Re-wrap depth stencil
    m_depthStencilWrapper = std::make_unique<CDX11Texture>(nullptr, width, height, ETextureFormat::D24_UNORM_S8_UINT);
    m_depthStencilWrapper->SetDSV(ctx.GetDSV());
}

void CDX11RenderContext::BeginFrame() {
    // DX11 doesn't need explicit frame begin
}

void CDX11RenderContext::EndFrame() {
    // DX11 doesn't need explicit frame end
}

void CDX11RenderContext::Present(bool vsync) {
    CDX11Context::Instance().Present(vsync ? 1 : 0, 0);
}

ICommandList* CDX11RenderContext::GetCommandList() {
    return m_commandList.get();
}

IBuffer* CDX11RenderContext::CreateBuffer(const BufferDesc& desc, const void* initialData) {
    CDX11Context& ctx = CDX11Context::Instance();

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = desc.size;
    bufferDesc.Usage = ToD3D11Usage(desc.cpuAccess);
    bufferDesc.BindFlags = ToD3D11BindFlags(desc.usage);
    bufferDesc.CPUAccessFlags = ToD3D11CPUAccessFlags(desc.cpuAccess);
    bufferDesc.MiscFlags = 0;
    bufferDesc.StructureByteStride = desc.structureByteStride;

    if (desc.usage & EBufferUsage::Structured) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    }

    D3D11_SUBRESOURCE_DATA* pInitData = nullptr;
    D3D11_SUBRESOURCE_DATA initData = {};
    if (initialData) {
        initData.pSysMem = initialData;
        pInitData = &initData;
    }

    ID3D11Buffer* d3dBuffer = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateBuffer(&bufferDesc, pInitData, &d3dBuffer);
    if (FAILED(hr)) {
        return nullptr;
    }

    return new CDX11Buffer(d3dBuffer, desc.size, desc.cpuAccess, ctx.GetContext());
}

ITexture* CDX11RenderContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    CDX11Context& ctx = CDX11Context::Instance();

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = desc.width;
    texDesc.Height = desc.height;
    texDesc.MipLevels = desc.mipLevels;
    texDesc.ArraySize = desc.arraySize;
    texDesc.Format = ToDXGIFormat(desc.format);
    texDesc.SampleDesc.Count = desc.sampleCount;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = ToD3D11BindFlags(desc.usage);
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    // Check for cubemap usage
    if (desc.arraySize == 6) {
        texDesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    }

    if (desc.mipLevels == 0) {
        texDesc.MipLevels = 0;
        texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }

    D3D11_SUBRESOURCE_DATA* pInitData = nullptr;
    D3D11_SUBRESOURCE_DATA initData = {};
    if (initialData) {
        initData.pSysMem = initialData;
        initData.SysMemPitch = desc.width * 4; // TODO: Calculate based on format
        pInitData = &initData;
    }

    ID3D11Texture2D* d3dTexture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&texDesc, pInitData, &d3dTexture);
    if (FAILED(hr)) {
        return nullptr;
    }

    auto texture = new CDX11Texture(d3dTexture, desc.width, desc.height, desc.format);

    // Get actual mip levels after creation (if 0 was specified, D3D calculates it)
    D3D11_TEXTURE2D_DESC createdDesc;
    d3dTexture->GetDesc(&createdDesc);
    UINT actualMipLevels = createdDesc.MipLevels;

    // Create views based on usage flags
    // Use view format overrides if specified (for TYPELESS textures)
    if (desc.usage & ETextureUsage::RenderTarget) {
        ID3D11RenderTargetView* rtv = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        D3D11_RENDER_TARGET_VIEW_DESC* pRtvDesc = nullptr;

        // Use override format if specified
        if (desc.rtvFormat != ETextureFormat::Unknown) {
            rtvDesc.Format = ToDXGIFormat(desc.rtvFormat);
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = 0;
            pRtvDesc = &rtvDesc;
        }
        hr = ctx.GetDevice()->CreateRenderTargetView(d3dTexture, pRtvDesc, &rtv);
        if (SUCCEEDED(hr)) {
            texture->SetRTV(rtv);
        }
    }

    if (desc.usage & ETextureUsage::DepthStencil) {
        ID3D11DepthStencilView* dsv = nullptr;
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        D3D11_DEPTH_STENCIL_VIEW_DESC* pDsvDesc = nullptr;

        // Use override format if specified
        if (desc.dsvFormat != ETextureFormat::Unknown) {
            dsvDesc.Format = ToDXGIFormat(desc.dsvFormat);
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;
            pDsvDesc = &dsvDesc;
        }
        hr = ctx.GetDevice()->CreateDepthStencilView(d3dTexture, pDsvDesc, &dsv);
        if (SUCCEEDED(hr)) {
            texture->SetDSV(dsv);
        }
    }

    if (desc.usage & ETextureUsage::ShaderResource) {
        ID3D11ShaderResourceView* srv = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        // Use override format if specified, otherwise use texture format
        srvDesc.Format = (desc.srvFormat != ETextureFormat::Unknown)
            ? ToDXGIFormat(desc.srvFormat)
            : texDesc.Format;

        if (desc.arraySize == 6) {
            // Cubemap
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = actualMipLevels;
            srvDesc.TextureCube.MostDetailedMip = 0;
        } else if (desc.arraySize > 1) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MipLevels = actualMipLevels;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.ArraySize = desc.arraySize;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = actualMipLevels;
            srvDesc.Texture2D.MostDetailedMip = 0;
        }

        hr = ctx.GetDevice()->CreateShaderResourceView(d3dTexture, &srvDesc, &srv);
        if (SUCCEEDED(hr)) {
            texture->SetSRV(srv);
        }
    }

    if (desc.usage & ETextureUsage::UnorderedAccess) {
        ID3D11UnorderedAccessView* uav = nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        D3D11_UNORDERED_ACCESS_VIEW_DESC* pUavDesc = nullptr;

        // Use override format if specified
        if (desc.uavFormat != ETextureFormat::Unknown) {
            uavDesc.Format = ToDXGIFormat(desc.uavFormat);
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = 0;
            pUavDesc = &uavDesc;
        }
        hr = ctx.GetDevice()->CreateUnorderedAccessView(d3dTexture, pUavDesc, &uav);
        if (SUCCEEDED(hr)) {
            texture->SetUAV(uav);
        }
    }

    return texture;
}

ITexture* CDX11RenderContext::WrapNativeTexture(void* nativeTexture, void* nativeSRV, uint32_t width, uint32_t height, ETextureFormat format) {
    auto texture = new CDX11Texture(
        static_cast<ID3D11Texture2D*>(nativeTexture),
        width,
        height,
        format
    );

    if (nativeSRV) {
        texture->SetSRV(static_cast<ID3D11ShaderResourceView*>(nativeSRV));
    }

    return texture;
}

ISampler* CDX11RenderContext::CreateSampler(const SamplerDesc& desc) {
    CDX11Context& ctx = CDX11Context::Instance();

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = ToD3D11Filter(desc.filter);
    samplerDesc.AddressU = ToD3D11AddressMode(desc.addressU);
    samplerDesc.AddressV = ToD3D11AddressMode(desc.addressV);
    samplerDesc.AddressW = ToD3D11AddressMode(desc.addressW);
    samplerDesc.MipLODBias = desc.mipLODBias;
    samplerDesc.MaxAnisotropy = desc.maxAnisotropy;
    samplerDesc.ComparisonFunc = ToD3D11ComparisonFunc(desc.comparisonFunc);
    samplerDesc.BorderColor[0] = desc.borderColor[0];
    samplerDesc.BorderColor[1] = desc.borderColor[1];
    samplerDesc.BorderColor[2] = desc.borderColor[2];
    samplerDesc.BorderColor[3] = desc.borderColor[3];
    samplerDesc.MinLOD = desc.minLOD;
    samplerDesc.MaxLOD = desc.maxLOD;

    ID3D11SamplerState* sampler = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateSamplerState(&samplerDesc, &sampler);
    if (FAILED(hr)) {
        return nullptr;
    }

    return new CDX11Sampler(sampler);
}

IShader* CDX11RenderContext::CreateShader(const ShaderDesc& desc) {
    CDX11Context& ctx = CDX11Context::Instance();
    auto shader = new CDX11Shader(desc.type);

    // Shader is already compiled (bytecode provided)
    if (!desc.bytecode || desc.bytecodeSize == 0) {
        delete shader;
        return nullptr;
    }

    HRESULT hr = S_OK;

    // Create shader object based on type
    switch (desc.type) {
        case EShaderType::Vertex: {
            ID3D11VertexShader* vs = nullptr;
            hr = ctx.GetDevice()->CreateVertexShader(desc.bytecode, desc.bytecodeSize, nullptr, &vs);
            if (SUCCEEDED(hr)) {
                // For vertex shaders, we need to keep the bytecode for input layout creation
                ID3DBlob* blob = nullptr;
                D3DCreateBlob(desc.bytecodeSize, &blob);
                if (blob) {
                    memcpy(blob->GetBufferPointer(), desc.bytecode, desc.bytecodeSize);
                    shader->SetVertexShader(vs, blob);
                } else {
                    shader->SetVertexShader(vs, nullptr);
                }
            }
            break;
        }
        case EShaderType::Pixel: {
            ID3D11PixelShader* ps = nullptr;
            hr = ctx.GetDevice()->CreatePixelShader(desc.bytecode, desc.bytecodeSize, nullptr, &ps);
            if (SUCCEEDED(hr)) {
                shader->SetPixelShader(ps);
            }
            break;
        }
        case EShaderType::Compute: {
            ID3D11ComputeShader* cs = nullptr;
            hr = ctx.GetDevice()->CreateComputeShader(desc.bytecode, desc.bytecodeSize, nullptr, &cs);
            if (SUCCEEDED(hr)) {
                shader->SetComputeShader(cs);
            }
            break;
        }
        case EShaderType::Geometry: {
            ID3D11GeometryShader* gs = nullptr;
            hr = ctx.GetDevice()->CreateGeometryShader(desc.bytecode, desc.bytecodeSize, nullptr, &gs);
            if (SUCCEEDED(hr)) {
                shader->SetGeometryShader(gs);
            }
            break;
        }
        case EShaderType::Hull: {
            ID3D11HullShader* hs = nullptr;
            hr = ctx.GetDevice()->CreateHullShader(desc.bytecode, desc.bytecodeSize, nullptr, &hs);
            if (SUCCEEDED(hr)) {
                shader->SetHullShader(hs);
            }
            break;
        }
        case EShaderType::Domain: {
            ID3D11DomainShader* ds = nullptr;
            hr = ctx.GetDevice()->CreateDomainShader(desc.bytecode, desc.bytecodeSize, nullptr, &ds);
            if (SUCCEEDED(hr)) {
                shader->SetDomainShader(ds);
            }
            break;
        }
    }

    if (FAILED(hr)) {
        delete shader;
        return nullptr;
    }

    return shader;
}

IPipelineState* CDX11RenderContext::CreatePipelineState(const PipelineStateDesc& desc) {
    CDX11Context& ctx = CDX11Context::Instance();
    auto pso = new CDX11PipelineState();

    // Create input layout
    if (desc.vertexShader && !desc.inputLayout.empty()) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> d3dElements;
        for (const auto& elem : desc.inputLayout) {
            D3D11_INPUT_ELEMENT_DESC d3dElem = {};
            d3dElem.SemanticName = ToD3D11SemanticName(elem.semantic);
            d3dElem.SemanticIndex = elem.semanticIndex;
            d3dElem.Format = ToD3D11VertexFormat(elem.format);
            d3dElem.InputSlot = elem.inputSlot;
            d3dElem.AlignedByteOffset = elem.offset;
            d3dElem.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            d3dElem.InstanceDataStepRate = 0;
            d3dElements.push_back(d3dElem);
        }

        CDX11Shader* vs = static_cast<CDX11Shader*>(desc.vertexShader);
        ID3DBlob* vsBlob = vs->GetBlob();

        if (vsBlob) {
            ID3D11InputLayout* inputLayout = nullptr;
            HRESULT hr = ctx.GetDevice()->CreateInputLayout(
                d3dElements.data(),
                static_cast<UINT>(d3dElements.size()),
                vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(),
                &inputLayout
            );

            if (SUCCEEDED(hr)) {
                pso->SetInputLayout(inputLayout);
            }
        }
    }

    // Create rasterizer state
    {
        D3D11_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = ToD3D11FillMode(desc.rasterizer.fillMode);
        rasterizerDesc.CullMode = ToD3D11CullMode(desc.rasterizer.cullMode);
        rasterizerDesc.FrontCounterClockwise = desc.rasterizer.frontCounterClockwise;
        rasterizerDesc.DepthBias = desc.rasterizer.depthBias;
        rasterizerDesc.DepthBiasClamp = desc.rasterizer.depthBiasClamp;
        rasterizerDesc.SlopeScaledDepthBias = desc.rasterizer.slopeScaledDepthBias;
        rasterizerDesc.DepthClipEnable = desc.rasterizer.depthClipEnable;
        rasterizerDesc.ScissorEnable = desc.rasterizer.scissorEnable;
        rasterizerDesc.MultisampleEnable = desc.rasterizer.multisampleEnable;
        rasterizerDesc.AntialiasedLineEnable = desc.rasterizer.antialiasedLineEnable;

        ID3D11RasterizerState* rasterizerState = nullptr;
        HRESULT hr = ctx.GetDevice()->CreateRasterizerState(&rasterizerDesc, &rasterizerState);
        if (SUCCEEDED(hr)) {
            pso->SetRasterizerState(rasterizerState);
        }
    }

    // Create depth stencil state
    {
        D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
        depthStencilDesc.DepthEnable = desc.depthStencil.depthEnable;
        depthStencilDesc.DepthWriteMask = desc.depthStencil.depthWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depthStencilDesc.DepthFunc = ToD3D11ComparisonFunc(desc.depthStencil.depthFunc);
        depthStencilDesc.StencilEnable = desc.depthStencil.stencilEnable;
        depthStencilDesc.StencilReadMask = desc.depthStencil.stencilReadMask;
        depthStencilDesc.StencilWriteMask = desc.depthStencil.stencilWriteMask;

        ID3D11DepthStencilState* depthStencilState = nullptr;
        HRESULT hr = ctx.GetDevice()->CreateDepthStencilState(&depthStencilDesc, &depthStencilState);
        if (SUCCEEDED(hr)) {
            pso->SetDepthStencilState(depthStencilState);
        }
    }

    // Create blend state
    {
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;

        // Only use first RT blend settings (simplified)
        blendDesc.RenderTarget[0].BlendEnable = desc.blend.blendEnable;
        blendDesc.RenderTarget[0].SrcBlend = ToD3D11Blend(desc.blend.srcBlend);
        blendDesc.RenderTarget[0].DestBlend = ToD3D11Blend(desc.blend.dstBlend);
        blendDesc.RenderTarget[0].BlendOp = ToD3D11BlendOp(desc.blend.blendOp);
        blendDesc.RenderTarget[0].SrcBlendAlpha = ToD3D11Blend(desc.blend.srcBlendAlpha);
        blendDesc.RenderTarget[0].DestBlendAlpha = ToD3D11Blend(desc.blend.dstBlendAlpha);
        blendDesc.RenderTarget[0].BlendOpAlpha = ToD3D11BlendOp(desc.blend.blendOpAlpha);
        blendDesc.RenderTarget[0].RenderTargetWriteMask = desc.blend.renderTargetWriteMask;

        ID3D11BlendState* blendState = nullptr;
        HRESULT hr = ctx.GetDevice()->CreateBlendState(&blendDesc, &blendState);
        if (SUCCEEDED(hr)) {
            pso->SetBlendState(blendState);
        }
    }

    // Set topology
    pso->SetTopology(ToD3D11Topology(desc.primitiveTopology));

    // Set shaders
    if (desc.vertexShader) {
        pso->SetVertexShader(static_cast<CDX11Shader*>(desc.vertexShader));
    }
    if (desc.pixelShader) {
        pso->SetPixelShader(static_cast<CDX11Shader*>(desc.pixelShader));
    }
    if (desc.geometryShader) {
        pso->SetGeometryShader(static_cast<CDX11Shader*>(desc.geometryShader));
    }
    if (desc.hullShader) {
        pso->SetHullShader(static_cast<CDX11Shader*>(desc.hullShader));
    }
    if (desc.domainShader) {
        pso->SetDomainShader(static_cast<CDX11Shader*>(desc.domainShader));
    }

    return pso;
}

IPipelineState* CDX11RenderContext::CreateComputePipelineState(const ComputePipelineDesc& desc) {
    auto pso = new CDX11PipelineState();

    if (desc.computeShader) {
        pso->SetComputeShader(static_cast<CDX11Shader*>(desc.computeShader));
    }

    return pso;
}

ITexture* CDX11RenderContext::GetBackbuffer() {
    return m_backbufferWrapper.get();
}

ITexture* CDX11RenderContext::GetDepthStencil() {
    return m_depthStencilWrapper.get();
}

uint32_t CDX11RenderContext::GetWidth() const {
    return CDX11Context::Instance().GetWidth();
}

uint32_t CDX11RenderContext::GetHeight() const {
    return CDX11Context::Instance().GetHeight();
}

void* CDX11RenderContext::GetNativeDevice() {
    return CDX11Context::Instance().GetDevice();
}

void* CDX11RenderContext::GetNativeContext() {
    return CDX11Context::Instance().GetContext();
}

} // namespace DX11
} // namespace RHI
