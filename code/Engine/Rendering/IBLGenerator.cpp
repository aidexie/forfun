#include "IBLGenerator.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/FFLog.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <dxgiformat.h>
#include <DirectXPackedVector.h>  // For XMHALF4, XMLoadHalf4

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
        CFFLog::Error("Failed to open shader: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool CIBLGenerator::Initialize() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return false;

    createFullscreenQuad();
    createIrradianceShader();
    createPreFilterShader();
    createBrdfLutShader();

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

    // Create constant buffer for roughness
    cbDesc.ByteWidth = 16;  // float + float + padding
    device->CreateBuffer(&cbDesc, nullptr, m_cbRoughness.GetAddressOf());

    return true;
}

void CIBLGenerator::Shutdown() {
    m_fullscreenVS.Reset();
    m_irradiancePS.Reset();
    m_prefilterPS.Reset();
    m_brdfLutPS.Reset();
    m_fullscreenVB.Reset();
    m_sampler.Reset();
    m_cbFaceIndex.Reset();
    m_cbRoughness.Reset();
    m_irradianceTexture.Reset();
    m_irradianceSRV.Reset();
    m_preFilteredTexture.Reset();
    m_preFilteredSRV.Reset();
    m_brdfLutTexture.Reset();
    m_brdfLutSRV.Reset();
}

void CIBLGenerator::createFullscreenQuad() {
    // No vertex buffer needed - using SV_VertexID trick in shader
}

void CIBLGenerator::createIrradianceShader() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());

    std::string vsSource = LoadShaderSource("../source/code/Shader/IrradianceConvolution.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/IrradianceConvolution.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load irradiance shaders!");
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
            CFFLog::Error("=== IRRADIANCE VS COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Compile PS
    hr = D3DCompile(psSource.c_str(), psSource.size(), "IrradianceConvolution.ps.hlsl",
                    nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== IRRADIANCE PS COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_fullscreenVS.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_irradiancePS.GetAddressOf());
}

void CIBLGenerator::createPreFilterShader() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());

    std::string vsSource = LoadShaderSource("../source/code/Shader/PreFilterEnvironmentMap.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/PreFilterEnvironmentMap.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load pre-filter shaders!");
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> psBlob, err;

    // Compile PS (VS already compiled in createIrradianceShader)
    HRESULT hr = D3DCompile(psSource.c_str(), psSource.size(), "PreFilterEnvironmentMap.ps.hlsl",
                            nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== PREFILTER PS COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_prefilterPS.GetAddressOf());
}

ID3D11ShaderResourceView* CIBLGenerator::GenerateIrradianceMap(
    ID3D11ShaderResourceView* envMap,
    int outputSize)
{
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

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

    CFFLog::Info("IBL: Irradiance map generated successfully!");

    return m_irradianceSRV.Get();
}

bool CIBLGenerator::SaveIrradianceMapToDDS(const std::string& filepath) {
    if (!m_irradianceTexture) {
        CFFLog::Error("No irradiance map to save!");
        return false;
    }

    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    m_irradianceTexture->GetDesc(&desc);

    CFFLog::Info("IBL: Saving irradiance map (%dx%d x 6 faces)...", desc.Width, desc.Height);

    // Create staging texture for readback
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Failed to create staging texture!");
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
        CFFLog::Error("Failed to create file: %s", fullPath.string().c_str());
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
            CFFLog::Error("Failed to map staging texture face %d", face);
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

        CFFLog::Info("IBL: Wrote face %d", face);
    }

    file.close();

    CFFLog::Info("IBL: Successfully saved to %s", fullPath.string().c_str());
    return true;
}

// Helper: Convert float RGB to RGBE (Radiance HDR format)
static void floatToRgbe(float r, float g, float b, unsigned char rgbe[4]) {
    float v = r;
    if (g > v) v = g;
    if (b > v) v = b;

    if (v < 1e-32f) {
        rgbe[0] = rgbe[1] = rgbe[2] = rgbe[3] = 0;
        return;
    }

    int e;
    float m = frexpf(v, &e);  // v = m * 2^e, where 0.5 <= m < 1
    v = m * 256.0f / v;       // Scale factor

    rgbe[0] = (unsigned char)(r * v);
    rgbe[1] = (unsigned char)(g * v);
    rgbe[2] = (unsigned char)(b * v);
    rgbe[3] = (unsigned char)(e + 128);
}

bool CIBLGenerator::SaveIrradianceMapToHDR(const std::string& filepath) {
    if (!m_irradianceTexture) {
        CFFLog::Error("No irradiance map to save!");
        return false;
    }

    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    m_irradianceTexture->GetDesc(&desc);

    CFFLog::Info("IBL: Saving irradiance map to HDR (%dx%d x 6 faces)...", desc.Width, desc.Height);

    // Create staging texture for readback
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Failed to create staging texture!");
        return false;
    }

    // Copy to staging texture
    context->CopyResource(stagingTexture.Get(), m_irradianceTexture.Get());

    // Face names for file naming
    const char* faceNames[6] = { "posX", "negX", "posY", "negY", "posZ", "negZ" };

    // Create output directory if needed
    std::filesystem::path basePath = filepath;
    if (basePath.is_relative()) {
        basePath = std::filesystem::current_path() / filepath;
    }
    std::filesystem::create_directories(basePath.parent_path());

    // Remove extension from base path for adding face suffix
    std::string baseStr = basePath.string();
    size_t dotPos = baseStr.rfind('.');
    if (dotPos != std::string::npos) {
        baseStr = baseStr.substr(0, dotPos);
    }

    // Save each face as a separate HDR file
    for (UINT face = 0; face < 6; ++face) {
        std::string faceFilename = baseStr + "_" + faceNames[face] + ".hdr";

        // Map staging texture for this face
        UINT subresource = D3D11CalcSubresource(0, face, 1);
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(stagingTexture.Get(), subresource, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            CFFLog::Error("Failed to map staging texture face %d", face);
            continue;
        }

        // Open file
        std::ofstream file(faceFilename, std::ios::binary);
        if (!file) {
            CFFLog::Error("Failed to create file: %s", faceFilename.c_str());
            context->Unmap(stagingTexture.Get(), subresource);
            continue;
        }

        // Write HDR header
        file << "#?RADIANCE\n";
        file << "FORMAT=32-bit_rle_rgbe\n";
        file << "\n";
        file << "-Y " << desc.Height << " +X " << desc.Width << "\n";

        // Write pixel data (convert R16G16B16A16_FLOAT to RGBE)
        // HDR files store from top to bottom (same as our texture)
        std::vector<unsigned char> rgbeData(desc.Width * desc.Height * 4);

        const uint16_t* srcData = reinterpret_cast<const uint16_t*>(mapped.pData);
        size_t srcRowPitch = mapped.RowPitch / sizeof(uint16_t);

        for (UINT y = 0; y < desc.Height; ++y) {
            for (UINT x = 0; x < desc.Width; ++x) {
                // R16G16B16A16_FLOAT: 4 half-floats per pixel
                size_t srcOffset = y * srcRowPitch + x * 4;

                // Convert half-float to float (using DirectXPackedVector)
                DirectX::PackedVector::XMHALF4 half4;
                half4.x = srcData[srcOffset + 0];
                half4.y = srcData[srcOffset + 1];
                half4.z = srcData[srcOffset + 2];
                half4.w = srcData[srcOffset + 3];

                DirectX::XMVECTOR v = DirectX::PackedVector::XMLoadHalf4(&half4);
                DirectX::XMFLOAT4 f4;
                DirectX::XMStoreFloat4(&f4, v);

                // Convert to RGBE
                size_t dstOffset = (y * desc.Width + x) * 4;
                floatToRgbe(f4.x, f4.y, f4.z, &rgbeData[dstOffset]);
            }
        }

        // Write all RGBE data
        file.write(reinterpret_cast<const char*>(rgbeData.data()), rgbeData.size());

        context->Unmap(stagingTexture.Get(), subresource);
        file.close();

        CFFLog::Info("IBL: Saved face %s to %s", faceNames[face], faceFilename.c_str());
    }

    CFFLog::Info("IBL: Successfully saved irradiance map to HDR files!");
    return true;
}

ID3D11ShaderResourceView* CIBLGenerator::GetIrradianceFaceSRV(int faceIndex) {
    if (faceIndex < 0 || faceIndex >= 6) return nullptr;
    if (!m_irradianceTexture) return nullptr;

    // Create face SRV on-demand
    if (!m_debugFaceSRVs[faceIndex]) {
        ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());

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

ID3D11ShaderResourceView* CIBLGenerator::GeneratePreFilteredMap(
    ID3D11ShaderResourceView* envMap,
    int outputSize,
    int numMipLevels)
{
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

    if (!device || !context || !envMap) return nullptr;

    // Clamp mip levels to reasonable range
    numMipLevels = std::max(1, std::min(numMipLevels, 10));
    m_preFilteredMipLevels = numMipLevels;

    CFFLog::Info("IBL: Generating pre-filtered map (%dx%d, %d mip levels)...", outputSize, outputSize, numMipLevels);

    // Get environment map description for resolution
    ID3D11Resource* envResource;
    envMap->GetResource(&envResource);
    ComPtr<ID3D11Texture2D> envTexture;
    envResource->QueryInterface(envTexture.GetAddressOf());
    D3D11_TEXTURE2D_DESC envDesc;
    envTexture->GetDesc(&envDesc);
    float envResolution = (float)envDesc.Width;

    CFFLog::Info("IBL: Environment map info: %dx%d, %d mip levels", envDesc.Width, envDesc.Height, envDesc.MipLevels);

    if (envDesc.MipLevels <= 1) {
        CFFLog::Warning("Environment map has no mipmaps! Pre-filtering quality will be poor.");
    }

    envResource->Release();

    // Create output cubemap texture with mipmaps
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = outputSize;
    texDesc.Height = outputSize;
    texDesc.MipLevels = numMipLevels;
    texDesc.ArraySize = 6;  // 6 faces
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR format
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    device->CreateTexture2D(&texDesc, nullptr, m_preFilteredTexture.ReleaseAndGetAddressOf());

    // Create SRV for the whole cubemap
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = numMipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;
    device->CreateShaderResourceView(m_preFilteredTexture.Get(), &srvDesc, m_preFilteredSRV.ReleaseAndGetAddressOf());

    // Render to each mip level
    for (int mip = 0; mip < numMipLevels; ++mip) {
        int mipWidth = outputSize >> mip;   // outputSize / 2^mip
        int mipHeight = outputSize >> mip;

        // Calculate roughness for this mip level (linear mapping)
        float roughness = (numMipLevels != 1) ? (float)mip / (float)(numMipLevels - 1) : 0.0f;

        // Calculate expected sample count (matches shader logic)
        int expectedSamples;
        if (roughness < 0.1f) {
            expectedSamples = 65536;
        } else if (roughness < 0.3f) {
            expectedSamples = 32768;
        } else if (roughness < 0.6f) {
            expectedSamples = 16384;
        } else {
            expectedSamples = 8192;
        }

        CFFLog::Info("  Mip %d: %dx%d, roughness=%.3f, samples=%d", mip, mipWidth, mipHeight, roughness, expectedSamples);

        // Render to each cubemap face at this mip level
        for (int face = 0; face < 6; ++face) {
            // Create RTV for this face and mip level
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = texDesc.Format;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice = mip;
            rtvDesc.Texture2DArray.FirstArraySlice = face;
            rtvDesc.Texture2DArray.ArraySize = 1;

            ComPtr<ID3D11RenderTargetView> rtv;
            HRESULT hr = device->CreateRenderTargetView(m_preFilteredTexture.Get(), &rtvDesc, rtv.GetAddressOf());
            if (FAILED(hr)) {
                CFFLog::Error("Failed to create RTV for face %d, mip %d", face, mip);
                continue;
            }

            // Set render target
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

            // Clear to magenta for debugging (will be overwritten if rendering works)
            const float clearColor[4] = { 1.0f, 0.0f, 1.0f, 1.0f };  // Magenta
            context->ClearRenderTargetView(rtv.Get(), clearColor);

            // Set viewport
            D3D11_VIEWPORT vp = {};
            vp.Width = (float)mipWidth;
            vp.Height = (float)mipHeight;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context->RSSetViewports(1, &vp);

            // Update face index constant buffer
            D3D11_MAPPED_SUBRESOURCE mapped;
            context->Map(m_cbFaceIndex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            *(int*)mapped.pData = face;
            context->Unmap(m_cbFaceIndex.Get(), 0);

            // Update roughness constant buffer
            context->Map(m_cbRoughness.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            float* cbData = (float*)mapped.pData;
            cbData[0] = roughness;
            cbData[1] = envResolution;
            context->Unmap(m_cbRoughness.Get(), 0);

            // Set shaders and resources
            context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            context->PSSetShader(m_prefilterPS.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, &envMap);
            context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
            context->PSSetConstantBuffers(0, 1, m_cbFaceIndex.GetAddressOf());
            context->PSSetConstantBuffers(1, 1, m_cbRoughness.GetAddressOf());

            // Verify shader is valid
            if (!m_fullscreenVS || !m_prefilterPS) {
                CFFLog::Error("Pre-filter shaders not compiled!");
                continue;
            }

            // Draw fullscreen triangle (3 vertices, no vertex buffer)
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(nullptr);
            context->Draw(3, 0);
        }
        CFFLog::Info("    Rendered all 6 faces for mip %d", mip);
    }

    // Cleanup
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);

    CFFLog::Info("IBL: Pre-filtered map generated successfully!");

    return m_preFilteredSRV.Get();
}

ID3D11ShaderResourceView* CIBLGenerator::GetPreFilteredFaceSRV(int faceIndex, int mipLevel) {
    if (faceIndex < 0 || faceIndex >= 6) return nullptr;
    if (mipLevel < 0 || mipLevel >= m_preFilteredMipLevels) return nullptr;
    if (!m_preFilteredTexture) return nullptr;

    int srvIndex = mipLevel * 6 + faceIndex;
    if (srvIndex < 0 || srvIndex >= 6 * 10) return nullptr;

    // Create face SRV on-demand
    if (!m_debugPreFilteredFaceSRVs[srvIndex]) {
        ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());

        D3D11_TEXTURE2D_DESC desc;
        m_preFilteredTexture->GetDesc(&desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = mipLevel;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = faceIndex;
        srvDesc.Texture2DArray.ArraySize = 1;

        device->CreateShaderResourceView(m_preFilteredTexture.Get(), &srvDesc,
                                        m_debugPreFilteredFaceSRVs[srvIndex].GetAddressOf());
    }

    return m_debugPreFilteredFaceSRVs[srvIndex].Get();
}

void CIBLGenerator::createBrdfLutShader() {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());

    std::string vsSource = LoadShaderSource("../source/code/Shader/BrdfLut.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/BrdfLut.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load BRDF LUT shaders!");
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> psBlob, err;

    // Compile PS (VS already compiled in createIrradianceShader, can reuse m_fullscreenVS)
    HRESULT hr = D3DCompile(psSource.c_str(), psSource.size(), "BrdfLut.ps.hlsl",
                            nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== BRDF LUT PS COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_brdfLutPS.GetAddressOf());
}

ID3D11ShaderResourceView* CIBLGenerator::GenerateBrdfLut(int resolution) {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());

    if (!device || !context) return nullptr;

    CFFLog::Info("IBL: Generating BRDF LUT (%dx%d)...", resolution, resolution);

    // Create 2D texture (not cubemap)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = resolution;
    texDesc.Height = resolution;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;  // RG channels for scale/bias
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = 0;  // NOT a cubemap

    device->CreateTexture2D(&texDesc, nullptr, m_brdfLutTexture.ReleaseAndGetAddressOf());

    // Create SRV for the whole texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    device->CreateShaderResourceView(m_brdfLutTexture.Get(), &srvDesc, m_brdfLutSRV.ReleaseAndGetAddressOf());

    // Create RTV
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = texDesc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(m_brdfLutTexture.Get(), &rtvDesc, rtv.GetAddressOf());

    // Set render target
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)resolution;
    vp.Height = (float)resolution;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // Clear to black (for debugging)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(rtv.Get(), clearColor);

    // Set shaders
    context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
    context->PSSetShader(m_brdfLutPS.Get(), nullptr, 0);

    // No textures or constant buffers needed for BRDF LUT

    // Draw fullscreen triangle (3 vertices, no vertex buffer)
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->Draw(3, 0);

    // Cleanup
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullptr);

    CFFLog::Info("IBL: BRDF LUT generated successfully!");

    return m_brdfLutSRV.Get();
}

bool CIBLGenerator::LoadIrradianceFromKTX2(const std::string& ktx2Path) {
    RHI::ITexture* rhiTexture = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!rhiTexture) {
        CFFLog::Error("IBLGenerator: Failed to load irradiance map from %s", ktx2Path.c_str());
        return false;
    }

    // Store RHI texture to keep it alive
    m_rhiIrradianceTexture.reset(rhiTexture);

    // Get native D3D11 resources from RHI texture
    ID3D11Texture2D* texture = static_cast<ID3D11Texture2D*>(rhiTexture->GetNativeHandle());
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(rhiTexture->GetSRV());

    // Store raw pointers for rendering (borrowed from RHI texture)
    m_irradianceTexture = texture;  // Note: ComPtr will AddRef
    m_irradianceSRV = srv;

    CFFLog::Info("IBLGenerator: Loaded irradiance map from KTX2 (%dx%d)", rhiTexture->GetWidth(), rhiTexture->GetHeight());
    return true;
}

bool CIBLGenerator::LoadPreFilteredFromKTX2(const std::string& ktx2Path) {
    RHI::ITexture* rhiTexture = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!rhiTexture) {
        CFFLog::Error("IBLGenerator: Failed to load pre-filtered map from %s", ktx2Path.c_str());
        return false;
    }

    // Store RHI texture to keep it alive
    m_rhiPreFilteredTexture.reset(rhiTexture);

    // Get native D3D11 resources from RHI texture
    ID3D11Texture2D* texture = static_cast<ID3D11Texture2D*>(rhiTexture->GetNativeHandle());
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(rhiTexture->GetSRV());

    // Store raw pointers for rendering (borrowed from RHI texture)
    m_preFilteredTexture = texture;
    m_preFilteredSRV = srv;

    // Get mip levels from texture descriptor
    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);
    m_preFilteredMipLevels = texDesc.MipLevels;

    CFFLog::Info("IBLGenerator: Loaded pre-filtered map from KTX2 (%dx%d, %d mips)", rhiTexture->GetWidth(), rhiTexture->GetHeight(), m_preFilteredMipLevels);
    return true;
}

bool CIBLGenerator::LoadBrdfLutFromKTX2(const std::string& ktx2Path) {
    RHI::ITexture* rhiTexture = CKTXLoader::Load2DTextureFromKTX2(ktx2Path);
    if (!rhiTexture) {
        CFFLog::Error("IBLGenerator: Failed to load BRDF LUT from %s", ktx2Path.c_str());
        return false;
    }

    // Store RHI texture to keep it alive
    m_rhiBrdfLutTexture.reset(rhiTexture);

    // Get native D3D11 resources from RHI texture
    ID3D11Texture2D* texture = static_cast<ID3D11Texture2D*>(rhiTexture->GetNativeHandle());
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(rhiTexture->GetSRV());

    // Store raw pointers for rendering (borrowed from RHI texture)
    m_brdfLutTexture = texture;
    m_brdfLutSRV = srv;

    CFFLog::Info("IBLGenerator: Loaded BRDF LUT from KTX2 (%dx%d)", rhiTexture->GetWidth(), rhiTexture->GetHeight());
    return true;
}
