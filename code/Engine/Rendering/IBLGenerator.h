#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/IDescriptorSet.h"
#include <DirectXMath.h>
#include <string>
#include <memory>
#include <array>

// ============================================
// IBL (Image-Based Lighting) Generator
// ============================================
// Generates diffuse irradiance map and specular prefiltered environment map
//
// 所有成员使用 RHI 抽象类型，不暴露任何 D3D11 类型
class CIBLGenerator {
public:
    CIBLGenerator();
    ~CIBLGenerator();

    // Non-copyable
    CIBLGenerator(const CIBLGenerator&) = delete;
    CIBLGenerator& operator=(const CIBLGenerator&) = delete;

    // Initialize generator resources
    bool Initialize();
    void Shutdown();

    // ============================================
    // Load pre-baked IBL textures from KTX2 files
    // ============================================
    bool LoadIrradianceFromKTX2(const std::string& ktx2Path);
    bool LoadPreFilteredFromKTX2(const std::string& ktx2Path);
    bool LoadBrdfLutFromKTX2(const std::string& ktx2Path);

    // ============================================
    // Generate IBL maps from environment cubemap
    // ============================================

    // Generate diffuse irradiance map from environment cubemap
    // envMap: Environment cubemap texture with SRV
    // outputSize: Output cubemap resolution (default 32)
    // Returns: Generated irradiance map texture (owned by IBLGenerator)
    RHI::ITexture* GenerateIrradianceMap(RHI::ITexture* envMap, int outputSize = 32);

    // Generate specular pre-filtered environment map
    // envMap: Environment cubemap texture with SRV
    // outputSize: Output cubemap resolution (default 128)
    // numMipLevels: Number of roughness levels (default 7 for full range)
    // Returns: Generated pre-filtered map texture (owned by IBLGenerator)
    RHI::ITexture* GeneratePreFilteredMap(RHI::ITexture* envMap, int outputSize = 128, int numMipLevels = 7);

    // Generate BRDF LUT for Split Sum Approximation
    // resolution: LUT resolution (typically 512x512)
    // Returns: Generated BRDF LUT texture (owned by IBLGenerator)
    RHI::ITexture* GenerateBrdfLut(int resolution = 512);

    // ============================================
    // Get rendering resources (for PBR shader usage)
    // ============================================
    RHI::ITexture* GetIrradianceTexture() const { return m_irradianceTexture.get(); }
    RHI::ITexture* GetPreFilteredTexture() const { return m_preFilteredTexture.get(); }
    RHI::ITexture* GetBrdfLutTexture() const { return m_brdfLutTexture.get(); }

    // Get the number of mip levels in the pre-filtered map (0 if not generated)
    int GetPreFilteredMipLevels() const { return m_preFilteredMipLevels; }

    // ============================================
    // Debug utilities
    // ============================================

    // Save the generated irradiance map to DDS file
    bool SaveIrradianceMapToDDS(const std::string& filepath);

    // Save the generated irradiance map to HDR files (Radiance RGBE format)
    // Creates 6 files: filepath_posX.hdr, filepath_negX.hdr, etc.
    bool SaveIrradianceMapToHDR(const std::string& filepath);

private:
    void createShaders();
    void initDescriptorSets();

    // Check if descriptor set mode is available (DX12 only)
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_irradiancePSO_ds != nullptr; }

private:
    // Rendering resources (RHI abstractions)
#ifndef FF_LEGACY_BINDING_DISABLED
    RHI::ShaderPtr m_fullscreenVS;
    RHI::ShaderPtr m_irradiancePS;
    RHI::ShaderPtr m_prefilterPS;
    RHI::ShaderPtr m_brdfLutPS;
#endif
    RHI::SamplerPtr m_sampler;
    RHI::BufferPtr m_cbFaceIndex;
    RHI::BufferPtr m_cbRoughness;

    // Descriptor set resources (DX12)
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;
    RHI::ShaderPtr m_fullscreenVS_ds;
    RHI::ShaderPtr m_irradiancePS_ds;
    RHI::ShaderPtr m_prefilterPS_ds;
    RHI::ShaderPtr m_brdfLutPS_ds;
    RHI::PipelineStatePtr m_irradiancePSO_ds;
    RHI::PipelineStatePtr m_prefilterPSO_ds;
    RHI::PipelineStatePtr m_brdfLutPSO_ds;

    // Generated/loaded irradiance map
    RHI::TexturePtr m_irradianceTexture;

    // Generated/loaded pre-filtered map
    RHI::TexturePtr m_preFilteredTexture;
    int m_preFilteredMipLevels = 0;

    // Generated/loaded BRDF LUT
    RHI::TexturePtr m_brdfLutTexture;

    bool m_initialized = false;
};
