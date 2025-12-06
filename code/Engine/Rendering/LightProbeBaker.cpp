#include "LightProbeBaker.h"
#include "ForwardRenderPipeline.h"
#include "RenderPipeline.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/SphericalHarmonics.h"
#include "Core/RenderDocCapture.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/LightProbe.h"
#include <memory>
#include <vector>
#include <array>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <fstream>
#include "Core/PathManager.h"

#include "stb_image_write.h"
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
        up = XMFLOAT3(0, 0, -1);  // 左手坐标系: up = -Z
        break;
    case 3: // -Y (Down)
        target = XMFLOAT3(position.x, position.y - 1.0f, position.z);
        up = XMFLOAT3(0, 0, 1);   // 左手坐标系: up = +Z
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

CLightProbeBaker::CLightProbeBaker() = default;

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
    m_pipeline = std::make_unique<CForwardRenderPipeline>();
    if (!m_pipeline->Initialize()) {
        CFFLog::Error("[LightProbeBaker] Failed to initialize ForwardRenderPipeline");
        m_pipeline.reset();
        return false;
    }

    // 创建 Cubemap render target
    if (!createCubemapRenderTarget()) {
        CFFLog::Error("[LightProbeBaker] Failed to create cubemap render target");
        m_pipeline->Shutdown();
        m_pipeline.reset();
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
        m_pipeline.reset();
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

                
    // RenderDoc 捕获：自动捕获第一个面的渲染
    // 这样你可以在 RenderDoc 中查看烘焙时的渲染状态
    static bool s_captureFirstFace = true;
    if (s_captureFirstFace) {
        CRenderDocCapture::BeginFrameCapture();
        s_captureFirstFace = false;  // 只捕获一次
    }
    // 1. 渲染 Cubemap
    renderToCubemap(position, scene, m_cubemapRT.Get());
    // 在所有 6 个面渲染完成后结束 RenderDoc 捕获
    CRenderDocCapture::EndFrameCapture();

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

    // 读取 6 个面的像素数据 - 使用 std::array<std::vector> 管理内存
    const int pixelCount = CUBEMAP_RESOLUTION * CUBEMAP_RESOLUTION;
    std::array<std::vector<XMFLOAT4>, 6> cubemapData;

    for (int face = 0; face < 6; face++)
    {
        // Map staging texture
        D3D11_MAPPED_SUBRESOURCE mapped{};
        UINT subresource = D3D11CalcSubresource(0, face, 1);
        HRESULT hr = context->Map(m_stagingTexture.Get(), subresource, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            CFFLog::Error("[LightProbeBaker] Failed to map staging texture face %d", face);
            return false;
        }

        // 拷贝数据到临时 buffer（因为 Unmap 后数据会失效）
        // 注意：DXGI_FORMAT_R16G16B16A16_FLOAT 是 4 个 16-bit float (HALF)
        cubemapData[face].resize(pixelCount);

        const uint16_t* src = (const uint16_t*)mapped.pData;
        const int rowPitch = mapped.RowPitch / sizeof(uint16_t);

        for (int y = 0; y < CUBEMAP_RESOLUTION; y++) {
            for (int x = 0; x < CUBEMAP_RESOLUTION; x++) {
                int srcIdx = y * rowPitch + x * 4;  // 4 channels (RGBA)
                int dstIdx = y * CUBEMAP_RESOLUTION + x;

                // 将 HALF (16-bit float) 转换为 FLOAT (32-bit float)
                cubemapData[face][dstIdx].x = PackedVector::XMConvertHalfToFloat(src[srcIdx + 0]);
                cubemapData[face][dstIdx].y = PackedVector::XMConvertHalfToFloat(src[srcIdx + 1]);
                cubemapData[face][dstIdx].z = PackedVector::XMConvertHalfToFloat(src[srcIdx + 2]);
                cubemapData[face][dstIdx].w = PackedVector::XMConvertHalfToFloat(src[srcIdx + 3]);
            }
        }

        context->Unmap(m_stagingTexture.Get(), subresource);
    }

    // // DEBUG: Export cubemap faces as PNG for verification
    // {
    //     static const char* faceNames[] = { "pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z" };
    //     std::string debugDir = FFPath::GetDebugDir() + "/lightprobe_cubemap";
        
    //     // 创建目录
    //     std::string mkdirCmd = "mkdir -p \"" + debugDir + "\"";
    //     system(mkdirCmd.c_str());
        
    //     for (int face = 0; face < 6; face++) {
    //         std::vector<uint8_t> pixels(CUBEMAP_RESOLUTION * CUBEMAP_RESOLUTION * 3);
    //         for (int i = 0; i < CUBEMAP_RESOLUTION * CUBEMAP_RESOLUTION; i++) {
    //             // Tone mapping: 简单的 Reinhard
    //             float r = cubemapData[face][i].x / (1.0f + cubemapData[face][i].x);
    //             float g = cubemapData[face][i].y / (1.0f + cubemapData[face][i].y);
    //             float b = cubemapData[face][i].z / (1.0f + cubemapData[face][i].z);
                
    //             // Gamma correction
    //             r = std::pow(r, 1.0f / 2.2f);
    //             g = std::pow(g, 1.0f / 2.2f);
    //             b = std::pow(b, 1.0f / 2.2f);
                
    //             pixels[i * 3 + 0] = (uint8_t)(std::min(r, 1.0f) * 255.0f);
    //             pixels[i * 3 + 1] = (uint8_t)(std::min(g, 1.0f) * 255.0f);
    //             pixels[i * 3 + 2] = (uint8_t)(std::min(b, 1.0f) * 255.0f);
    //         }
            
    //         std::string filename = debugDir + "/" + faceNames[face] + ".png";
    //         stbi_write_png(filename.c_str(), CUBEMAP_RESOLUTION, CUBEMAP_RESOLUTION, 3, pixels.data(), CUBEMAP_RESOLUTION * 3);
    //         CFFLog::Info("[LightProbeBaker] Exported cubemap face: %s", filename.c_str());
    //     }
    // }

    // 投影到 SH - 使用 std::array 包装输出
    std::array<XMFLOAT3, 9> shCoeffs;
    SphericalHarmonics::ProjectCubemapToSH(cubemapData, CUBEMAP_RESOLUTION, shCoeffs);

    // // DEBUG: 从 SH 重建 cubemap 并导出为 KTX2
    // // 用于验证 SH 投影和重建的正确性
    // {
    //     std::string debugDir = FFPath::GetDebugDir() + "/sh_debug";
        
    //     // L1 重建 (4 coeffs) - 最低精度
    //     std::array<XMFLOAT3, 4> shCoeffsL1;
    //     SphericalHarmonics::ProjectCubemapToSH_L1(cubemapData, CUBEMAP_RESOLUTION, shCoeffsL1);
    //     SphericalHarmonics::DebugExportSHAsCubemap_L1(shCoeffsL1, 64, debugDir, "sh_reconstructed_L1");
        
    //     // L2 重建 (9 coeffs) - 实际使用的
    //     SphericalHarmonics::DebugExportSHAsCubemap(shCoeffs, 64, debugDir, "sh_reconstructed_L2");
        
    //     // L3 重建 (16 coeffs) - 用于对比
    //     std::array<XMFLOAT3, 16> shCoeffsL3;
    //     SphericalHarmonics::ProjectCubemapToSH_L3(cubemapData, CUBEMAP_RESOLUTION, shCoeffsL3);
    //     SphericalHarmonics::DebugExportSHAsCubemap_L3(shCoeffsL3, 64, debugDir, "sh_reconstructed_L3");
        
    //     // L4 重建 (25 coeffs) - 用于对比，精度更高
    //     std::array<XMFLOAT3, 25> shCoeffsL4;
    //     SphericalHarmonics::ProjectCubemapToSH_L4(cubemapData, CUBEMAP_RESOLUTION, shCoeffsL4);
    //     SphericalHarmonics::DebugExportSHAsCubemap_L4(shCoeffsL4, 64, debugDir, "sh_reconstructed_L4");
        
    //     CFFLog::Info("[LightProbeBaker] DEBUG: Exported SH L1/L2/L3/L4 cubemaps to: %s", debugDir.c_str());
    // }

    // 拷贝结果到输出数组
    for (int i = 0; i < 9; i++) {
        outCoeffs[i] = shCoeffs[i];
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
