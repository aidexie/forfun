#include "Lightmap2DManager.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/TextureManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include <fstream>
#include <filesystem>

// ============================================
// File Format (must match LightmapBaker)
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
// Query
// ============================================

RHI::ITexture* CLightmap2DManager::GetAtlasTexture() const {
    // Return whichever texture is valid (owned takes priority)
    if (m_atlasTextureOwned) {
        return m_atlasTextureOwned.get();
    }
    return m_atlasHandle ? m_atlasHandle->GetTexture() : nullptr;
}

// ============================================
// Bind
// ============================================

void CLightmap2DManager::Bind(RHI::ICommandList* cmdList) {
    if (!m_isLoaded || !cmdList) {
        return;
    }

    // Bind t16: Atlas texture
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 16, GetAtlasTexture());

    // Bind t17: ScaleOffset structured buffer
    cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Pixel, 17, m_scaleOffsetBuffer.get());
}

// ============================================
// Direct Data Transfer
// ============================================

void CLightmap2DManager::SetBakedData(
    RHI::TexturePtr atlasTexture,
    const std::vector<SLightmapInfo>& infos
) {
    // Unload any existing data first
    UnloadLightmap();

    // Take unique ownership of the texture (from baker)
    m_atlasTextureOwned = std::move(atlasTexture);
    m_atlasHandle.reset();  // Clear async-loaded handle

    // Copy lightmap infos
    m_lightmapInfos = infos;

    // Create structured buffer for scaleOffsets
    if (!CreateScaleOffsetBuffer()) {
        CFFLog::Error("[Lightmap2DManager] SetBakedData: Failed to create scaleOffset buffer");
        UnloadLightmap();
        return;
    }

    m_isLoaded = true;
    CFFLog::Info("[Lightmap2DManager] SetBakedData: Loaded %d lightmap infos directly from baker",
                 static_cast<int>(infos.size()));
}

// ============================================
// Load
// ============================================

bool CLightmap2DManager::LoadLightmap(const std::string& lightmapPath) {
    UnloadLightmap();

    // Store path for hot-reload
    m_loadedPath = lightmapPath;

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

    // Use async loading from TextureManager (returns placeholder until ready)
    m_atlasHandle = CTextureManager::Instance().LoadAsync(atlasPath, false);
    m_atlasTextureOwned.reset();  // Clear owned ptr

    if (!m_atlasHandle) {
        CFFLog::Error("[Lightmap2DManager] Failed to create texture handle: %s", atlasPath.c_str());
        return false;
    }

    CFFLog::Info("[Lightmap2DManager] Queued atlas texture for async load: %s", atlasPath.c_str());
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
    m_atlasTextureOwned.reset();
    m_atlasHandle.reset();
    m_scaleOffsetBuffer.reset();
    m_isLoaded = false;
    // Note: m_loadedPath is preserved for hot-reload
}

// ============================================
// Hot-Reload
// ============================================

bool CLightmap2DManager::ReloadLightmap() {
    if (m_loadedPath.empty()) {
        CFFLog::Warning("[Lightmap2DManager] No lightmap path to reload");
        return false;
    }

    CFFLog::Info("[Lightmap2DManager] Reloading lightmap: %s", m_loadedPath.c_str());

    // LoadLightmap will call UnloadLightmap internally,
    // which defers GPU resource deletion via CDX12DeferredDeletionQueue
    std::string pathCopy = m_loadedPath;  // Copy since UnloadLightmap doesn't clear it
    return LoadLightmap(pathCopy);
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
