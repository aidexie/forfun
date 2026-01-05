#include "DeferredLightingPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/Scene.h"
#include "Engine/Camera.h"
#include "Engine/GameObject.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Rendering/ShadowPass.h"
#include "Engine/Rendering/ClusteredLightingPass.h"
#include "Engine/Rendering/ReflectionProbeManager.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

// ============================================
// Constant Buffer Structure
// ============================================
namespace {

struct alignas(16) CB_DeferredLighting {
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX invViewProj;

    // CSM
    int cascadeCount;
    int enableSoftShadows;
    float cascadeBlendRange;
    float shadowBias;
    XMFLOAT4 cascadeSplits;
    XMMATRIX lightSpaceVPs[4];

    // Directional light
    XMFLOAT3 lightDirWS;
    float _pad0;
    XMFLOAT3 lightColor;
    float _pad1;

    // Camera
    XMFLOAT3 camPosWS;
    float _pad2;

    // IBL
    float iblIntensity;
    int diffuseGIMode;
    int probeIndex;
    float _pad3;
};

// Full-screen triangle vertex shader
static const char* kFullScreenVS = R"(
    struct VSOut {
        float4 posH : SV_Position;
        float2 uv : TEXCOORD0;
    };

    VSOut main(uint vertexID : SV_VertexID) {
        VSOut o;
        o.uv = float2((vertexID << 1) & 2, vertexID & 2);
        o.posH = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
        return o;
    }
)";

std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // anonymous namespace

bool CDeferredLightingPass::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile full-screen triangle VS
    SCompiledShader vsCompiled = CompileShaderFromSource(kFullScreenVS, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("DeferredLightingPass VS error: %s", vsCompiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "DeferredLighting_VS";
    m_vs.reset(ctx->CreateShader(vsDesc));

    // Compile deferred lighting PS
    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";
    std::string psSource = LoadShaderSource(shaderDir + "DeferredLighting.ps.hlsl");
    if (psSource.empty()) {
        CFFLog::Error("DeferredLightingPass: Failed to load pixel shader");
        return false;
    }

    CDefaultShaderIncludeHandler includeHandler(shaderDir);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_0", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("DeferredLightingPass PS error: %s", psCompiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "DeferredLighting_PS";
    m_ps.reset(ctx->CreateShader(psDesc));

    // Create PSO
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();
    psoDesc.inputLayout = {};  // No vertex input
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.blend.blendEnable = false;
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoDesc.depthStencilFormat = ETextureFormat::Unknown;
    psoDesc.debugName = "DeferredLighting_PSO";
    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    if (!m_pso) {
        CFFLog::Error("Failed to create DeferredLightingPass PSO");
        return false;
    }

    // Create samplers
    SamplerDesc linearSampDesc;
    linearSampDesc.filter = EFilter::MinMagMipLinear;
    linearSampDesc.addressU = ETextureAddressMode::Clamp;
    linearSampDesc.addressV = ETextureAddressMode::Clamp;
    linearSampDesc.addressW = ETextureAddressMode::Clamp;
    m_linearSampler.reset(ctx->CreateSampler(linearSampDesc));

    SamplerDesc shadowSampDesc;
    shadowSampDesc.filter = EFilter::ComparisonMinMagMipLinear;
    shadowSampDesc.addressU = ETextureAddressMode::Border;
    shadowSampDesc.addressV = ETextureAddressMode::Border;
    shadowSampDesc.addressW = ETextureAddressMode::Border;
    shadowSampDesc.borderColor[0] = 1.0f;
    shadowSampDesc.borderColor[1] = 1.0f;
    shadowSampDesc.borderColor[2] = 1.0f;
    shadowSampDesc.borderColor[3] = 1.0f;
    shadowSampDesc.comparisonFunc = EComparisonFunc::LessEqual;
    m_shadowSampler.reset(ctx->CreateSampler(shadowSampDesc));

    SamplerDesc pointSampDesc;
    pointSampDesc.filter = EFilter::MinMagMipPoint;
    pointSampDesc.addressU = ETextureAddressMode::Clamp;
    pointSampDesc.addressV = ETextureAddressMode::Clamp;
    m_pointSampler.reset(ctx->CreateSampler(pointSampDesc));

    CFFLog::Info("DeferredLightingPass initialized");
    return true;
}

void CDeferredLightingPass::Shutdown()
{
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_linearSampler.reset();
    m_shadowSampler.reset();
    m_pointSampler.reset();
}

void CDeferredLightingPass::Render(
    const CCamera& camera,
    CScene& scene,
    CGBuffer& gbuffer,
    RHI::ITexture* hdrOutput,
    uint32_t width,
    uint32_t height,
    const CShadowPass* shadowPass,
    CClusteredLightingPass* clusteredLighting,
    RHI::ITexture* ssaoTexture)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    CScopedDebugEvent evt(cmdList, L"Deferred Lighting Pass");

    // Set render target (HDR output)
    cmdList->SetRenderTargets(1, &hdrOutput, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height);
    cmdList->SetScissorRect(0, 0, width, height);

    // Clear HDR buffer
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTarget(hdrOutput, clearColor);

    // Set pipeline state
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // ============================================
    // Bind G-Buffer textures (t0-t5)
    // ============================================
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, gbuffer.GetWorldPosMetallic());
    cmdList->SetShaderResource(EShaderStage::Pixel, 1, gbuffer.GetNormalRoughness());
    cmdList->SetShaderResource(EShaderStage::Pixel, 2, gbuffer.GetAlbedoAO());
    cmdList->SetShaderResource(EShaderStage::Pixel, 3, gbuffer.GetEmissiveMaterialID());
    cmdList->SetShaderResource(EShaderStage::Pixel, 4, gbuffer.GetVelocity());
    cmdList->SetShaderResource(EShaderStage::Pixel, 5, gbuffer.GetDepthBuffer());

    // ============================================
    // Bind Shadow maps (t6) and BRDF LUT (t7)
    // ============================================
    const CShadowPass::Output* shadowData = shadowPass ? &shadowPass->GetOutput() : nullptr;
    if (shadowData && shadowData->shadowMapArray) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 6, shadowData->shadowMapArray);
    }

    // ============================================
    // Bind IBL textures (t7, t16-t17)
    // Note: t8-t10 used by ClusteredShading, t11-t15 by VolumetricLightmap
    // ============================================
    auto& probeManager = scene.GetProbeManager();
    cmdList->SetShaderResource(EShaderStage::Pixel, 7, probeManager.GetBrdfLutTexture());
    cmdList->SetShaderResource(EShaderStage::Pixel, 16, probeManager.GetIrradianceArrayTexture());
    cmdList->SetShaderResource(EShaderStage::Pixel, 17, probeManager.GetPrefilteredArrayTexture());

    // ============================================
    // Bind SSAO texture (t18)
    // ============================================
    if (ssaoTexture) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 18, ssaoTexture);
    }

    // ============================================
    // Bind Clustered Lighting data (t8-t10, b3)
    // ============================================
    if (clusteredLighting) {
        clusteredLighting->BindToMainPass(cmdList);
    }

    // ============================================
    // Bind Volumetric Lightmap (t20-t24, b6)
    // ============================================
    scene.GetVolumetricLightmap().Bind(cmdList);

    // ============================================
    // Bind Samplers (s0-s1, s3)
    // Note: s2 used by VolumetricLightmap.hlsl
    // ============================================
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
    if (shadowData && shadowData->shadowSampler) {
        cmdList->SetSampler(EShaderStage::Pixel, 1, shadowData->shadowSampler);
    } else {
        cmdList->SetSampler(EShaderStage::Pixel, 1, m_shadowSampler.get());
    }
    cmdList->SetSampler(EShaderStage::Pixel, 3, m_pointSampler.get());

    // ============================================
    // Update Constant Buffer
    // ============================================
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = camera.GetProjectionMatrix();
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    // Find directional light
    SDirectionalLight* dirLight = nullptr;
    for (auto& objPtr : scene.GetWorld().Objects()) {
        dirLight = objPtr->GetComponent<SDirectionalLight>();
        if (dirLight) break;
    }

    CB_DeferredLighting cb = {};
    cb.view = XMMatrixTranspose(view);
    cb.proj = XMMatrixTranspose(proj);
    cb.invViewProj = XMMatrixTranspose(invViewProj);

    // CSM data
    if (shadowData) {
        cb.cascadeCount = shadowData->cascadeCount;
        cb.enableSoftShadows = shadowData->enableSoftShadows ? 1 : 0;
        cb.cascadeBlendRange = shadowData->cascadeBlendRange;
        cb.cascadeSplits = XMFLOAT4(
            (0 < shadowData->cascadeCount) ? shadowData->cascadeSplits[0] : 100.0f,
            (1 < shadowData->cascadeCount) ? shadowData->cascadeSplits[1] : 100.0f,
            (2 < shadowData->cascadeCount) ? shadowData->cascadeSplits[2] : 100.0f,
            (3 < shadowData->cascadeCount) ? shadowData->cascadeSplits[3] : 100.0f
        );
        for (int i = 0; i < 4; ++i) {
            cb.lightSpaceVPs[i] = XMMatrixTranspose(shadowData->lightSpaceVPs[i]);
        }
    } else {
        cb.cascadeCount = 1;
        cb.enableSoftShadows = 1;
        for (int i = 0; i < 4; ++i) {
            cb.lightSpaceVPs[i] = XMMatrixTranspose(XMMatrixIdentity());
        }
    }

    // Directional light
    if (dirLight) {
        cb.lightDirWS = dirLight->GetDirection();
        cb.lightColor = XMFLOAT3(
            dirLight->color.x * dirLight->intensity,
            dirLight->color.y * dirLight->intensity,
            dirLight->color.z * dirLight->intensity
        );
        cb.shadowBias = dirLight->shadow_bias;
        cb.iblIntensity = dirLight->ibl_intensity;
    } else {
        cb.lightDirWS = XMFLOAT3(0.4f, -1.0f, 0.2f);
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cb.lightDirWS));
        XMStoreFloat3(&cb.lightDirWS, L);
        cb.lightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        cb.shadowBias = 0.005f;
        cb.iblIntensity = 1.0f;
    }

    cb.camPosWS = camera.position;
    cb.diffuseGIMode = static_cast<int>(scene.GetLightSettings().diffuseGIMode);
    cb.probeIndex = 0;  // Default global probe

    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));

    // ============================================
    // Draw full-screen triangle
    // ============================================
    cmdList->Draw(3, 0);
}
