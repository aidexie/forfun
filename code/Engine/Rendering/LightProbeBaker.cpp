#include "LightProbeBaker.h"
#include "ForwardRenderPipeline.h"
#include "RenderPipeline.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/SphericalHarmonics.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/LightProbe.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ============================================
// Helper: Setup camera for cubemap face rendering
// ============================================
static void SetupCameraForCubemapFace(CCamera& camera, int face, const XMFLOAT3& position)
{
    camera.position = position;
    camera.fovY = XM_PIDIV2;         // 90 度
    camera.aspectRatio = 1.0f;       // 1:1 方形
    camera.nearZ = 0.3f;
    camera.farZ = 1000.0f;

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
        up = XMFLOAT3(0, 0, -1);
        break;
    case 3: // -Y (Down)
        target = XMFLOAT3(position.x, position.y - 1.0f, position.z);
        up = XMFLOAT3(0, 0, 1);
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

CLightProbeBaker::CLightProbeBaker()
    : m_pipeline(nullptr)
{
}

CLightProbeBaker::~CLightProbeBaker()
{
    Shutdown();
}

bool CLightProbeBaker::Initialize()
{
    if (m_initialized) {
        CFFLog::Warning("[LightProbeBaker] Already initialized");
        return true;
    }

    // 创建 rendering pipeline
    m_pipeline = new CForwardRenderPipeline();
    if (!m_pipeline->Initialize()) {
        CFFLog::Error("[LightProbeBaker] Failed to initialize ForwardRenderPipeline");
        delete m_pipeline;
        m_pipeline = nullptr;
        return false;
    }

    // 创建 Cubemap render target
    if (!createCubemapRenderTarget()) {
        CFFLog::Error("[LightProbeBaker] Failed to create cubemap render target");
        m_pipeline->Shutdown();
        delete m_pipeline;
        m_pipeline = nullptr;
        return false;
    }

    m_initialized = true;
    CFFLog::Info("[LightProbeBaker] Initialized (resolution: %dx%d)", CUBEMAP_RESOLUTION, CUBEMAP_RESOLUTION);
    return true;
}

void CLightProbeBaker::Shutdown()
{
    if (!m_initialized) return;

    m_cubemapRT.Reset();
    m_depthBuffer.Reset();
    m_stagingTexture.Reset();

    if (m_pipeline) {
        m_pipeline->Shutdown();
        delete m_pipeline;
        m_pipeline = nullptr;
    }

    m_initialized = false;
}

bool CLightProbeBaker::BakeProbe(
    SLightProbe& probe,
    const XMFLOAT3& position,
    CScene& scene)
{
    if (!m_initialized) {
        CFFLog::Error("[LightProbeBaker] Not initialized");
        return false;
    }

    CFFLog::Info("[LightProbeBaker] Baking probe at (%.1f, %.1f, %.1f)...",
                position.x, position.y, position.z);

    // 1. 渲染 Cubemap
    renderToCubemap(position, scene, m_cubemapRT.Get());

    // 2. 投影到 SH 系数
    if (!projectCubemapToSH(m_cubemapRT.Get(), probe.shCoeffs)) {
        CFFLog::Error("[LightProbeBaker] Failed to project cubemap to SH");
        return false;
    }

    // 3. 标记为已烘焙
    probe.isDirty = false;

    CFFLog::Info("[LightProbeBaker] Probe baked successfully. SH L0=%.3f,%.3f,%.3f",
                probe.shCoeffs[0].x, probe.shCoeffs[0].y, probe.shCoeffs[0].z);

    return true;
}

int CLightProbeBaker::BakeAllProbes(CScene& scene)
{
    if (!m_initialized) {
        CFFLog::Error("[LightProbeBaker] Not initialized");
        return 0;
    }

    int bakedCount = 0;

    // 遍历场景中所有 LightProbe 组件
    for (auto& objPtr : scene.GetWorld().Objects())
    {
        auto* probeComp = objPtr->GetComponent<SLightProbe>();
        if (!probeComp) continue;

        auto* transform = objPtr->GetComponent<STransform>();
        if (!transform) continue;

        // 烘焙该 probe
        if (BakeProbe(*probeComp, transform->position, scene)) {
            bakedCount++;
            CFFLog::Info("[LightProbeBaker] Baked probe '%s' (%d/%d)",
                        objPtr->GetName().c_str(), bakedCount, scene.GetWorld().Count());
        }
    }

    CFFLog::Info("[LightProbeBaker] Baked %d light probes", bakedCount);
    return bakedCount;
}

// ============================================
// Cubemap Rendering
// ============================================

void CLightProbeBaker::renderToCubemap(
    const XMFLOAT3& position,
    CScene& scene,
    ID3D11Texture2D* outputCubemap)
{
    auto* device = CDX11Context::Instance().GetDevice();
    auto* context = CDX11Context::Instance().GetContext();

    // RenderDoc 捕获：自动捕获第一个面的渲染
    // 为每个 face 创建 RTV
    ComPtr<ID3D11RenderTargetView> faceRTVs[6];
    for (int face = 0; face < 6; face++)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.ArraySize = 1;

        HRESULT hr = device->CreateRenderTargetView(outputCubemap, &rtvDesc, faceRTVs[face].GetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[LightProbeBaker] Failed to create RTV for face %d", face);
            return;
        }
    }

    // 创建 DSV
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;

    HRESULT hr = device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, dsv.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[LightProbeBaker] Failed to create DSV");
        return;
    }

    // 渲染 6 个面
    for (int face = 0; face < 6; face++)
    {
        renderCubemapFace(face, position, scene, faceRTVs[face].Get(), dsv.Get());
    }

    // 解绑所有 render targets 和 depth stencil（确保 GPU 写入完成）
    ID3D11RenderTargetView* nullRTV = nullptr;
    ID3D11DepthStencilView* nullDSV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, nullDSV);
}

void CLightProbeBaker::renderCubemapFace(
    int face,
    const XMFLOAT3& position,
    CScene& scene,
    ID3D11RenderTargetView* faceRTV,
    ID3D11DepthStencilView* dsv)
{
    auto* context = CDX11Context::Instance().GetContext();

    // 设置 viewport
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = (float)CUBEMAP_RESOLUTION;
    viewport.Height = (float)CUBEMAP_RESOLUTION;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // 清空 RT 和 depth buffer
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(faceRTV, clearColor);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // 设置 RT
    context->OMSetRenderTargets(1, &faceRTV, dsv);

    // 设置相机
    CCamera camera;
    SetupCameraForCubemapFace(camera, face, position);

    // 渲染场景（不包含 skybox，只渲染几何体）
    FShowFlags showFlags = FShowFlags::ReflectionProbe();

    CRenderPipeline::RenderContext ctx{
        camera,
        scene,
        (unsigned int)CUBEMAP_RESOLUTION,
        (unsigned int)CUBEMAP_RESOLUTION,
        0.0f,  // deltaTime
        showFlags
    };

    // Configure output: copy HDR result to this cubemap face
    ctx.finalOutputRTV = faceRTV;
    ctx.outputFormat = CRenderPipeline::RenderContext::EOutputFormat::HDR;
    
    m_pipeline->Render(ctx);
}

// ============================================
// SH Projection
// ============================================

bool CLightProbeBaker::projectCubemapToSH(
    ID3D11Texture2D* cubemap,
    XMFLOAT3 outCoeffs[9])
{
    auto* context = CDX11Context::Instance().GetContext();

    // 拷贝 cubemap 到 staging texture（用于 CPU 读取）
    context->CopyResource(m_stagingTexture.Get(), cubemap);

    // 读取 6 个面的像素数据
    const XMFLOAT4* cubemapData[6];
    XMFLOAT4* faceBuffers[6];

    for (int face = 0; face < 6; face++)
    {
        // Map staging texture
        D3D11_MAPPED_SUBRESOURCE mapped{};
        UINT subresource = D3D11CalcSubresource(0, face, 1);
        HRESULT hr = context->Map(m_stagingTexture.Get(), subresource, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            CFFLog::Error("[LightProbeBaker] Failed to map staging texture face %d", face);
            // 清理已分配的 buffer
            for (int i = 0; i < face; i++) {
                delete[] faceBuffers[i];
            }
            return false;
        }

        // 拷贝数据到临时 buffer（因为 Unmap 后数据会失效）
        int pixelCount = CUBEMAP_RESOLUTION * CUBEMAP_RESOLUTION;
        faceBuffers[face] = new XMFLOAT4[pixelCount];

        const XMFLOAT4* src = (const XMFLOAT4*)mapped.pData;
        for (int i = 0; i < pixelCount; i++) {
            faceBuffers[face][i] = src[i];
        }

        cubemapData[face] = faceBuffers[face];

        context->Unmap(m_stagingTexture.Get(), subresource);
    }

    // 投影到 SH
    SphericalHarmonics::ProjectCubemapToSH(cubemapData, CUBEMAP_RESOLUTION, outCoeffs);

    // 清理临时 buffer
    for (int face = 0; face < 6; face++) {
        delete[] faceBuffers[face];
    }

    return true;
}

// ============================================
// Helpers
// ============================================

bool CLightProbeBaker::createCubemapRenderTarget()
{
    auto* device = CDX11Context::Instance().GetDevice();

    // 创建 Cubemap render target (R16G16B16A16_FLOAT, 6 faces)
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = CUBEMAP_RESOLUTION;
    texDesc.Height = CUBEMAP_RESOLUTION;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 6;  // 6 faces
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, m_cubemapRT.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[LightProbeBaker] Failed to create cubemap RT (HRESULT: 0x%08X)", hr);
        return false;
    }

    // 创建 depth buffer
    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = CUBEMAP_RESOLUTION;
    depthDesc.Height = CUBEMAP_RESOLUTION;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthDesc.CPUAccessFlags = 0;
    depthDesc.MiscFlags = 0;

    hr = device->CreateTexture2D(&depthDesc, nullptr, m_depthBuffer.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[LightProbeBaker] Failed to create depth buffer (HRESULT: 0x%08X)", hr);
        return false;
    }

    // 创建 staging texture（用于 CPU 读取）
    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    hr = device->CreateTexture2D(&stagingDesc, nullptr, m_stagingTexture.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[LightProbeBaker] Failed to create staging texture (HRESULT: 0x%08X)", hr);
        return false;
    }

    return true;
}
