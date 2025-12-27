#include "Lightmap2DManager.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/Exporter/KTXExporter.h"
#include "Core/TextureManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include <fstream>
#include <filesystem>

// ============================================
// File Format
// ============================================

struct SLightmapDataHeader {
    uint32_t magic = 0x4C4D3244;  // "LM2D"
    uint32_t version = 1;
    uint32_t infoCount = 0;
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    uint32_t reserved[3] = {0, 0, 0};
};

// ============================================
// Bind
// ============================================

void CLightmap2DManager::Bind(RHI::ICommandList* cmdList) {
    if (!m_isLoaded || !cmdList) {
        return;
    }

    // Bind t16: Atlas texture
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 16, m_atlasTexture.get());

    // Bind t17: ScaleOffset structured buffer
    cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Pixel, 17, m_scaleOffsetBuffer.get());

    // Bind b7: CB_Lightmap2D (enabled flag + intensity)
    struct CB_Lightmap2D {
        int enabled;
        float intensity;
        float pad[2];
    };
    CB_Lightmap2D cb{};
    cb.enabled = 1;
    cb.intensity = 1.0f;
    cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 7, &cb, sizeof(CB_Lightmap2D));
}

// ============================================
// Save
// ============================================

bool CLightmap2DManager::SaveLightmap(
    const std::string& lightmapPath,
    const std::vector<SLightmapInfo>& infos,
    RHI::ITexture* atlasTexture)
{
    if (infos.empty()) {
        CFFLog::Error("[Lightmap2DManager] No lightmap infos to save");
        return false;
    }

    if (!atlasTexture) {
        CFFLog::Error("[Lightmap2DManager] No atlas texture to save");
        return false;
    }

    // Create lightmap folder
    std::string absLightmapPath = FFPath::GetAbsolutePath(lightmapPath);
    std::filesystem::path folderPath(absLightmapPath);

    if (!std::filesystem::exists(folderPath)) {
        std::filesystem::create_directories(folderPath);
    }

    // Save data.bin
    std::string dataPath = absLightmapPath + "/data.bin";
    if (!SaveLightmapData(dataPath, infos)) {
        return false;
    }

    // Save atlas.ktx2
    std::string atlasPath = absLightmapPath + "/atlas.ktx2";
    if (!SaveAtlasTexture(atlasPath, atlasTexture)) {
        return false;
    }

    CFFLog::Info("[Lightmap2DManager] Saved lightmap to: %s", lightmapPath.c_str());
    return true;
}

bool CLightmap2DManager::SaveLightmapData(const std::string& dataPath, const std::vector<SLightmapInfo>& infos) {
    std::ofstream file(dataPath, std::ios::binary);
    if (!file) {
        CFFLog::Error("[Lightmap2DManager] Failed to create file: %s", dataPath.c_str());
        return false;
    }

    // Write header
    SLightmapDataHeader header;
    header.infoCount = static_cast<uint32_t>(infos.size());
    header.atlasWidth = 0;   // TODO: Get from texture
    header.atlasHeight = 0;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write infos
    file.write(reinterpret_cast<const char*>(infos.data()), infos.size() * sizeof(SLightmapInfo));

    CFFLog::Info("[Lightmap2DManager] Saved %d lightmap infos to: %s",
                 static_cast<int>(infos.size()), dataPath.c_str());
    return true;
}

bool CLightmap2DManager::SaveAtlasTexture(const std::string& atlasPath, RHI::ITexture* texture) {
    if (!CKTXExporter::Export2DTextureToKTX2(texture, atlasPath)) {
        CFFLog::Error("[Lightmap2DManager] Failed to export atlas texture: %s", atlasPath.c_str());
        return false;
    }

    CFFLog::Info("[Lightmap2DManager] Exported atlas texture: %s", atlasPath.c_str());
    return true;
}

// ============================================
// Load
// ============================================

bool CLightmap2DManager::LoadLightmap(const std::string& lightmapPath) {
    UnloadLightmap();

    std::string absLightmapPath = FFPath::GetAbsolutePath(lightmapPath);

    if (!std::filesystem::exists(absLightmapPath)) {
        CFFLog::Warning("[Lightmap2DManager] Lightmap folder not found: %s", lightmapPath.c_str());
        return false;
    }

    // Load data.bin
    std::string dataPath = absLightmapPath + "/data.bin";
    if (!LoadLightmapData(dataPath)) {
        return false;
    }

    // Load atlas.ktx2
    std::string atlasPath = absLightmapPath + "/atlas.ktx2";
    if (!LoadAtlasTexture(atlasPath)) {
        return false;
    }

    // Create structured buffer
    if (!CreateScaleOffsetBuffer()) {
        return false;
    }

    m_isLoaded = true;
    CFFLog::Info("[Lightmap2DManager] Loaded lightmap from: %s", lightmapPath.c_str());
    return true;
}

bool CLightmap2DManager::LoadLightmapData(const std::string& dataPath) {
    std::ifstream file(dataPath, std::ios::binary);
    if (!file) {
        CFFLog::Error("[Lightmap2DManager] Failed to open file: %s", dataPath.c_str());
        return false;
    }

    // Read header
    SLightmapDataHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != 0x4C4D3244) {
        CFFLog::Error("[Lightmap2DManager] Invalid magic number in: %s", dataPath.c_str());
        return false;
    }

    if (header.version != 1) {
        CFFLog::Error("[Lightmap2DManager] Unsupported version %d in: %s", header.version, dataPath.c_str());
        return false;
    }

    // Read infos
    m_lightmapInfos.resize(header.infoCount);
    file.read(reinterpret_cast<char*>(m_lightmapInfos.data()), header.infoCount * sizeof(SLightmapInfo));

    CFFLog::Info("[Lightmap2DManager] Loaded %d lightmap infos", header.infoCount);
    return true;
}

bool CLightmap2DManager::LoadAtlasTexture(const std::string& atlasPath) {
    if (!std::filesystem::exists(atlasPath)) {
        CFFLog::Warning("[Lightmap2DManager] Atlas texture not found: %s", atlasPath.c_str());
        return false;
    }

    m_atlasTexture.reset(CTextureManager::Instance().Load(atlasPath, false));

    if (!m_atlasTexture) {
        CFFLog::Error("[Lightmap2DManager] Failed to load atlas texture: %s", atlasPath.c_str());
        return false;
    }

    CFFLog::Info("[Lightmap2DManager] Loaded atlas texture: %s", atlasPath.c_str());
    return true;
}

bool CLightmap2DManager::CreateScaleOffsetBuffer() {
    if (m_lightmapInfos.empty()) {
        return false;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        return false;
    }

    // Extract scaleOffset data (float4 per object)
    std::vector<DirectX::XMFLOAT4> scaleOffsets;
    scaleOffsets.reserve(m_lightmapInfos.size());

    for (const auto& info : m_lightmapInfos) {
        scaleOffsets.push_back(info.scaleOffset);
    }

    // Create structured buffer
    RHI::BufferDesc bufDesc;
    bufDesc.size = static_cast<uint32_t>(scaleOffsets.size() * sizeof(DirectX::XMFLOAT4));
    bufDesc.usage = RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(DirectX::XMFLOAT4);

    m_scaleOffsetBuffer.reset(ctx->CreateBuffer(bufDesc, scaleOffsets.data()));

    if (!m_scaleOffsetBuffer) {
        CFFLog::Error("[Lightmap2DManager] Failed to create scaleOffset buffer");
        return false;
    }

    CFFLog::Info("[Lightmap2DManager] Created scaleOffset buffer: %d entries",
                 static_cast<int>(scaleOffsets.size()));
    return true;
}

void CLightmap2DManager::UnloadLightmap() {
    m_lightmapInfos.clear();
    m_atlasTexture.reset();
    m_scaleOffsetBuffer.reset();
    m_isLoaded = false;
}

// ============================================
// Query
// ============================================

const SLightmapInfo* CLightmap2DManager::GetLightmapInfo(int index) const {
    if (index < 0 || index >= static_cast<int>(m_lightmapInfos.size())) {
        return nullptr;
    }
    return &m_lightmapInfos[index];
}
