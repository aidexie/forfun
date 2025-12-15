#include "DX11RenderContext.h"
#include "DX11Utils.h"
#include "Core/FFLog.h"
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
    m_commandList = std::make_unique<CDX11CommandList>(ctx.GetContext(), ctx.GetDevice());

    // Wrap backbuffer - need to get actual texture from swap chain
    IDXGISwapChain* swapChain = ctx.GetSwapChain();
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr)) {
        return false;
    }

    // Create TextureDesc for backbuffer
    TextureDesc backbufferDesc = TextureDesc::Texture2D(width, height, ETextureFormat::R8G8B8A8_UNORM,
                                                         ETextureUsage::RenderTarget);
    m_backbufferWrapper = std::make_unique<CDX11Texture>(backbufferDesc, backbuffer, ctx.GetDevice(), ctx.GetContext());
    // Set the RTV from existing swap chain backbuffer
    m_backbufferWrapper->SetRTV(ctx.GetBackbufferRTV());
    backbuffer->Release();  // CDX11Texture owns it now

    // Wrap depth stencil
    TextureDesc depthDesc = TextureDesc::DepthStencil(width, height);
    m_depthStencilWrapper = std::make_unique<CDX11Texture>(depthDesc, (ID3D11Texture2D*)nullptr, ctx.GetDevice(), ctx.GetContext());
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
        TextureDesc backbufferDesc = TextureDesc::Texture2D(width, height, ETextureFormat::R8G8B8A8_UNORM,
                                                             ETextureUsage::RenderTarget);
        m_backbufferWrapper = std::make_unique<CDX11Texture>(backbufferDesc, backbuffer, ctx.GetDevice(), ctx.GetContext());
        m_backbufferWrapper->SetRTV(ctx.GetBackbufferRTV());
        backbuffer->Release();
    }

    // Re-wrap depth stencil
    TextureDesc depthDesc = TextureDesc::DepthStencil(width, height);
    m_depthStencilWrapper = std::make_unique<CDX11Texture>(depthDesc, (ID3D11Texture2D*)nullptr, ctx.GetDevice(), ctx.GetContext());
    m_depthStencilWrapper->SetDSV(ctx.GetDSV());
}

void CDX11RenderContext::BeginFrame() {
    // Reset dynamic constant buffer pool indices
    m_commandList->ResetFrame();
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
        CFFLog::Error("CreateBuffer failed: %s", HRESULTToString(hr).c_str());
        CFFLog::Error("  Size: %u bytes", desc.size);
        CFFLog::Error("  Usage: %s", BufferUsageToString(desc.usage));
        CFFLog::Error("  CPUAccess: %s", CPUAccessToString(desc.cpuAccess));
        CFFLog::Error("  StructureByteStride: %u", desc.structureByteStride);
        CFFLog::Error("  D3D11 BindFlags: 0x%X, Usage: %d, CPUAccessFlags: 0x%X",
                      bufferDesc.BindFlags, bufferDesc.Usage, bufferDesc.CPUAccessFlags);
        return nullptr;
    }

    return new CDX11Buffer(d3dBuffer, desc, ctx.GetDevice(), ctx.GetContext());
}

ITexture* CDX11RenderContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    CDX11Context& ctx = CDX11Context::Instance();

    // Handle Staging texture specially
    const bool isStaging = (desc.usage & ETextureUsage::Staging);

    // Determine dimension from desc
    ETextureDimension dimension = desc.dimension;
    // Legacy compatibility: convert isCubemap/isCubemapArray to dimension
    if (desc.isCubemap && dimension == ETextureDimension::Tex2D) {
        dimension = ETextureDimension::TexCube;
    } else if (desc.isCubemapArray && dimension == ETextureDimension::Tex2D) {
        dimension = ETextureDimension::TexCubeArray;
    }

    // Check if we need to generate mipmaps
    const bool needsGenerateMips = (desc.miscFlags & ETextureMiscFlags::GenerateMips) || desc.mipLevels == 0;

    // 3D Texture path
    if (dimension == ETextureDimension::Tex3D) {
        D3D11_TEXTURE3D_DESC tex3DDesc = {};
        tex3DDesc.Width = desc.width;
        tex3DDesc.Height = desc.height;
        tex3DDesc.Depth = desc.depth;
        tex3DDesc.MipLevels = desc.mipLevels;
        tex3DDesc.Format = ToDXGIFormat(desc.format);
        tex3DDesc.Usage = D3D11_USAGE_DEFAULT;
        tex3DDesc.BindFlags = ToD3D11BindFlags(desc.usage);
        tex3DDesc.CPUAccessFlags = 0;
        tex3DDesc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA* pInitData = nullptr;
        D3D11_SUBRESOURCE_DATA initData = {};
        if (initialData) {
            initData.pSysMem = initialData;
            uint32_t bytesPerPixel = GetBytesPerPixel(desc.format);
            if (bytesPerPixel == 0) bytesPerPixel = 4;  // Default
            initData.SysMemPitch = desc.width * bytesPerPixel;
            initData.SysMemSlicePitch = desc.width * desc.height * bytesPerPixel;
            pInitData = &initData;
        }

        ID3D11Texture3D* d3dTexture3D = nullptr;
        HRESULT hr = ctx.GetDevice()->CreateTexture3D(&tex3DDesc, pInitData, &d3dTexture3D);
        if (FAILED(hr)) {
            CFFLog::Error("CreateTexture3D failed: %s", HRESULTToString(hr).c_str());
            CFFLog::Error("  Size: %ux%ux%u, MipLevels: %u, Format: %d",
                          desc.width, desc.height, desc.depth, desc.mipLevels, (int)desc.format);
            return nullptr;
        }

        // Create TextureDesc with correct dimension
        TextureDesc texDesc = desc;
        texDesc.dimension = ETextureDimension::Tex3D;
        return new CDX11Texture(texDesc, d3dTexture3D, ctx.GetDevice(), ctx.GetContext());
    }

    // 2D Texture path (includes 2D, 2DArray, Cube, CubeArray)

    // Calculate D3D11 array size
    uint32_t d3dArraySize = desc.arraySize;
    if (dimension == ETextureDimension::TexCube) {
        d3dArraySize = 6;  // Single cubemap = 6 faces
    } else if (dimension == ETextureDimension::TexCubeArray) {
        d3dArraySize = desc.arraySize * 6;  // Cubemap array = N cubes * 6 faces
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = desc.width;
    texDesc.Height = desc.height;
    texDesc.MipLevels = desc.mipLevels;
    texDesc.ArraySize = d3dArraySize;
    texDesc.Format = ToDXGIFormat(desc.format);
    texDesc.SampleDesc.Count = desc.sampleCount;
    texDesc.SampleDesc.Quality = 0;

    if (isStaging) {
        texDesc.Usage = D3D11_USAGE_STAGING;
        texDesc.BindFlags = 0;
        texDesc.CPUAccessFlags = (desc.cpuAccess == ECPUAccess::Write)
            ? D3D11_CPU_ACCESS_WRITE
            : D3D11_CPU_ACCESS_READ;
        texDesc.MiscFlags = 0;
    } else {
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = ToD3D11BindFlags(desc.usage);
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = 0;

        // Cubemap flag
        if (dimension == ETextureDimension::TexCube || dimension == ETextureDimension::TexCubeArray) {
            texDesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        }

        // Generate mips flag
        if (needsGenerateMips) {
            texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (desc.mipLevels == 0) {
                texDesc.MipLevels = 0;  // Let D3D11 calculate
            }
        }
    }

    // Initial data handling:
    // - For GenerateMips textures: don't use pInitData (use UpdateSubresource after creation)
    // - For staging textures: don't use pInitData
    // - For normal textures with mipLevels > 0: can use pInitData for mip 0
    D3D11_SUBRESOURCE_DATA* pInitData = nullptr;
    D3D11_SUBRESOURCE_DATA initData = {};
    if (initialData && !isStaging && !needsGenerateMips) {
        initData.pSysMem = initialData;
        uint32_t bytesPerPixel = GetBytesPerPixel(desc.format);
        if (bytesPerPixel == 0) bytesPerPixel = 4;
        initData.SysMemPitch = desc.width * bytesPerPixel;
        pInitData = &initData;
    }

    ID3D11Texture2D* d3dTexture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&texDesc, pInitData, &d3dTexture);
    if (FAILED(hr)) {
        CFFLog::Error("CreateTexture2D failed: %s", HRESULTToString(hr).c_str());
        CFFLog::Error("  Size: %ux%u, ArraySize: %u, MipLevels: %u, Format: %d",
                      desc.width, desc.height, d3dArraySize, desc.mipLevels, (int)desc.format);
        CFFLog::Error("  Usage: 0x%X, BindFlags: 0x%X, MiscFlags: 0x%X",
                      texDesc.Usage, texDesc.BindFlags, texDesc.MiscFlags);
        return nullptr;
    }

    // Get actual mip levels after creation (if 0 was specified)
    D3D11_TEXTURE2D_DESC createdDesc;
    d3dTexture->GetDesc(&createdDesc);

    // Build final TextureDesc with actual values
    TextureDesc finalDesc = desc;
    finalDesc.dimension = dimension;
    finalDesc.mipLevels = createdDesc.MipLevels;
    finalDesc.arraySize = (dimension == ETextureDimension::TexCubeArray) ? desc.arraySize : d3dArraySize;

    // For GenerateMips textures: upload initial data to mip 0 via UpdateSubresource
    if (initialData && needsGenerateMips && !isStaging) {
        uint32_t bytesPerPixel = GetBytesPerPixel(desc.format);
        if (bytesPerPixel == 0) bytesPerPixel = 4;
        uint32_t rowPitch = desc.width * bytesPerPixel;

        // Upload to subresource 0 (mip 0, array slice 0)
        ctx.GetContext()->UpdateSubresource(
            d3dTexture,
            0,  // Subresource 0 = mip 0
            nullptr,  // Entire subresource
            initialData,
            rowPitch,
            0  // Not used for 2D textures
        );
    }

    // Views are created on-demand by CDX11Texture::GetOrCreateXXX()
    return new CDX11Texture(finalDesc, d3dTexture, ctx.GetDevice(), ctx.GetContext());
}

ITexture* CDX11RenderContext::CreateTextureWithData(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources) {
    if (!subresources || numSubresources == 0) {
        return CreateTexture(desc, nullptr);
    }

    CDX11Context& ctx = CDX11Context::Instance();

    // Determine dimension
    ETextureDimension dimension = desc.dimension;
    if (desc.isCubemap && dimension == ETextureDimension::Tex2D) {
        dimension = ETextureDimension::TexCube;
    } else if (desc.isCubemapArray && dimension == ETextureDimension::Tex2D) {
        dimension = ETextureDimension::TexCubeArray;
    }

    // Calculate D3D11 array size
    uint32_t d3dArraySize = desc.arraySize;
    if (dimension == ETextureDimension::TexCube) {
        d3dArraySize = 6;
    } else if (dimension == ETextureDimension::TexCubeArray) {
        d3dArraySize = desc.arraySize * 6;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = desc.width;
    texDesc.Height = desc.height;
    texDesc.MipLevels = desc.mipLevels;
    texDesc.ArraySize = d3dArraySize;
    texDesc.Format = ToDXGIFormat(desc.format);
    texDesc.SampleDesc.Count = desc.sampleCount;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = ToD3D11BindFlags(desc.usage);
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    if (dimension == ETextureDimension::TexCube || dimension == ETextureDimension::TexCubeArray) {
        texDesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    }

    // Convert SubresourceData array to D3D11_SUBRESOURCE_DATA
    std::vector<D3D11_SUBRESOURCE_DATA> initData(numSubresources);
    for (uint32_t i = 0; i < numSubresources; ++i) {
        initData[i].pSysMem = subresources[i].pData;
        initData[i].SysMemPitch = subresources[i].rowPitch;
        initData[i].SysMemSlicePitch = subresources[i].slicePitch;
    }

    ID3D11Texture2D* d3dTexture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&texDesc, initData.data(), &d3dTexture);
    if (FAILED(hr)) {
        CFFLog::Error("CreateTexture2D (with data) failed: %s", HRESULTToString(hr).c_str());
        CFFLog::Error("  Size: %ux%u, ArraySize: %u, MipLevels: %u, Format: %d, Subresources: %u",
                      desc.width, desc.height, d3dArraySize, desc.mipLevels, (int)desc.format, numSubresources);
        return nullptr;
    }

    // Build final TextureDesc
    TextureDesc finalDesc = desc;
    finalDesc.dimension = dimension;
    finalDesc.arraySize = (dimension == ETextureDimension::TexCubeArray) ? desc.arraySize : d3dArraySize;

    // Views are created on-demand
    return new CDX11Texture(finalDesc, d3dTexture, ctx.GetDevice(), ctx.GetContext());
}

ITexture* CDX11RenderContext::WrapNativeTexture(void* nativeTexture, void* nativeSRV, uint32_t width, uint32_t height, ETextureFormat format) {
    CDX11Context& ctx = CDX11Context::Instance();

    TextureDesc desc = TextureDesc::Texture2D(width, height, format, ETextureUsage::ShaderResource);
    auto texture = new CDX11Texture(desc,
        static_cast<ID3D11Texture2D*>(nativeTexture),
        ctx.GetDevice(),
        ctx.GetContext()
    );

    if (nativeSRV) {
        texture->SetSRV(static_cast<ID3D11ShaderResourceView*>(nativeSRV));
    }

    return texture;
}

ITexture* CDX11RenderContext::WrapExternalTexture(void* nativeTexture, const TextureDesc& desc) {
    CDX11Context& ctx = CDX11Context::Instance();

    // Determine dimension
    ETextureDimension dimension = desc.dimension;
    if (desc.isCubemap && dimension == ETextureDimension::Tex2D) {
        dimension = ETextureDimension::TexCube;
    } else if (desc.isCubemapArray && dimension == ETextureDimension::Tex2D) {
        dimension = ETextureDimension::TexCubeArray;
    }

    TextureDesc finalDesc = desc;
    finalDesc.dimension = dimension;

    // Calculate array size for cubemaps
    if (dimension == ETextureDimension::TexCube) {
        finalDesc.arraySize = 6;
    } else if (dimension == ETextureDimension::TexCubeArray) {
        // arraySize already represents number of cubes, keep it
    }

    return new CDX11Texture(finalDesc,
        static_cast<ID3D11Texture2D*>(nativeTexture),
        ctx.GetDevice(),
        ctx.GetContext()
    );
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
        CFFLog::Error("CreateSamplerState failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    return new CDX11Sampler(sampler);
}

static const char* ShaderTypeToString(EShaderType type) {
    switch (type) {
        case EShaderType::Vertex: return "Vertex";
        case EShaderType::Pixel: return "Pixel";
        case EShaderType::Compute: return "Compute";
        case EShaderType::Geometry: return "Geometry";
        case EShaderType::Hull: return "Hull";
        case EShaderType::Domain: return "Domain";
        default: return "Unknown";
    }
}

IShader* CDX11RenderContext::CreateShader(const ShaderDesc& desc) {
    CDX11Context& ctx = CDX11Context::Instance();
    auto shader = new CDX11Shader(desc.type);

    // Shader is already compiled (bytecode provided)
    if (!desc.bytecode || desc.bytecodeSize == 0) {
        CFFLog::Error("CreateShader failed: No bytecode provided (type: %s)", ShaderTypeToString(desc.type));
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
                    CFFLog::Error("D3DCreateBlob failed for vertex shader bytecode");
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
        CFFLog::Error("CreateShader failed: %s (type: %s, bytecodeSize: %zu)",
                      HRESULTToString(hr).c_str(), ShaderTypeToString(desc.type), desc.bytecodeSize);
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
            } else {
                CFFLog::Error("CreateInputLayout failed: %s", HRESULTToString(hr).c_str());
                CFFLog::Error("  Input layout has %zu elements", d3dElements.size());
                for (size_t i = 0; i < d3dElements.size(); ++i) {
                    CFFLog::Error("    [%zu] %s%u @ offset %u",
                        i, d3dElements[i].SemanticName, d3dElements[i].SemanticIndex,
                        d3dElements[i].AlignedByteOffset);
                }
            }
        } else {
            CFFLog::Error("CreateInputLayout failed: Vertex shader has no blob (bytecode not preserved)");
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
        } else {
            CFFLog::Error("CreateRasterizerState failed: %s", HRESULTToString(hr).c_str());
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
        } else {
            CFFLog::Error("CreateDepthStencilState failed: %s", HRESULTToString(hr).c_str());
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
        } else {
            CFFLog::Error("CreateBlendState failed: %s", HRESULTToString(hr).c_str());
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
