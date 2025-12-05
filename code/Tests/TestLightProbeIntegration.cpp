// ============================================
// TestLightProbeIntegration.cpp
// ============================================
// 测试 Light Probe 在渲染管线中的集成
// 验证 SH 系数被正确传输到 Shader 并产生 diffuse lighting 效果
//
// 测试场景：
// - 一个红色彩色 Light Probe
// - 一个白色球体在 Probe 范围内
// - 期望：球体呈现红色环境光
// ============================================

#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/LightProbe.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Rendering/LightProbeManager.h"
#include <DirectXMath.h>

using namespace DirectX;

class CTestLightProbeIntegration : public ITestCase
{
public:
    const char* GetName() const override { return "TestLightProbeIntegration"; }

    void Setup(CTestContext& ctx) override
    {
        ctx.OnFrame(1, [&]() {
            CFFLog::Info("[TestLightProbeIntegration] Frame 1: Setting up scene");

            auto& scene = CScene::Instance();

            // =============================================
            // 1. Create a dim directional light (let IBL be visible)
            // =============================================
            auto* lightObj = scene.GetWorld().Create("MainLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->position = {0, 10, 0};

            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 1.0f, 1.0f);
            dirLight->intensity = 0.1f;  // Very dim to let IBL shine
            dirLight->ibl_intensity = 0.0f;  // Disable global IBL, use Light Probe only

            // =============================================
            // 2. Create a white sphere at origin
            // =============================================
            auto* sphere = scene.GetWorld().Create("TestSphere");
            auto* sphereTransform = sphere->AddComponent<STransform>();
            sphereTransform->position = XMFLOAT3(0.0f, 0.0f, 5.0f);  // In front of camera
            sphereTransform->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);

            auto* meshRenderer = sphere->AddComponent<SMeshRenderer>();
            meshRenderer->path = "mesh/sphere.obj";
            meshRenderer->materialPath = "materials/default_white.ffasset";

            // =============================================
            // 3. Create a Light Probe with RED SH coefficients
            // =============================================
            auto* probeObj = scene.GetWorld().Create("RedLightProbe");
            auto* probeTransform = probeObj->AddComponent<STransform>();
            probeTransform->position = XMFLOAT3(0.0f, 0.0f, 5.0f);  // Same as sphere

            auto* lightProbe = probeObj->AddComponent<SLightProbe>();
            lightProbe->radius = 20.0f;  // Large radius to cover sphere

            // Set SH coefficients to produce RED ambient light
            // L0 (DC term) = sqrt(1/4π) ≈ 0.282
            // For uniform red ambient: shCoeffs[0] = (R, 0, 0) * scale
            float ambientScale = 3.0f;  // Brighten the effect
            lightProbe->shCoeffs[0] = XMFLOAT3(0.282f * ambientScale, 0.0f, 0.0f);  // L0: Red

            // L1 terms (directional) - set to zero for uniform ambient
            for (int i = 1; i < 9; i++) {
                lightProbe->shCoeffs[i] = XMFLOAT3(0.0f, 0.0f, 0.0f);
            }

            lightProbe->isDirty = false;  // Mark as baked

            // =============================================
            // 4. Reload Light Probes
            // =============================================
            scene.ReloadLightProbesFromScene();

            CFFLog::Info("[TestLightProbeIntegration] Setup complete");
            CFFLog::Info("[TestLightProbeIntegration] Light probes loaded: %d",
                        scene.GetLightProbeManager().GetProbeCount());
        });

        ctx.OnFrame(5, [&]() {
            auto& scene = CScene::Instance();
            int probeCount = scene.GetLightProbeManager().GetProbeCount();

            ASSERT(ctx, probeCount > 0, "At least one Light Probe should be loaded");
            ASSERT(ctx, probeCount == 1, "Exactly one Light Probe should exist");

            CFFLog::Info("[TestLightProbeIntegration] Frame 5: %d light probes active", probeCount);
        });

        ctx.OnFrame(20, [&]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);
            CFFLog::Info("[TestLightProbeIntegration] Screenshot captured");
        });

        ctx.OnFrame(25, [&]() {
            CFFLog::Info("[TestLightProbeIntegration] Test complete");
            CFFLog::Info("[TestLightProbeIntegration] VISUAL CHECK: The sphere should have a RED tint");
            CFFLog::Info("[TestLightProbeIntegration] from the Light Probe's SH coefficients.");

            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestLightProbeIntegration)
