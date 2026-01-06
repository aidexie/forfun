#include "IBLGenerator.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <DirectXPackedVector.h>

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
#define DDS_MAGIC 0x20534444
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
#define DXGI_FORMAT_R16G16B16A16_FLOAT 10

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

CIBLGenerator::CIBLGenerator() = default;
CIBLGenerator::~CIBLGenerator() { Shutdown(); }

bool CIBLGenerator::Initialize() {
    if (m_initialized) return true;

    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return false;

    createShaders();

    // Create sampler state
    RHI::SamplerDesc sampDesc;
    sampDesc.filter = RHI::EFilter::MinMagMipLinear;
    sampDesc.addressU = RHI::ETextureAddressMode::Clamp;
    sampDesc.addressV = RHI::ETextureAddressMode::Clamp;
    sampDesc.addressW = RHI::ETextureAddressMode::Clamp;
    sampDesc.minLOD = 0;
    sampDesc.maxLOD = 3.402823466e+38f; // FLT_MAX
    m_sampler.reset(renderContext->CreateSampler(sampDesc));

    // Create constant buffer for face index (16 bytes: int + padding)
    RHI::BufferDesc cbDesc;
    cbDesc.size = 16;
    cbDesc.usage = RHI::EBufferUsage::Constant;
    cbDesc.cpuAccess = RHI::ECPUAccess::Write;
    m_cbFaceIndex.reset(renderContext->CreateBuffer(cbDesc, nullptr));

    // Create constant buffer for roughness (16 bytes: float + float + padding)
    m_cbRoughness.reset(renderContext->CreateBuffer(cbDesc, nullptr));

    m_initialized = true;
    return true;
}

void CIBLGenerator::Shutdown() {
    if (!m_initialized) return;

    m_fullscreenVS.reset();
    m_irradiancePS.reset();
    m_prefilterPS.reset();
    m_brdfLutPS.reset();
    m_sampler.reset();
    m_cbFaceIndex.reset();
    m_cbRoughness.reset();
    m_irradianceTexture.reset();
    m_preFilteredTexture.reset();
    m_brdfLutTexture.reset();

    m_initialized = false;
}

void CIBLGenerator::createShaders() {
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile Irradiance shaders
    std::string vsSource = LoadShaderSource(shaderDir + "IrradianceConvolution.vs.hlsl");
    std::string irradiancePsSource = LoadShaderSource(shaderDir + "IrradianceConvolution.ps.hlsl");

    if (!vsSource.empty()) {
        RHI::SCompiledShader vsCompiled = RHI::CompileShaderFromSource(vsSource, "main", "vs_5_0", nullptr, debugShaders);
        if (vsCompiled.success) {
            RHI::ShaderDesc vsDesc;
            vsDesc.type = RHI::EShaderType::Vertex;
            vsDesc.bytecode = vsCompiled.bytecode.data();
            vsDesc.bytecodeSize = vsCompiled.bytecode.size();
            m_fullscreenVS.reset(renderContext->CreateShader(vsDesc));
        } else {
            CFFLog::Error("IBL: Failed to compile irradiance VS: %s", vsCompiled.errorMessage.c_str());
        }
    }

    if (!irradiancePsSource.empty()) {
        RHI::SCompiledShader psCompiled = RHI::CompileShaderFromSource(irradiancePsSource, "main", "ps_5_0", nullptr, debugShaders);
        if (psCompiled.success) {
            RHI::ShaderDesc psDesc;
            psDesc.type = RHI::EShaderType::Pixel;
            psDesc.bytecode = psCompiled.bytecode.data();
            psDesc.bytecodeSize = psCompiled.bytecode.size();
            m_irradiancePS.reset(renderContext->CreateShader(psDesc));
        } else {
            CFFLog::Error("IBL: Failed to compile irradiance PS: %s", psCompiled.errorMessage.c_str());
        }
    }

    // Compile Pre-filter shader
    std::string prefilterPsSource = LoadShaderSource(shaderDir + "PreFilterEnvironmentMap.ps.hlsl");
    if (!prefilterPsSource.empty()) {
        RHI::SCompiledShader psCompiled = RHI::CompileShaderFromSource(prefilterPsSource, "main", "ps_5_0", nullptr, debugShaders);
        if (psCompiled.success) {
            RHI::ShaderDesc psDesc;
            psDesc.type = RHI::EShaderType::Pixel;
            psDesc.bytecode = psCompiled.bytecode.data();
            psDesc.bytecodeSize = psCompiled.bytecode.size();
            m_prefilterPS.reset(renderContext->CreateShader(psDesc));
        } else {
            CFFLog::Error("IBL: Failed to compile prefilter PS: %s", psCompiled.errorMessage.c_str());
        }
    }

    // Compile BRDF LUT shader
    std::string brdfLutPsSource = LoadShaderSource(shaderDir + "BrdfLut.ps.hlsl");
    if (!brdfLutPsSource.empty()) {
        RHI::SCompiledShader psCompiled = RHI::CompileShaderFromSource(brdfLutPsSource, "main", "ps_5_0", nullptr, debugShaders);
        if (psCompiled.success) {
            RHI::ShaderDesc psDesc;
            psDesc.type = RHI::EShaderType::Pixel;
            psDesc.bytecode = psCompiled.bytecode.data();
            psDesc.bytecodeSize = psCompiled.bytecode.size();
            m_brdfLutPS.reset(renderContext->CreateShader(psDesc));
        } else {
            CFFLog::Error("IBL: Failed to compile BRDF LUT PS: %s", psCompiled.errorMessage.c_str());
        }
    }
}

RHI::ITexture* CIBLGenerator::GenerateIrradianceMap(RHI::ITexture* envMap, int outputSize) {
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();

    if (!envMap || !m_fullscreenVS || !m_irradiancePS) {
        CFFLog::Error("IBL: Cannot generate irradiance map - missing resources");
        return nullptr;
    }

    // Create output cubemap texture
    RHI::TextureDesc texDesc;
    texDesc.width = outputSize;
    texDesc.height = outputSize;
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    texDesc.usage = RHI::ETextureUsage::RenderTarget | RHI::ETextureUsage::ShaderResource;
    texDesc.isCubemap = true;
    texDesc.debugName = "IBL_IrradianceMap";

    m_irradianceTexture.reset(renderContext->CreateTexture(texDesc));
    if (!m_irradianceTexture) {
        CFFLog::Error("IBL: Failed to create irradiance texture");
        return nullptr;
    }

    // Render to each cubemap face
    for (int face = 0; face < 6; ++face) {
        // Set render target to this face
        cmdList->SetRenderTargetSlice(m_irradianceTexture.get(), face, nullptr);

        // Set viewport and scissor rect (DX12 requires both)
        cmdList->SetViewport(0, 0, (float)outputSize, (float)outputSize);
        cmdList->SetScissorRect(0, 0, outputSize, outputSize);

        // Update face index constant buffer (use SetConstantBufferData for DX12 compatibility)
        struct { int faceIndex; int padding[3]; } faceData = { face, {0, 0, 0} };

        // Set shaders and resources
        cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 0, envMap);
        cmdList->SetSampler(RHI::EShaderStage::Pixel, 0, m_sampler.get());
        cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 0, &faceData, sizeof(faceData));

        // Create and set pipeline state
        RHI::PipelineStateDesc psoDesc;
        psoDesc.vertexShader = m_fullscreenVS.get();
        psoDesc.pixelShader = m_irradiancePS.get();
        psoDesc.primitiveTopology = RHI::EPrimitiveTopology::TriangleList;
        psoDesc.depthStencil.depthEnable = false;
        psoDesc.depthStencil.depthWriteEnable = false;
        psoDesc.debugName = "IBL_Irradiance_PSO";

        std::unique_ptr<RHI::IPipelineState> pso(renderContext->CreatePipelineState(psoDesc));
        cmdList->SetPipelineState(pso.get());

        // Draw fullscreen triangle
        cmdList->Draw(3, 0);
    }

    // Cleanup
    cmdList->UnbindRenderTargets();
    cmdList->UnbindShaderResources(RHI::EShaderStage::Pixel, 0, 1);

    CFFLog::Info("IBL: Irradiance map generated (%dx%d)", outputSize, outputSize);
    return m_irradianceTexture.get();
}

RHI::ITexture* CIBLGenerator::GeneratePreFilteredMap(RHI::ITexture* envMap, int outputSize, int numMipLevels) {
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();

    if (!envMap || !m_fullscreenVS || !m_prefilterPS) {
        CFFLog::Error("IBL: Cannot generate pre-filtered map - missing resources");
        return nullptr;
    }

    numMipLevels = std::max(1, std::min(numMipLevels, 10));
    m_preFilteredMipLevels = numMipLevels;

    CFFLog::Info("IBL: Generating pre-filtered map (%dx%d, %d mip levels)...", outputSize, outputSize, numMipLevels);

    // Create output cubemap texture with mipmaps
    RHI::TextureDesc texDesc;
    texDesc.width = outputSize;
    texDesc.height = outputSize;
    texDesc.mipLevels = numMipLevels;
    texDesc.arraySize = 1;
    texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    texDesc.usage = RHI::ETextureUsage::RenderTarget | RHI::ETextureUsage::ShaderResource;
    texDesc.isCubemap = true;
    texDesc.debugName = "IBL_PreFilteredMap";

    m_preFilteredTexture.reset(renderContext->CreateTexture(texDesc));
    if (!m_preFilteredTexture) {
        CFFLog::Error("IBL: Failed to create pre-filtered texture");
        return nullptr;
    }

    float envResolution = (float)envMap->GetWidth();

    // Note: Current RHI doesn't support per-mip RTVs well
    // For now, only generate mip 0 (roughness = 0)
    // Full mip chain generation would require RHI extension for per-mip RTVs

    // Render mip 0 only (for now)
    for (int face = 0; face < 6; ++face) {
        cmdList->SetRenderTargetSlice(m_preFilteredTexture.get(), face, nullptr);
        cmdList->SetViewport(0, 0, (float)outputSize, (float)outputSize);
        cmdList->SetScissorRect(0, 0, outputSize, outputSize);

        // Build constant buffers (use SetConstantBufferData for DX12 compatibility)
        struct { int faceIndex; int padding[3]; } faceData = { face, {0, 0, 0} };
        struct { float roughness; float envResolution; float padding[2]; } roughnessData = { 0.0f, envResolution, {0.0f, 0.0f} };

        // Set shaders and resources
        cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 0, envMap);
        cmdList->SetSampler(RHI::EShaderStage::Pixel, 0, m_sampler.get());
        cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 0, &faceData, sizeof(faceData));
        cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 1, &roughnessData, sizeof(roughnessData));

        RHI::PipelineStateDesc psoDesc;
        psoDesc.vertexShader = m_fullscreenVS.get();
        psoDesc.pixelShader = m_prefilterPS.get();
        psoDesc.primitiveTopology = RHI::EPrimitiveTopology::TriangleList;
        psoDesc.depthStencil.depthEnable = false;
        psoDesc.depthStencil.depthWriteEnable = false;
        psoDesc.debugName = "IBL_Prefilter_PSO";

        std::unique_ptr<RHI::IPipelineState> pso(renderContext->CreatePipelineState(psoDesc));
        cmdList->SetPipelineState(pso.get());

        cmdList->Draw(3, 0);
    }

    cmdList->UnbindRenderTargets();
    cmdList->UnbindShaderResources(RHI::EShaderStage::Pixel, 0, 1);

    CFFLog::Info("IBL: Pre-filtered map generated");
    return m_preFilteredTexture.get();
}

RHI::ITexture* CIBLGenerator::GenerateBrdfLut(int resolution) {
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();

    if (!m_fullscreenVS || !m_brdfLutPS) {
        CFFLog::Error("IBL: Cannot generate BRDF LUT - missing shaders");
        return nullptr;
    }

    CFFLog::Info("IBL: Generating BRDF LUT (%dx%d)...", resolution, resolution);

    // Create 2D texture (not cubemap)
    RHI::TextureDesc texDesc;
    texDesc.width = resolution;
    texDesc.height = resolution;
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = RHI::ETextureFormat::R16G16_FLOAT;  // RG channels for scale/bias
    texDesc.usage = RHI::ETextureUsage::RenderTarget | RHI::ETextureUsage::ShaderResource;
    texDesc.isCubemap = false;
    texDesc.debugName = "IBL_BrdfLut";

    m_brdfLutTexture.reset(renderContext->CreateTexture(texDesc));
    if (!m_brdfLutTexture) {
        CFFLog::Error("IBL: Failed to create BRDF LUT texture");
        return nullptr;
    }

    // Set render target
    RHI::ITexture* rts[] = { m_brdfLutTexture.get() };
    cmdList->SetRenderTargets(1, rts, nullptr);

    // Set viewport and scissor rect (DX12 requires both)
    cmdList->SetViewport(0, 0, (float)resolution, (float)resolution);
    cmdList->SetScissorRect(0, 0, resolution, resolution);

    // Clear
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTarget(m_brdfLutTexture.get(), clearColor);

    // Set pipeline state
    RHI::PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_fullscreenVS.get();
    psoDesc.pixelShader = m_brdfLutPS.get();
    psoDesc.primitiveTopology = RHI::EPrimitiveTopology::TriangleList;
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.depthStencil.depthWriteEnable = false;

    std::unique_ptr<RHI::IPipelineState> pso(renderContext->CreatePipelineState(psoDesc));
    cmdList->SetPipelineState(pso.get());

    // Draw fullscreen triangle
    cmdList->Draw(3, 0);

    // Cleanup
    cmdList->UnbindRenderTargets();

    CFFLog::Info("IBL: BRDF LUT generated");
    return m_brdfLutTexture.get();
}

bool CIBLGenerator::LoadIrradianceFromKTX2(const std::string& ktx2Path) {
    RHI::ITexture* texture = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!texture) {
        CFFLog::Error("IBL: Failed to load irradiance map from %s", ktx2Path.c_str());
        return false;
    }

    m_irradianceTexture.reset(texture);
    CFFLog::Info("IBL: Loaded irradiance map from KTX2 (%dx%d)", texture->GetWidth(), texture->GetHeight());
    return true;
}

bool CIBLGenerator::LoadPreFilteredFromKTX2(const std::string& ktx2Path) {
    RHI::ITexture* texture = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!texture) {
        CFFLog::Error("IBL: Failed to load pre-filtered map from %s", ktx2Path.c_str());
        return false;
    }

    m_preFilteredTexture.reset(texture);
    m_preFilteredMipLevels = texture->GetMipLevels();
    CFFLog::Info("IBL: Loaded pre-filtered map from KTX2 (%dx%d, %d mips)",
                texture->GetWidth(), texture->GetHeight(), m_preFilteredMipLevels);
    return true;
}

bool CIBLGenerator::LoadBrdfLutFromKTX2(const std::string& ktx2Path) {
    RHI::ITexture* texture = CKTXLoader::Load2DTextureFromKTX2(ktx2Path);
    if (!texture) {
        CFFLog::Error("IBL: Failed to load BRDF LUT from %s", ktx2Path.c_str());
        return false;
    }

    m_brdfLutTexture.reset(texture);
    CFFLog::Info("IBL: Loaded BRDF LUT from KTX2 (%dx%d)", texture->GetWidth(), texture->GetHeight());
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
    float m = frexpf(v, &e);
    v = m * 256.0f / v;

    rgbe[0] = (unsigned char)(r * v);
    rgbe[1] = (unsigned char)(g * v);
    rgbe[2] = (unsigned char)(b * v);
    rgbe[3] = (unsigned char)(e + 128);
}

bool CIBLGenerator::SaveIrradianceMapToDDS(const std::string& filepath) {
    if (!m_irradianceTexture) {
        CFFLog::Error("No irradiance map to save!");
        return false;
    }

    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();

    uint32_t width = m_irradianceTexture->GetWidth();
    uint32_t height = m_irradianceTexture->GetHeight();

    CFFLog::Info("IBL: Saving irradiance map (%dx%d x 6 faces)...", width, height);

    // Create staging texture for readback
    RHI::TextureDesc stagingDesc;
    stagingDesc.width = width;
    stagingDesc.height = height;
    stagingDesc.mipLevels = 1;
    stagingDesc.arraySize = 1;
    stagingDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    stagingDesc.usage = RHI::ETextureUsage::Staging;
    stagingDesc.isCubemap = true;
    stagingDesc.debugName = "IBL_StagingTexture";

    RHI::TexturePtr stagingTexture(renderContext->CreateTexture(stagingDesc));
    if (!stagingTexture) {
        CFFLog::Error("Failed to create staging texture!");
        return false;
    }

    // Copy to staging
    cmdList->CopyTexture(stagingTexture.get(), m_irradianceTexture.get());

    // Prepare DDS header
    uint32_t magic = DDS_MAGIC;
    DDS_HEADER header = {};
    header.dwSize = 124;
    header.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
    header.dwHeight = height;
    header.dwWidth = width;
    header.dwPitchOrLinearSize = width * height * 8;
    header.dwMipMapCount = 1;
    header.ddspf.dwSize = 32;
    header.ddspf.dwFlags = DDPF_FOURCC;
    header.ddspf.dwFourCC = 0x30315844;  // "DX10"
    header.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX;
    header.dwCaps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES;

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
    for (uint32_t face = 0; face < 6; ++face) {
        RHI::MappedTexture mapped = stagingTexture->Map(face, 0);
        if (!mapped.pData) {
            CFFLog::Error("Failed to map staging texture face %d", face);
            file.close();
            return false;
        }

        const char* srcData = reinterpret_cast<const char*>(mapped.pData);
        size_t rowPitch = width * 8;  // 8 bytes per pixel
        for (uint32_t row = 0; row < height; ++row) {
            file.write(srcData + row * mapped.rowPitch, rowPitch);
        }

        stagingTexture->Unmap(face, 0);
        CFFLog::Info("IBL: Wrote face %d", face);
    }

    file.close();
    CFFLog::Info("IBL: Successfully saved to %s", fullPath.string().c_str());
    return true;
}

bool CIBLGenerator::SaveIrradianceMapToHDR(const std::string& filepath) {
    if (!m_irradianceTexture) {
        CFFLog::Error("No irradiance map to save!");
        return false;
    }

    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();

    uint32_t width = m_irradianceTexture->GetWidth();
    uint32_t height = m_irradianceTexture->GetHeight();

    CFFLog::Info("IBL: Saving irradiance map to HDR (%dx%d x 6 faces)...", width, height);

    // Create staging texture
    RHI::TextureDesc stagingDesc;
    stagingDesc.width = width;
    stagingDesc.height = height;
    stagingDesc.mipLevels = 1;
    stagingDesc.arraySize = 1;
    stagingDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    stagingDesc.usage = RHI::ETextureUsage::Staging;
    stagingDesc.isCubemap = true;
    stagingDesc.debugName = "IBL_StagingTexture";

    RHI::TexturePtr stagingTexture(renderContext->CreateTexture(stagingDesc));
    if (!stagingTexture) {
        CFFLog::Error("Failed to create staging texture!");
        return false;
    }

    cmdList->CopyTexture(stagingTexture.get(), m_irradianceTexture.get());

    const char* faceNames[6] = { "posX", "negX", "posY", "negY", "posZ", "negZ" };

    std::filesystem::path basePath = filepath;
    if (basePath.is_relative()) {
        basePath = std::filesystem::current_path() / filepath;
    }
    std::filesystem::create_directories(basePath.parent_path());

    std::string baseStr = basePath.string();
    size_t dotPos = baseStr.rfind('.');
    if (dotPos != std::string::npos) {
        baseStr = baseStr.substr(0, dotPos);
    }

    for (uint32_t face = 0; face < 6; ++face) {
        std::string faceFilename = baseStr + "_" + faceNames[face] + ".hdr";

        RHI::MappedTexture mapped = stagingTexture->Map(face, 0);
        if (!mapped.pData) {
            CFFLog::Error("Failed to map staging texture face %d", face);
            continue;
        }

        std::ofstream file(faceFilename, std::ios::binary);
        if (!file) {
            CFFLog::Error("Failed to create file: %s", faceFilename.c_str());
            stagingTexture->Unmap(face, 0);
            continue;
        }

        file << "#?RADIANCE\n";
        file << "FORMAT=32-bit_rle_rgbe\n";
        file << "\n";
        file << "-Y " << height << " +X " << width << "\n";

        std::vector<unsigned char> rgbeData(width * height * 4);
        const uint16_t* srcData = reinterpret_cast<const uint16_t*>(mapped.pData);
        size_t srcRowPitch = mapped.rowPitch / sizeof(uint16_t);

        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                size_t srcOffset = y * srcRowPitch + x * 4;

                DirectX::PackedVector::XMHALF4 half4;
                half4.x = srcData[srcOffset + 0];
                half4.y = srcData[srcOffset + 1];
                half4.z = srcData[srcOffset + 2];
                half4.w = srcData[srcOffset + 3];

                DirectX::XMVECTOR v = DirectX::PackedVector::XMLoadHalf4(&half4);
                DirectX::XMFLOAT4 f4;
                DirectX::XMStoreFloat4(&f4, v);

                size_t dstOffset = (y * width + x) * 4;
                floatToRgbe(f4.x, f4.y, f4.z, &rgbeData[dstOffset]);
            }
        }

        file.write(reinterpret_cast<const char*>(rgbeData.data()), rgbeData.size());

        stagingTexture->Unmap(face, 0);
        file.close();

        CFFLog::Info("IBL: Saved face %s to %s", faceNames[face], faceFilename.c_str());
    }

    CFFLog::Info("IBL: Successfully saved irradiance map to HDR files!");
    return true;
}
