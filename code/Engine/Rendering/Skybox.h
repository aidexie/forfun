#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include "RHI/IDescriptorSet.h"
#include <DirectXMath.h>
#include <string>
#include <functional>

// Skybox renderer using HDR cubemap
class CSkybox {
public:
    CSkybox() = default;
    ~CSkybox() = default;

    // Load HDR environment map and convert to cubemap
    // Note: This path still uses D3D11 internally for cubemap conversion (Phase 6 migration pending)
    bool Initialize(const std::string& hdrPath, int cubemapSize = 512);

    // Load pre-baked environment cubemap from KTX2
    bool InitializeFromKTX2(const std::string& ktx2Path);

    void Shutdown();

    // Render skybox
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

    // Get cubemap for IBL
    RHI::ITexture* GetEnvironmentTexture() const { return m_envTexture.get(); }
    RHI::ISampler* GetEnvironmentTextureSampler() const { return m_sampler.get(); }

private:
    void createCubeMesh();
    void createShaders();
    void createPipelineState();
    void createConstantBuffer();
    void createSampler();
    void initDescriptorSets();

    // Legacy: HDR to cubemap conversion (still D3D11)
    void convertEquirectToCubemapLegacy(const std::string& hdrPath, int size);

private:
    // RHI resources
    RHI::TexturePtr m_envTexture;
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;
    RHI::BufferPtr m_vertexBuffer;
    RHI::BufferPtr m_indexBuffer;
    RHI::BufferPtr m_constantBuffer;
    RHI::SamplerPtr m_sampler;
    RHI::PipelineStatePtr m_pso;

    // Descriptor set resources (DX12)
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    std::unique_ptr<RHI::IDescriptorSet, std::function<void(RHI::IDescriptorSet*)>> m_perPassSet;
    RHI::ShaderPtr m_vs_ds;
    RHI::ShaderPtr m_ps_ds;
    RHI::PipelineStatePtr m_pso_ds;

    // Conversion pass descriptor set resources (DX12)
    RHI::IDescriptorSetLayout* m_convLayout = nullptr;
    std::unique_ptr<RHI::IDescriptorSet, std::function<void(RHI::IDescriptorSet*)>> m_convSet;
    RHI::ShaderPtr m_convVS_ds;
    RHI::ShaderPtr m_convPS_ds;
    RHI::PipelineStatePtr m_convPSO_ds;

    uint32_t m_indexCount = 0;
    std::string m_envPathKTX2 = "";
    bool m_initialized = false;
};
