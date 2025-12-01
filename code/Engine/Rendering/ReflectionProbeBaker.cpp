#include "ReflectionProbeBaker.h"
#include "ForwardRenderPipeline.h"
#include "IBLGenerator.h"
#include "ShowFlags.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/ReflectionProbeAsset.h"
#include "Core/Exporter/KTXExporter.h"
#include "Core/RenderDocCapture.h"
#include "Engine/Scene.h"
#include "Engine/Camera.h"
#include <filesystem>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ============================================
// Helper: Setup camera for cubemap face rendering
// ============================================
// 设置相机的 yaw/pitch 来朝向指定的 cubemap face
// DirectX 左手坐标系：+X=Right, +Y=Up, +Z=Forward
static void SetupCameraForCubemapFace(CCamera& camera, int face, const XMFLOAT3& position)
{
    using namespace DirectX;

    camera.position = position;

    // 设置 FOV 为 90 度（覆盖整个 cubemap face）
    camera.fovY = XM_PIDIV2;         // 90 度
    camera.aspectRatio = 1.0f;       // 1:1 方形
    camera.nearZ = 0.3f;
    camera.farZ = 1000.0f;

    // 为每个 cubemap face 设置 LookAt
    // DirectX Cubemap 标准约定
    XMFLOAT3 target, up;

    switch (face) {
    case 0: // +X (Right)
        target = XMFLOAT3(position.x + 1.0f, position.y, position.z);
        up = XMFLOAT3(0, 1, 0);
        break;
    case 1: // -X (Left)
        target = XMFLOAT3(position.x - 1.0f, position.y, position.z);
        up = XMFLOAT3(0, 1, 0);
        break;
    case 2: // +Y (Up)
        target = XMFLOAT3(position.x, position.y + 1.0f, position.z);
        up = XMFLOAT3(0, 0, -1);   // ⚠️ 特殊：朝上看时，up 指向 +Z
        break;
    case 3: // -Y (Down)
        target = XMFLOAT3(position.x, position.y - 1.0f, position.z);
        up = XMFLOAT3(0, 0, 1);  // ⚠️ 特殊：朝下看时，up 指向 -Z
        break;
    case 4: // +Z (Forward)
        target = XMFLOAT3(position.x, position.y, position.z + 1.0f);
        up = XMFLOAT3(0, 1, 0);
        break;
    case 5: // -Z (Back)
        target = XMFLOAT3(position.x, position.y, position.z - 1.0f);
        up = XMFLOAT3(0, 1, 0);
        break;
    }

    camera.SetLookAt(position, target, up);
}

CReflectionProbeBaker::CReflectionProbeBaker()
    : m_pipeline(nullptr)
    , m_iblGenerator(nullptr)
{
}

CReflectionProbeBaker::~CReflectionProbeBaker()
{
    Shutdown();
}

bool CReflectionProbeBaker::Initialize()
{
    if (m_initialized) {
        CFFLog::Warning("ReflectionProbeBaker already initialized");
        return true;
    }

    // 创建 rendering pipeline
    m_pipeline = new CForwardRenderPipeline();
    if (!m_pipeline->Initialize()) {
        CFFLog::Error("Failed to initialize ForwardRenderPipeline for ReflectionProbeBaker");
        delete m_pipeline;
        m_pipeline = nullptr;
        return false;
    }

    // 创建 IBL generator
    m_iblGenerator = new CIBLGenerator();
    if (!m_iblGenerator->Initialize()) {
        CFFLog::Error("Failed to initialize IBLGenerator for ReflectionProbeBaker");
        m_pipeline->Shutdown();
        delete m_pipeline;
        m_pipeline = nullptr;
        delete m_iblGenerator;
        m_iblGenerator = nullptr;
        return false;
    }

    m_initialized = true;
    CFFLog::Info("ReflectionProbeBaker initialized");
    return true;
}

void CReflectionProbeBaker::Shutdown()
{
    if (!m_initialized) return;

    m_cubemapRT.Reset();
    m_depthBuffer.Reset();

    if (m_iblGenerator) {
        m_iblGenerator->Shutdown();
        delete m_iblGenerator;
        m_iblGenerator = nullptr;
    }

    if (m_pipeline) {
        m_pipeline->Shutdown();
        delete m_pipeline;
        m_pipeline = nullptr;
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
    renderToCubemap(position, resolution, scene, m_cubemapRT.Get());

    // 3. 构建输出路径
    std::string fullAssetPath = "E:/forfun/assets/" + outputAssetPath;
    std::filesystem::path assetPath(fullAssetPath);
    std::string basePath = assetPath.parent_path().string();

    // 确保目录存在
    if (!std::filesystem::exists(basePath)) {
        std::filesystem::create_directories(basePath);
        CFFLog::Info("Created directory: %s", basePath.c_str());
    }

    // 4. 保存 Environment Cubemap
    std::string envPath = basePath + "/env.ktx2";
    if (!saveCubemapAsKTX2(m_cubemapRT.Get(), envPath)) {
        CFFLog::Error("Failed to save environment cubemap");
        return false;
    }

    // 5. 生成并保存 IBL maps
    generateAndSaveIBL(m_cubemapRT.Get(), basePath);

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
    auto device = CDX11Context::Instance().GetDevice();

    // Release old resources if they exist
    m_cubemapRT.Reset();
    m_depthBuffer.Reset();

    // Create Cubemap render target (HDR format for accurate lighting)
    D3D11_TEXTURE2D_DESC cubemapDesc{};
    cubemapDesc.Width = resolution;
    cubemapDesc.Height = resolution;
    cubemapDesc.MipLevels = 1;
    cubemapDesc.ArraySize = 6;  // 6 faces
    cubemapDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR
    cubemapDesc.SampleDesc.Count = 1;
    cubemapDesc.SampleDesc.Quality = 0;
    cubemapDesc.Usage = D3D11_USAGE_DEFAULT;
    cubemapDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    cubemapDesc.CPUAccessFlags = 0;
    cubemapDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr = device->CreateTexture2D(&cubemapDesc, nullptr, m_cubemapRT.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Failed to create cubemap render target (HRESULT: 0x%08X)", hr);
        return false;
    }

    // Create depth buffer
    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = resolution;
    depthDesc.Height = resolution;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;  // Shared depth buffer for all faces
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthDesc.CPUAccessFlags = 0;
    depthDesc.MiscFlags = 0;

    hr = device->CreateTexture2D(&depthDesc, nullptr, m_depthBuffer.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Failed to create depth buffer (HRESULT: 0x%08X)", hr);
        m_cubemapRT.Reset();
        return false;
    }

    CFFLog::Info("Created cubemap render target: %dx%d", resolution, resolution);
    return true;
}

void CReflectionProbeBaker::renderToCubemap(
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    ID3D11Texture2D* outputCubemap)
{
    auto device = CDX11Context::Instance().GetDevice();

    // RenderDoc 捕获：自动捕获第一个面的渲染
    // 这样你可以在 RenderDoc 中查看烘焙时的渲染状态
    static bool s_captureFirstFace = true;
    if (s_captureFirstFace) {
        CRenderDocCapture::BeginFrameCapture();
        s_captureFirstFace = false;  // 只捕获一次
    }

    // Create depth stencil view (shared for all faces)
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;

    HRESULT hr = device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, dsv.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Failed to create depth stencil view");
        return;
    }

    // Render each face
    for (int face = 0; face < 6; ++face) {
        // Create RTV for this face
        ComPtr<ID3D11RenderTargetView> faceRTV;
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.ArraySize = 1;

        hr = device->CreateRenderTargetView(outputCubemap, &rtvDesc, faceRTV.GetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("Failed to create RTV for face %d", face);
            continue;
        }

        // Render this face
        renderCubemapFace(face, position, resolution, scene, faceRTV.Get(), dsv.Get());
    }

    // 解绑所有 render targets 和 depth stencil（确保 GPU 写入完成）
    auto context = CDX11Context::Instance().GetContext();
    // ID3D11RenderTargetView* nullRTV = nullptr;
    // ID3D11DepthStencilView* nullDSV = nullptr;
    // ctx->OMSetRenderTargets(1, &nullRTV, nullDSV);

    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
    // 在所有 6 个面渲染完成后结束 RenderDoc 捕获
    CRenderDocCapture::EndFrameCapture();

    CFFLog::Info("Rendered all 6 cubemap faces");
}

void CReflectionProbeBaker::renderCubemapFace(
    int face,
    const XMFLOAT3& position,
    int resolution,
    CScene& scene,
    ID3D11RenderTargetView* faceRTV,
    ID3D11DepthStencilView* dsv)
{
    auto ctx = CDX11Context::Instance().GetContext();

    // Clear render target and depth buffer
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ctx->ClearRenderTargetView(faceRTV, clearColor);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Setup camera for this cubemap face
    CCamera faceCamera;
    SetupCameraForCubemapFace(faceCamera, face, position);

    // Setup RenderContext for this face
    CRenderPipeline::RenderContext renderCtx{
        faceCamera,                                     // camera
        scene,                                          // scene
        static_cast<unsigned int>(resolution),          // width
        static_cast<unsigned int>(resolution),          // height
        0.0f,                                          // deltaTime (not needed for baking)
        FShowFlags::ReflectionProbe()                   // showFlags
    };

    // Configure output: copy HDR result to this cubemap face
    renderCtx.finalOutputRTV = faceRTV;
    renderCtx.outputFormat = CRenderPipeline::RenderContext::EOutputFormat::HDR;

    // Render using the pipeline
    m_pipeline->Render(renderCtx);
}

void CReflectionProbeBaker::generateAndSaveIBL(
    ID3D11Texture2D* envCubemap,
    const std::string& basePath)
{
    auto device = CDX11Context::Instance().GetDevice();

    // Create SRV for the environment cubemap
    ComPtr<ID3D11ShaderResourceView> envSRV;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    HRESULT hr = device->CreateShaderResourceView(envCubemap, &srvDesc, envSRV.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("Failed to create SRV for environment cubemap");
        return;
    }

    // Generate Irradiance Map (32×32, diffuse convolution)
    ID3D11ShaderResourceView* irradianceSRV = m_iblGenerator->GenerateIrradianceMap(envSRV.Get(), 32);
    if (!irradianceSRV) {
        CFFLog::Error("Failed to generate irradiance map");
        return;
    }

    // Generate Pre-filtered Map (128×128, 7 mip levels for roughness)
    ID3D11ShaderResourceView* prefilteredSRV = m_iblGenerator->GeneratePreFilteredMap(envSRV.Get(), 128, 7);
    if (!prefilteredSRV) {
        CFFLog::Error("Failed to generate pre-filtered map");
        return;
    }

    // Save Irradiance Map to KTX2
    std::string irradiancePath = basePath + "/irradiance.ktx2";
    ID3D11Texture2D* irradianceTexture = m_iblGenerator->GetIrradianceTexture();
    if (!saveCubemapAsKTX2(irradianceTexture, irradiancePath)) {
        CFFLog::Error("Failed to save irradiance map");
    }

    // Save Pre-filtered Map to KTX2
    std::string prefilteredPath = basePath + "/prefiltered.ktx2";
    ID3D11Texture2D* prefilteredTexture = m_iblGenerator->GetPreFilteredTexture();
    if (!saveCubemapAsKTX2(prefilteredTexture, prefilteredPath)) {
        CFFLog::Error("Failed to save pre-filtered map");
    }

    CFFLog::Info("Generated and saved IBL maps: irradiance + prefiltered");
}

bool CReflectionProbeBaker::saveCubemapAsKTX2(
    ID3D11Texture2D* cubemap,
    const std::string& outputPath)
{
    if (!cubemap) {
        CFFLog::Error("Cannot save null cubemap");
        return false;
    }

    // Get texture description to determine mip levels
    D3D11_TEXTURE2D_DESC desc;
    cubemap->GetDesc(&desc);

    // Export using CKTXExporter
    // 0 = export all mip levels (or just 1 if texture has no mipmaps)
    bool success = CKTXExporter::ExportCubemapToKTX2(cubemap, outputPath, desc.MipLevels);

    if (success) {
        CFFLog::Info("Saved cubemap to KTX2: %s (resolution: %dx%d, mips: %d)",
                    outputPath.c_str(), desc.Width, desc.Height, desc.MipLevels);
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

