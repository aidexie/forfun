#include "ReflectionProbeBaker.h"
#include "CubemapRenderer.h"
#include "ForwardRenderPipeline.h"
#include "IBLGenerator.h"
#include "ShowFlags.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/ReflectionProbeAsset.h"
#include "Core/Exporter/KTXExporter.h"
#include "Core/RenderDocCapture.h"
#include "Engine/Scene.h"
#include "Engine/Camera.h"
#include <filesystem>

using namespace DirectX;

CReflectionProbeBaker::CReflectionProbeBaker() = default;
CReflectionProbeBaker::~CReflectionProbeBaker() { Shutdown(); }

bool CReflectionProbeBaker::Initialize()
{
    if (m_initialized) {
        CFFLog::Warning("ReflectionProbeBaker already initialized");
        return true;
    }

    // 创建 rendering pipeline
    m_pipeline = std::make_unique<CForwardRenderPipeline>();
    if (!m_pipeline->Initialize()) {
        CFFLog::Error("Failed to initialize ForwardRenderPipeline for ReflectionProbeBaker");
        m_pipeline.reset();
        return false;
    }

    // 创建 IBL generator
    m_iblGenerator = std::make_unique<CIBLGenerator>();
    if (!m_iblGenerator->Initialize()) {
        CFFLog::Error("Failed to initialize IBLGenerator for ReflectionProbeBaker");
        m_pipeline->Shutdown();
        m_pipeline.reset();
        m_iblGenerator.reset();
        return false;
    }

    m_initialized = true;
    CFFLog::Info("ReflectionProbeBaker initialized");
    return true;
}

void CReflectionProbeBaker::Shutdown()
{
    if (!m_initialized) return;

    m_cubemapRT.reset();
    m_depthBuffer.reset();

    if (m_iblGenerator) {
        m_iblGenerator->Shutdown();
        m_iblGenerator.reset();
    }

    if (m_pipeline) {
        m_pipeline->Shutdown();
        m_pipeline.reset();
    }

    m_initialized = false;
    CFFLog::Info("ReflectionProbeBaker shut down");
}

bool CReflectionProbeBaker::BakeProbe(
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    const std::string& outputAssetPath)
{
    if (!m_initialized) {
        CFFLog::Error("ReflectionProbeBaker not initialized");
        return false;
    }

    CFFLog::Info("Baking Reflection Probe at (%.2f, %.2f, %.2f), resolution: %d",
                position.x, position.y, position.z, resolution);

    // 1. 创建 Cubemap render target
    if (!createCubemapRenderTarget(resolution)) {
        CFFLog::Error("Failed to create cubemap render target");
        return false;
    }

    // 2. 渲染 6 个面到 Cubemap
    renderToCubemap(position, resolution, scene, m_cubemapRT.get());

    // 3. 构建输出路径
    std::string fullAssetPath = FFPath::GetAbsolutePath(outputAssetPath);
    std::filesystem::path assetPath(fullAssetPath);
    std::string basePath = assetPath.parent_path().string();

    // 确保目录存在
    if (!std::filesystem::exists(basePath)) {
        std::filesystem::create_directories(basePath);
        CFFLog::Info("Created directory: %s", basePath.c_str());
    }

    // 4. 保存 Environment Cubemap
    std::string envPath = basePath + "/env.ktx2";
    if (!saveCubemapAsKTX2(m_cubemapRT.get(), envPath)) {
        CFFLog::Error("Failed to save environment cubemap");
        return false;
    }

    // 5. 生成并保存 IBL maps
    generateAndSaveIBL(m_cubemapRT.get(), basePath);

    // 6. 创建 .ffasset
    if (!createAssetFile(fullAssetPath, resolution)) {
        CFFLog::Error("Failed to create .ffasset file");
        return false;
    }

    CFFLog::Info("Successfully baked Reflection Probe: %s", outputAssetPath.c_str());
    return true;
}

bool CReflectionProbeBaker::createCubemapRenderTarget(int resolution)
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();

    // Release old resources if resolution changed
    if (m_currentResolution != resolution) {
        m_cubemapRT.reset();
        m_depthBuffer.reset();
    }

    // Create Cubemap render target (HDR format for accurate lighting)
    RHI::TextureDesc cubemapDesc;
    cubemapDesc.width = resolution;
    cubemapDesc.height = resolution;
    cubemapDesc.mipLevels = 1;
    cubemapDesc.arraySize = 1;  // Will be 6 due to isCubemap
    cubemapDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    cubemapDesc.usage = RHI::ETextureUsage::RenderTarget | RHI::ETextureUsage::ShaderResource;
    cubemapDesc.isCubemap = true;
    cubemapDesc.debugName = "ReflectionProbeBaker_CubemapRT";

    m_cubemapRT.reset(renderContext->CreateTexture(cubemapDesc));
    if (!m_cubemapRT) {
        CFFLog::Error("Failed to create cubemap render target");
        return false;
    }

    // Create depth buffer
    RHI::TextureDesc depthDesc;
    depthDesc.width = resolution;
    depthDesc.height = resolution;
    depthDesc.mipLevels = 1;
    depthDesc.arraySize = 1;
    depthDesc.format = RHI::ETextureFormat::D24_UNORM_S8_UINT;
    depthDesc.usage = RHI::ETextureUsage::DepthStencil;
    depthDesc.debugName = "ReflectionProbeBaker_DepthBuffer";

    m_depthBuffer.reset(renderContext->CreateTexture(depthDesc));
    if (!m_depthBuffer) {
        CFFLog::Error("Failed to create depth buffer");
        m_cubemapRT.reset();
        return false;
    }

    m_currentResolution = resolution;
    CFFLog::Info("Created cubemap render target: %dx%d", resolution, resolution);
    return true;
}

void CReflectionProbeBaker::renderToCubemap(
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    RHI::ITexture* outputCubemap)
{
    // RenderDoc 捕获：自动捕获渲染过程
    static bool s_captureFirstBake = true;
    if (s_captureFirstBake) {
        CRenderDocCapture::BeginFrameCapture();
        s_captureFirstBake = false;
    }

    // 使用共享的 CubemapRenderer
    CCubemapRenderer::RenderToCubemap(
        position,
        resolution,
        scene,
        m_pipeline.get(),
        outputCubemap);

    // 结束 RenderDoc 捕获
    CRenderDocCapture::EndFrameCapture();

    CFFLog::Info("Rendered all 6 cubemap faces");
}

void CReflectionProbeBaker::generateAndSaveIBL(
    RHI::ITexture* envCubemap,
    const std::string& basePath)
{
    // Generate Irradiance Map (32×32, diffuse convolution)
    RHI::ITexture* irradianceTexture = m_iblGenerator->GenerateIrradianceMap(envCubemap, 32);
    if (!irradianceTexture) {
        CFFLog::Error("Failed to generate irradiance map");
        return;
    }

    // Generate Pre-filtered Map (128×128, 7 mip levels for roughness)
    RHI::ITexture* prefilteredTexture = m_iblGenerator->GeneratePreFilteredMap(envCubemap, 128, 7);
    if (!prefilteredTexture) {
        CFFLog::Error("Failed to generate pre-filtered map");
        return;
    }

    // Save Irradiance Map to KTX2
    std::string irradiancePath = basePath + "/irradiance.ktx2";
    if (!saveCubemapAsKTX2(irradianceTexture, irradiancePath)) {
        CFFLog::Error("Failed to save irradiance map");
    }

    // Save Pre-filtered Map to KTX2
    std::string prefilteredPath = basePath + "/prefiltered.ktx2";
    if (!saveCubemapAsKTX2(prefilteredTexture, prefilteredPath)) {
        CFFLog::Error("Failed to save pre-filtered map");
    }

    CFFLog::Info("Generated and saved IBL maps: irradiance + prefiltered");
}

bool CReflectionProbeBaker::saveCubemapAsKTX2(
    RHI::ITexture* cubemap,
    const std::string& outputPath)
{
    if (!cubemap) {
        CFFLog::Error("Cannot save null cubemap");
        return false;
    }

    // Export using CKTXExporter (RHI version)
    bool success = CKTXExporter::ExportCubemapToKTX2(cubemap, outputPath, cubemap->GetMipLevels());

    if (success) {
        CFFLog::Info("Saved cubemap to KTX2: %s (resolution: %dx%d, mips: %d)",
                    outputPath.c_str(), cubemap->GetWidth(), cubemap->GetHeight(), cubemap->GetMipLevels());
    } else {
        CFFLog::Error("Failed to save cubemap to KTX2: %s", outputPath.c_str());
    }

    return success;
}

bool CReflectionProbeBaker::createAssetFile(
    const std::string& fullAssetPath,
    int resolution)
{
    // 创建 ReflectionProbeAsset
    CReflectionProbeAsset asset;
    asset.m_resolution = resolution;
    asset.m_environmentMap = "env.ktx2";
    asset.m_irradianceMap = "irradiance.ktx2";
    asset.m_prefilteredMap = "prefiltered.ktx2";

    // 保存
    return asset.SaveToFile(fullAssetPath);
}
