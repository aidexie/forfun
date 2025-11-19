#include "IBLGenerator.h"
#include "Core/DX11Context.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <filesystem>
#include <dxgiformat.h>

using Microsoft::WRL::ComPtr;

// DDS file format structures
#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

// DDS constants
#define DDS_MAGIC 0x20534444  // "DDS "
#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_MIPMAPCOUNT 0x20000
#define DDSD_LINEARSIZE 0x80000
#define DDSCAPS_TEXTURE 0x1000
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS2_CUBEMAP 0x200
#define DDSCAPS2_CUBEMAP_ALLFACES 0xFC00
#define DDPF_FOURCC 0x4
#define D3D10_RESOURCE_DIMENSION_TEXTURE2D 3

// Helper to load shader source
static std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cout << "ERROR: Failed to open shader: " << filepath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool IBLGenerator::Initialize() {
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    if (!device) return false;

    createFullscreenQuad();
    createIrradianceShader();

    // Create sampler state
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sampDesc, m_sampler.GetAddressOf());

    // Create constant buffer for face index
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 16;  // int + padding
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbDesc, nullptr, m_cbFaceIndex.GetAddressOf());

    return true;
}

void IBLGenerator::Shutdown() {
    m_fullscreenVS.Reset();
    m_irradiancePS.Reset();
    m_fullscreenVB.Reset();
    m_sampler.Reset();
    m_cbFaceIndex.Reset();
    m_irradianceTexture.Reset();
    m_irradianceSRV.Reset();
}

void IBLGenerator::createFullscreenQuad() {
    // No vertex buffer needed - using SV_VertexID trick in shader
}

void IBLGenerator::createIrradianceShader() {
    ID3D11Device* device = DX11Context::Instance().GetDevice();

    std::string vsSource = LoadShaderSource("../source/code/Shader/IrradianceConvolution.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/IrradianceConvolution.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        std::cout << "ERROR: Failed to load irradiance shaders!" << std::endl;
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> vsBlob, psBlob, err;

    // Compile VS
    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "IrradianceConvolution.vs.hlsl",
                            nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            std::cout << "=== IRRADIANCE VS COMPILATION ERROR ===" << std::endl;
            std::cout << (const char*)err->GetBufferPointer() << std::endl;
        }
        return;
    }

    // Compile PS
    hr = D3DCompile(psSource.c_str(), psSource.size(), "IrradianceConvolution.ps.hlsl",
                    nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            std::cout << "=== IRRADIANCE PS COMPILATION ERROR ===" << std::endl;
            std::cout << (const char*)err->GetBufferPointer() << std::endl;
        }
        return;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_fullscreenVS.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_irradiancePS.GetAddressOf());
}

ID3D11ShaderResourceView* IBLGenerator::GenerateIrradianceMap(
    ID3D11ShaderResourceView* envMap,
    int outputSize)
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    ID3D11DeviceContext* context = DX11Context::Instance().GetContext();

    if (!device || !context || !envMap) return nullptr;

    // Create output cubemap texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = outputSize;
    texDesc.Height = outputSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 6;  // 6 faces
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR format
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    device->CreateTexture2D(&texDesc, nullptr, m_irradianceTexture.ReleaseAndGetAddressOf());

    // Create SRV for the whole cubemap
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;
    device->CreateShaderResourceView(m_irradianceTexture.Get(), &srvDesc, m_irradianceSRV.ReleaseAndGetAddressOf());

    // Render to each cubemap face
    for (int face = 0; face < 6; ++face) {
        // Create RTV for this face
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = texDesc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.ArraySize = 1;

        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateRenderTargetView(m_irradianceTexture.Get(), &rtvDesc, rtv.GetAddressOf());

        // Set render target
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        // Set viewport
        D3D11_VIEWPORT vp = {};
        vp.Width = (float)outputSize;
        vp.Height = (float)outputSize;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        // Update face index constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        context->Map(m_cbFaceIndex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        *(int*)mapped.pData = face;
        context->Unmap(m_cbFaceIndex.Get(), 0);

        // Set shaders and resources
        context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
        context->PSSetShader(m_irradiancePS.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &envMap);
        context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, m_cbFaceIndex.GetAddressOf());

        // Draw fullscreen triangle (3 vertices, no vertex buffer)
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->Draw(3, 0);
    }

    // Cleanup
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);

    std::cout << "IBL: Irradiance map generated successfully!" << std::endl;

    return m_irradianceSRV.Get();
}

bool IBLGenerator::SaveIrradianceMapToDDS(const std::string& filepath) {
    if (!m_irradianceTexture) {
        std::cout << "ERROR: No irradiance map to save!" << std::endl;
        return false;
    }

    ID3D11Device* device = DX11Context::Instance().GetDevice();
    ID3D11DeviceContext* context = DX11Context::Instance().GetContext();

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    m_irradianceTexture->GetDesc(&desc);

    std::cout << "IBL: Saving irradiance map (" << desc.Width << "x" << desc.Height << " x 6 faces)..." << std::endl;

    // Create staging texture for readback
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf());
    if (FAILED(hr)) {
        std::cout << "ERROR: Failed to create staging texture!" << std::endl;
        return false;
    }

    // Copy to staging texture
    context->CopyResource(stagingTexture.Get(), m_irradianceTexture.Get());

    // Prepare DDS header
    uint32_t magic = DDS_MAGIC;

    DDS_HEADER header = {};
    header.dwSize = 124;
    header.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
    header.dwHeight = desc.Height;
    header.dwWidth = desc.Width;
    header.dwPitchOrLinearSize = desc.Width * desc.Height * 8;  // R16G16B16A16 = 8 bytes/pixel
    header.dwMipMapCount = 1;

    // Pixel format (DX10 extended header)
    header.ddspf.dwSize = 32;
    header.ddspf.dwFlags = DDPF_FOURCC;
    header.ddspf.dwFourCC = 0x30315844;  // "DX10"

    header.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX;
    header.dwCaps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES;

    // DX10 extended header
    DDS_HEADER_DXT10 header10 = {};
    header10.dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    header10.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
    header10.miscFlag = 0x4;  // RESOURCE_MISC_TEXTURECUBE
    header10.arraySize = 1;
    header10.miscFlags2 = 0;

    // Create output directory if needed
    std::filesystem::path fullPath = filepath;
    if (fullPath.is_relative()) {
        fullPath = std::filesystem::current_path() / filepath;
    }
    std::filesystem::create_directories(fullPath.parent_path());

    // Open file
    std::ofstream file(fullPath, std::ios::binary);
    if (!file) {
        std::cout << "ERROR: Failed to create file: " << fullPath << std::endl;
        return false;
    }

    // Write headers
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&header10), sizeof(header10));

    // Read and write pixel data for all 6 faces
    for (UINT face = 0; face < 6; ++face) {
        // Map staging texture for this face
        UINT subresource = D3D11CalcSubresource(0, face, 1);
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(stagingTexture.Get(), subresource, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            std::cout << "ERROR: Failed to map staging texture face " << face << std::endl;
            file.close();
            return false;
        }

        // Write pixel data
        const char* srcData = reinterpret_cast<const char*>(mapped.pData);
        size_t rowPitch = desc.Width * 8;  // 8 bytes per pixel
        for (UINT row = 0; row < desc.Height; ++row) {
            file.write(srcData + row * mapped.RowPitch, rowPitch);
        }

        context->Unmap(stagingTexture.Get(), subresource);

        std::cout << "IBL: Wrote face " << face << std::endl;
    }

    file.close();

    std::cout << "IBL: Successfully saved to " << fullPath << std::endl;
    return true;
}

ID3D11ShaderResourceView* IBLGenerator::GetIrradianceFaceSRV(int faceIndex) {
    if (faceIndex < 0 || faceIndex >= 6) return nullptr;
    if (!m_irradianceTexture) return nullptr;

    // Create face SRV on-demand
    if (!m_debugFaceSRVs[faceIndex]) {
        ID3D11Device* device = DX11Context::Instance().GetDevice();

        D3D11_TEXTURE2D_DESC desc;
        m_irradianceTexture->GetDesc(&desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = faceIndex;
        srvDesc.Texture2DArray.ArraySize = 1;

        device->CreateShaderResourceView(m_irradianceTexture.Get(), &srvDesc,
                                        m_debugFaceSRVs[faceIndex].GetAddressOf());
    }

    return m_debugFaceSRVs[faceIndex].Get();
}
