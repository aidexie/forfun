#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include "RHI/ICommandList.h"
#include <cstdint>

struct SAntiAliasingSettings;
enum class EAntiAliasingMode : int;

// Post-process anti-aliasing pass supporting FXAA (fast, single-pass) and SMAA (higher quality, 3-pass).
// Pipeline: PostProcess (Tonemapping) -> [AntiAliasing] -> Debug Lines/Grid -> Output
// Input/Output: LDR texture (R8G8B8A8_UNORM_SRGB)
class CAntiAliasingPass {
public:
    CAntiAliasingPass() = default;
    ~CAntiAliasingPass() = default;

    bool Initialize();
    void Shutdown();

    void Render(RHI::ITexture* inputTexture,
                RHI::ITexture* outputTexture,
                uint32_t width, uint32_t height,
                const SAntiAliasingSettings& settings);

    bool IsEnabled(const SAntiAliasingSettings& settings) const;

private:
    void renderFXAA(RHI::ICommandList* cmdList,
                    RHI::ITexture* input, RHI::ITexture* output,
                    uint32_t width, uint32_t height,
                    const SAntiAliasingSettings& settings);

    void renderSMAA(RHI::ICommandList* cmdList,
                    RHI::ITexture* input, RHI::ITexture* output,
                    uint32_t width, uint32_t height,
                    const SAntiAliasingSettings& settings);

    void createSharedResources();
    void createFXAAResources();
    void createSMAAResources();
    void ensureSMAATextures(uint32_t width, uint32_t height);

    // Shared resources
    RHI::BufferPtr m_fullscreenQuadVB;
    RHI::SamplerPtr m_linearSampler;
    RHI::SamplerPtr m_pointSampler;
    RHI::ShaderPtr m_fullscreenVS;

    // FXAA resources
    RHI::ShaderPtr m_fxaaPS;
    RHI::PipelineStatePtr m_fxaaPSO;

    // SMAA resources (3-pass)
    RHI::ShaderPtr m_smaaEdgePS;
    RHI::PipelineStatePtr m_smaaEdgePSO;
    RHI::TexturePtr m_smaaEdgesTex;

    RHI::ShaderPtr m_smaaBlendPS;
    RHI::PipelineStatePtr m_smaaBlendPSO;
    RHI::TexturePtr m_smaaBlendTex;

    RHI::ShaderPtr m_smaaNeighborPS;
    RHI::PipelineStatePtr m_smaaNeighborPSO;

    RHI::TexturePtr m_smaaAreaTex;
    RHI::TexturePtr m_smaaSearchTex;

    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;
    bool m_initialized = false;
};
