#include "ReflectionProbeBaker.h"
#include "ForwardRenderPipeline.h"
#include "IBLGenerator.h"
#include "ShowFlags.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/ReflectionProbeAsset.h"
#include "Core/Exporter/KTXExporter.h"
#include "Engine/Scene.h"
#include "Engine/Camera.h"
#include <filesystem>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ============================================
// Helper: Compute cubemap face view/projection matrices
// ============================================
static void ComputeCubemapFaceMatrices(
    int face,
    const XMFLOAT3& position,
    XMMATRIX& outView,
    XMMATRIX& outProj)
{
    XMVECTOR pos = XMLoadFloat3(&position);

    // Cubemap face directions (DirectX left-handed)
    // face: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
    XMVECTOR targets[6] = {
        XMVectorSet( 1.0f,  0.0f,  0.0f, 0.0f),  // +X (Right)
        XMVectorSet(-1.0f,  0.0f,  0.0f, 0.0f),  // -X (Left)
        XMVectorSet( 0.0f,  1.0f,  0.0f, 0.0f),  // +Y (Up)
        XMVectorSet( 0.0f, -1.0f,  0.0f, 0.0f),  // -Y (Down)
        XMVectorSet( 0.0f,  0.0f,  1.0f, 0.0f),  // +Z (Forward)
        XMVectorSet( 0.0f,  0.0f, -1.0f, 0.0f),  // -Z (Back)
    };

    XMVECTOR ups[6] = {
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // +X
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // -X
        XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),  // +Y (looking down, up is -Z)
        XMVectorSet(0.0f, 0.0f,  1.0f, 0.0f),  // -Y (looking up, up is +Z)
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // +Z
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // -Z
    };

    XMVECTOR target = pos + targets[face];
    XMVECTOR up = ups[face];

    outView = XMMatrixLookAtLH(pos, target, up);
    outProj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.3f, 1000.0f);  // 90° FOV, 1:1 aspect
}

// ============================================
// CCubemapFaceCamera - Internal helper class
// ============================================
// Specialized camera for rendering cubemap faces
// Overrides GetViewMatrix() and GetProjectionMatrix() to use precomputed matrices
class CCubemapFaceCamera : public CCamera
{
public:
    CCubemapFaceCamera(int face, const XMFLOAT3& position)
    {
        ComputeCubemapFaceMatrices(face, position, m_viewMatrix, m_projMatrix);
    }

    XMMATRIX GetViewMatrix() const { return m_viewMatrix; }
    XMMATRIX GetProjectionMatrix() const { return m_projMatrix; }

private:
    XMMATRIX m_viewMatrix;
    XMMATRIX m_projMatrix;
};

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

    // Create a CCubemapFaceCamera for this face
    CCubemapFaceCamera faceCamera(face, position);

    // Setup RenderContext for this face
    CRenderPipeline::RenderContext renderCtx{
        faceCamera,
        scene,
        faceRTV,
        dsv,
        static_cast<unsigned int>(resolution),
        static_cast<unsigned int>(resolution),
        0.0f,  // deltaTime not needed for baking
        FShowFlags::ReflectionProbe()
    };

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
    const std::string& assetPath,
    int resolution)
{
    // 创建 ReflectionProbeAsset
    CReflectionProbeAsset asset;
    asset.m_resolution = resolution;
    asset.m_environmentMap = "env.ktx2";
    asset.m_irradianceMap = "irradiance.ktx2";
    asset.m_prefilteredMap = "prefiltered.ktx2";

    // 构建完整路径（相对路径 → 绝对路径）
    std::string fullPath = "E:/forfun/assets/" + assetPath;

    // 保存
    return asset.SaveToFile(fullPath);
}

void CReflectionProbeBaker::getCubemapFaceMatrices(
    int face,
    const XMFLOAT3& position,
    XMMATRIX& outView,
    XMMATRIX& outProj)
{
    ComputeCubemapFaceMatrices(face, position, outView, outProj);
}
