#include "LightProbeBaker.h"
#include "CubemapRenderer.h"
#include "ForwardRenderPipeline.h"
#include "RenderPipeline.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
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
using namespace DirectX;

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

    m_cubemapRT.reset();
    m_depthBuffer.reset();
    m_stagingTexture.reset();

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

    // RenderDoc 捕获：自动捕获第一个面的渲染
    static bool s_captureFirstFace = true;
    if (s_captureFirstFace) {
        CRenderDocCapture::BeginFrameCapture();
        s_captureFirstFace = false;
    }

    // 1. 渲染 Cubemap
    renderToCubemap(position, scene, m_cubemapRT.get());

    // 在所有 6 个面渲染完成后结束 RenderDoc 捕获
    CRenderDocCapture::EndFrameCapture();

    // 2. 投影到 SH 系数
    if (!projectCubemapToSH(m_cubemapRT.get(), probe.shCoeffs)) {
        CFFLog::Error("[LightProbeBaker] Failed to project cubemap to SH");
        return false;
    }

    // 3. 标记为已烘焙
    probe.isDirty = false;

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
    RHI::ITexture* outputCubemap)
{
    // 使用共享的 CubemapRenderer
    CCubemapRenderer::RenderToCubemap(
        position,
        CUBEMAP_RESOLUTION,
        scene,
        m_pipeline.get(),
        outputCubemap);
}

// ============================================
// SH Projection
// ============================================

bool CLightProbeBaker::projectCubemapToSH(
    RHI::ITexture* cubemap,
    XMFLOAT3 outCoeffs[9])
{
    auto* cmdList = RHI::CRHIManager::Instance().GetRenderContext()->GetCommandList();

    // 拷贝 cubemap 到 staging texture（用于 CPU 读取）
    cmdList->CopyTexture(m_stagingTexture.get(), cubemap);

    // 读取 6 个面的像素数据
    const int pixelCount = CUBEMAP_RESOLUTION * CUBEMAP_RESOLUTION;
    std::array<std::vector<XMFLOAT4>, 6> cubemapData;

    for (int face = 0; face < 6; face++)
    {
        // Map staging texture using RHI interface
        RHI::MappedTexture mapped = m_stagingTexture->Map(face, 0);
        if (!mapped.pData) {
            CFFLog::Error("[LightProbeBaker] Failed to map staging texture face %d", face);
            return false;
        }

        // 拷贝数据到临时 buffer
        // 注意：R16G16B16A16_FLOAT 是 4 个 16-bit float (HALF)
        cubemapData[face].resize(pixelCount);

        const uint16_t* src = static_cast<const uint16_t*>(mapped.pData);
        const int rowPitch = mapped.rowPitch / sizeof(uint16_t);

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

        m_stagingTexture->Unmap(face, 0);
    }

    // 投影到 SH
    std::array<XMFLOAT3, 9> shCoeffs;
    SphericalHarmonics::ProjectCubemapToSH(cubemapData, CUBEMAP_RESOLUTION, shCoeffs);

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
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();

    // 创建 Cubemap render target (R16G16B16A16_FLOAT, 6 faces)
    RHI::TextureDesc cubemapDesc;
    cubemapDesc.width = CUBEMAP_RESOLUTION;
    cubemapDesc.height = CUBEMAP_RESOLUTION;
    cubemapDesc.mipLevels = 1;
    cubemapDesc.arraySize = 1;  // Will be 6 due to isCubemap
    cubemapDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    cubemapDesc.usage = RHI::ETextureUsage::RenderTarget | RHI::ETextureUsage::ShaderResource;
    cubemapDesc.isCubemap = true;
    cubemapDesc.debugName = "LightProbeBaker_CubemapRT";

    m_cubemapRT.reset(renderContext->CreateTexture(cubemapDesc));
    if (!m_cubemapRT) {
        CFFLog::Error("[LightProbeBaker] Failed to create cubemap RT");
        return false;
    }

    // 创建 depth buffer
    RHI::TextureDesc depthDesc;
    depthDesc.width = CUBEMAP_RESOLUTION;
    depthDesc.height = CUBEMAP_RESOLUTION;
    depthDesc.mipLevels = 1;
    depthDesc.arraySize = 1;
    depthDesc.format = RHI::ETextureFormat::D24_UNORM_S8_UINT;
    depthDesc.usage = RHI::ETextureUsage::DepthStencil;
    depthDesc.debugName = "LightProbeBaker_DepthBuffer";

    m_depthBuffer.reset(renderContext->CreateTexture(depthDesc));
    if (!m_depthBuffer) {
        CFFLog::Error("[LightProbeBaker] Failed to create depth buffer");
        return false;
    }

    // 创建 staging texture 用于 CPU readback
    RHI::TextureDesc stagingDesc;
    stagingDesc.width = CUBEMAP_RESOLUTION;
    stagingDesc.height = CUBEMAP_RESOLUTION;
    stagingDesc.mipLevels = 1;
    stagingDesc.arraySize = 1;  // Will be 6 due to isCubemap
    stagingDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    stagingDesc.usage = RHI::ETextureUsage::Staging;
    stagingDesc.isCubemap = true;
    stagingDesc.debugName = "LightProbeBaker_StagingTexture";

    m_stagingTexture.reset(renderContext->CreateTexture(stagingDesc));
    if (!m_stagingTexture) {
        CFFLog::Error("[LightProbeBaker] Failed to create staging texture");
        return false;
    }

    return true;
}
