#include "ReflectionProbeManager.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/ReflectionProbe.h"
#include "Core/ReflectionProbeAsset.h"
#include "Core/Loader/KTXLoader.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerFrameSlots.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <DirectXPackedVector.h>
#include <filesystem>

using namespace DirectX;
using namespace DirectX::PackedVector;

// ============================================
// Public Interface
// ============================================

bool CReflectionProbeManager::Initialize()
{
    if (m_initialized) return true;

    if (!createCubeArrays()) {
        CFFLog::Error("[ReflectionProbeManager] Failed to create cube arrays");
        return false;
    }

    if (!createConstantBuffer()) {
        CFFLog::Error("[ReflectionProbeManager] Failed to create constant buffer");
        return false;
    }

    // 设置默认全局 IBL (index 0) - 纯灰色 fallback
    // 防止没有加载 skybox 时 IBL 为空
    fillSliceWithSolidColor(0, 0.2f, 0.2f, 0.2f);  // 灰色环境光
    m_probeData.probes[0].position = { 0, 0, 0 };
    m_probeData.probes[0].radius = 1e10f;  // 无限大 (fallback)
    m_probeCount = 1;
    m_probeData.probeCount = 1;

    // 更新常量缓冲区
    updateConstantBuffer();

    // Note: Don't add barriers here - LoadGlobalProbe() or LoadLocalProbesFromScene()
    // will be called during scene loading and will transition to ShaderResource state.
    // Adding barriers here would cause duplicate barrier warnings when those functions
    // do their copy operations (which transition back to CopyDest).

    m_initialized = true;
    CFFLog::Info("[ReflectionProbeManager] Initialized (max %d probes, default fallback IBL set)", MAX_PROBES);
    return true;
}

void CReflectionProbeManager::Shutdown()
{
    m_irradianceArray.reset();
    m_prefilteredArray.reset();
    m_brdfLutTexture.reset();
    m_cbProbes.reset();
    m_probeCount = 0;
    m_initialized = false;
}

void CReflectionProbeManager::LoadLocalProbesFromScene(CScene& scene)
{
    if (!m_initialized) {
        CFFLog::Error("[ReflectionProbeManager] Not initialized");
        return;
    }

    // 保留全局 IBL (index 0)，只重置局部 Probe 数据 (index 1-7)
    // 全局 IBL 由 Initialize() 设置默认值，或通过 LoadGlobalProbe() 更新
    for (int i = 1; i < MAX_PROBES; i++) {
        m_probeData.probes[i] = {};
    }
    m_probeCount = 1;  // 保留全局 probe

    // ============================================
    // Index 1-7: 局部 Probe
    // ============================================
    for (auto& objPtr : scene.GetWorld().Objects())
    {
        if (m_probeCount >= MAX_PROBES) {
            CFFLog::Warning("[ReflectionProbeManager] Max probe count reached (%d)", MAX_PROBES);
            break;
        }

        auto* probeComp = objPtr->GetComponent<SReflectionProbe>();
        if (!probeComp) continue;

        auto* transform = objPtr->GetComponent<STransform>();
        if (!transform) continue;

        if (probeComp->assetPath.empty()) {
            CFFLog::Warning("ReflectionProbe on '%s' has no assetPath, skipping",
                           objPtr->GetName().c_str());
            continue;
        }

        // 构建 KTX2 路径
        std::string assetFullPath = FFPath::GetAbsolutePath(probeComp->assetPath);
        CReflectionProbeAsset asset;
        if (!asset.LoadFromFile(assetFullPath)) {
            CFFLog::Warning("Failed to load probe asset: %s", assetFullPath.c_str());
            continue;
        }

        std::filesystem::path assetDir = std::filesystem::path(assetFullPath).parent_path();
        std::string irradiancePath = (assetDir / asset.m_irradianceMap).string();
        std::string prefilteredPath = (assetDir / asset.m_prefilteredMap).string();

        // 加载并拷贝到 Array
        int sliceIndex = m_probeCount;
        if (loadAndCopyToArray(irradiancePath, sliceIndex, true) &&
            loadAndCopyToArray(prefilteredPath, sliceIndex, false))
        {
            m_probeData.probes[sliceIndex].position = transform->position;
            m_probeData.probes[sliceIndex].radius = probeComp->radius;
            m_probeCount++;

            CFFLog::Info("[ReflectionProbeManager] Loaded probe '%s' at index %d (pos=%.1f,%.1f,%.1f r=%.1f)",
                        objPtr->GetName().c_str(), sliceIndex,
                        transform->position.x, transform->position.y, transform->position.z,
                        probeComp->radius);
        }
    }

    // 更新常量缓冲区
    m_probeData.probeCount = m_probeCount;
    updateConstantBuffer();

    // Transition arrays from CopyDest to ShaderResource for consumers
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();
    cmdList->Barrier(m_irradianceArray.get(), RHI::EResourceState::CopyDest, RHI::EResourceState::ShaderResource);
    cmdList->Barrier(m_prefilteredArray.get(), RHI::EResourceState::CopyDest, RHI::EResourceState::ShaderResource);

    CFFLog::Info("[ReflectionProbeManager] Total probes loaded: %d", m_probeCount);
}

void CReflectionProbeManager::Bind(RHI::ICommandList* cmdList)
{
    if (!m_initialized || !cmdList) return;

#ifndef FF_LEGACY_BINDING_DISABLED
    // Legacy binding path - use descriptor sets via PopulatePerFrameSet() instead
    // t5: IrradianceArray
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 5, m_irradianceArray.get());

    // t6: PrefilteredArray
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 6, m_prefilteredArray.get());

    // t7: BRDF LUT
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 7, m_brdfLutTexture.get());

    // b4: CB_Probes (use SetConstantBufferData for DX12 compatibility)
    cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 4, &m_probeData, sizeof(CB_Probes));
#else
    CFFLog::Warning("[ReflectionProbeManager] Bind() called but legacy binding is disabled. Use PopulatePerFrameSet() with descriptor sets instead.");
#endif
}

void CReflectionProbeManager::PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet)
{
    using namespace PerFrameSlots;

    if (!perFrameSet || !m_initialized) return;

    // Bind textures to PerFrame set using PerFrameSlots constants
    perFrameSet->Bind({
        RHI::BindingSetItem::Texture_SRV(Tex::BrdfLUT, m_brdfLutTexture.get()),
        RHI::BindingSetItem::Texture_SRV(Tex::IrradianceArray, m_irradianceArray.get()),
        RHI::BindingSetItem::Texture_SRV(Tex::PrefilteredArray, m_prefilteredArray.get()),
        RHI::BindingSetItem::VolatileCBV(CB::ReflectionProbe, &m_probeData, sizeof(CB_Probes))
    });
}

int CReflectionProbeManager::SelectProbeForPosition(const DirectX::XMFLOAT3& worldPos) const
{
    int bestIndex = 0;  // Default: global IBL (index 0)
    float bestDistSq = 1e20f;

    // Search local probes (index 1+), find nearest containing probe
    for (int i = 1; i < m_probeCount; i++) {
        float dx = worldPos.x - m_probeData.probes[i].position.x;
        float dy = worldPos.y - m_probeData.probes[i].position.y;
        float dz = worldPos.z - m_probeData.probes[i].position.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        float radiusSq = m_probeData.probes[i].radius * m_probeData.probes[i].radius;

        // Check if inside probe radius and closer than current best
        if (distSq < radiusSq && distSq < bestDistSq) {
            bestDistSq = distSq;
            bestIndex = i;
        }
    }

    return bestIndex;
}

bool CReflectionProbeManager::LoadGlobalProbe(const std::string& irrPath, const std::string& prefPath)
{
    if (!m_initialized) {
        CFFLog::Error("[ReflectionProbeManager] Not initialized");
        return false;
    }

    // Load global IBL at index 0
    if (!loadAndCopyToArray(irrPath, 0, true)) {
        CFFLog::Error("[ReflectionProbeManager] Failed to load global irradiance");
        return false;
    }

    if (!loadAndCopyToArray(prefPath, 0, false)) {
        CFFLog::Error("[ReflectionProbeManager] Failed to load global prefiltered");
        return false;
    }

    // Update probe data for index 0
    m_probeData.probes[0].position = { 0, 0, 0 };
    m_probeData.probes[0].radius = 1e10f;  // Infinite (fallback)

    // Ensure probe count includes global
    if (m_probeCount < 1) {
        m_probeCount = 1;
    }
    m_probeData.probeCount = m_probeCount;

    // Update constant buffer
    updateConstantBuffer();

    // Transition arrays from CopyDest to ShaderResource for consumers
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();
    cmdList->Barrier(m_irradianceArray.get(), RHI::EResourceState::CopyDest, RHI::EResourceState::ShaderResource);
    cmdList->Barrier(m_prefilteredArray.get(), RHI::EResourceState::CopyDest, RHI::EResourceState::ShaderResource);

    CFFLog::Info("[ReflectionProbeManager] Global probe (index 0) reloaded");
    return true;
}

bool CReflectionProbeManager::LoadBrdfLut(const std::string& brdfLutPath)
{
    if (!m_initialized) {
        CFFLog::Error("[ReflectionProbeManager] Not initialized");
        return false;
    }

    // Load BRDF LUT using KTXLoader
    RHI::ITexture* rhiTexture = CKTXLoader::Load2DTextureFromKTX2(brdfLutPath);
    if (!rhiTexture) {
        CFFLog::Error("[ReflectionProbeManager] Failed to load BRDF LUT: %s", brdfLutPath.c_str());
        return false;
    }

    // Store RHI texture
    m_brdfLutTexture.reset(rhiTexture);

    CFFLog::Info("[ReflectionProbeManager] Loaded BRDF LUT: %s", brdfLutPath.c_str());
    return true;
}

// ============================================
// Internal Methods
// ============================================

bool CReflectionProbeManager::createCubeArrays()
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return false;

    // ============================================
    // Irradiance Array: 32x32, 1 mip, 8 cubes
    // ============================================
    {
        RHI::TextureDesc desc = RHI::TextureDesc::CubemapArray(
            IRRADIANCE_SIZE,
            MAX_PROBES,
            RHI::ETextureFormat::R16G16B16A16_FLOAT,
            1  // 1 mip level
        );
        desc.debugName = "ReflectionProbeManager_IrradianceArray";

        m_irradianceArray.reset(renderContext->CreateTexture(desc));
        if (!m_irradianceArray) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create irradiance array");
            return false;
        }
    }

    // ============================================
    // Prefiltered Array: 128x128, 7 mips, 8 cubes
    // ============================================
    {
        RHI::TextureDesc desc = RHI::TextureDesc::CubemapArray(
            PREFILTERED_SIZE,
            MAX_PROBES,
            RHI::ETextureFormat::R16G16B16A16_FLOAT,
            PREFILTERED_MIP_COUNT
        );
        desc.debugName = "ReflectionProbeManager_PrefilteredArray";

        m_prefilteredArray.reset(renderContext->CreateTexture(desc));
        if (!m_prefilteredArray) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create prefiltered array");
            return false;
        }
    }

    CFFLog::Info("[ReflectionProbeManager] Created cube arrays (irr=%dx%d, pref=%dx%d, %d probes)",
                IRRADIANCE_SIZE, IRRADIANCE_SIZE,
                PREFILTERED_SIZE, PREFILTERED_SIZE,
                MAX_PROBES);
    return true;
}

bool CReflectionProbeManager::createConstantBuffer()
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return false;

    RHI::BufferDesc desc;
    desc.size = sizeof(CB_Probes);
    desc.usage = RHI::EBufferUsage::Constant;
    desc.cpuAccess = RHI::ECPUAccess::Write;  // Dynamic buffer for Map/Unmap
    desc.debugName = "ReflectionProbeManager_CB_Probes";

    m_cbProbes.reset(renderContext->CreateBuffer(desc));
    if (!m_cbProbes) {
        CFFLog::Error("[ReflectionProbeManager] Failed to create CB_Probes");
        return false;
    }

    return true;
}

bool CReflectionProbeManager::copyCubemapToArray(
    RHI::ITexture* srcCubemap,
    RHI::ITexture* dstArray,
    int sliceIndex,
    int expectedSize,
    int mipCount)
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();
    if (!cmdList || !srcCubemap || !dstArray) return false;

    // Verify source texture size
    if (srcCubemap->GetWidth() != (uint32_t)expectedSize ||
        srcCubemap->GetHeight() != (uint32_t)expectedSize) {
        CFFLog::Warning("[ReflectionProbeManager] Source cubemap size mismatch: expected %d, got %d",
                       expectedSize, srcCubemap->GetWidth());
        // Continue trying to copy
    }

    // Copy each face of each mip level
    int srcMipCount = (int)srcCubemap->GetMipLevels();
    for (int face = 0; face < 6; face++) {
        for (int mip = 0; mip < mipCount && mip < srcMipCount; mip++) {
            // Source: cubemap face (arraySlice = face, mipLevel = mip)
            uint32_t srcArraySlice = face;
            uint32_t srcMipLevel = mip;

            // Destination: cubemap array (arraySlice = sliceIndex * 6 + face, mipLevel = mip)
            uint32_t dstArraySlice = sliceIndex * 6 + face;
            uint32_t dstMipLevel = mip;

            cmdList->CopyTextureSubresource(
                dstArray, dstArraySlice, dstMipLevel,
                srcCubemap, srcArraySlice, srcMipLevel
            );
        }
    }

    return true;
}

bool CReflectionProbeManager::loadAndCopyToArray(
    const std::string& ktx2Path,
    int sliceIndex,
    bool isIrradiance)
{
    // Load KTX2 Cubemap using RHI
    std::unique_ptr<RHI::ITexture> rhiTexture(CKTXLoader::LoadCubemapFromKTX2(ktx2Path));
    if (!rhiTexture) {
        CFFLog::Error("[ReflectionProbeManager] Failed to load: %s", ktx2Path.c_str());
        return false;
    }

    // Copy to Array
    bool success = false;
    if (isIrradiance) {
        success = copyCubemapToArray(rhiTexture.get(), m_irradianceArray.get(), sliceIndex, IRRADIANCE_SIZE, 1);
    } else {
        success = copyCubemapToArray(rhiTexture.get(), m_prefilteredArray.get(), sliceIndex, PREFILTERED_SIZE, PREFILTERED_MIP_COUNT);
    }

    // rhiTexture unique_ptr will automatically release the texture
    return success;
}

void CReflectionProbeManager::fillSliceWithSolidColor(int sliceIndex, float r, float g, float b)
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = renderContext->GetCommandList();
    if (!renderContext || !cmdList) return;

    // Convert float to half (using DirectX PackedVector)
    uint16_t hr16 = XMConvertFloatToHalf(r);
    uint16_t hg16 = XMConvertFloatToHalf(g);
    uint16_t hb16 = XMConvertFloatToHalf(b);
    uint16_t ha16 = XMConvertFloatToHalf(1.0f);

    // ============================================
    // Fill Irradiance Array (32x32, 1 mip)
    // ============================================
    {
        // Create staging texture for writing
        RHI::TextureDesc stagingDesc = RHI::TextureDesc::StagingCubemap(
            IRRADIANCE_SIZE,
            RHI::ETextureFormat::R16G16B16A16_FLOAT,
            RHI::ECPUAccess::Write
        );
        stagingDesc.debugName = "ReflectionProbeManager_IrrStagingTemp";

        std::unique_ptr<RHI::ITexture> stagingTex(renderContext->CreateTexture(stagingDesc));
        if (!stagingTex) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create irradiance staging texture");
            return;
        }

        // Fill each face
        for (int face = 0; face < 6; face++) {
            RHI::MappedTexture mapped = stagingTex->Map(face, 0);
            if (mapped.pData) {
                // R16G16B16A16_FLOAT: 8 bytes per pixel
                for (int y = 0; y < IRRADIANCE_SIZE; y++) {
                    uint16_t* row = reinterpret_cast<uint16_t*>(
                        reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.rowPitch);
                    for (int x = 0; x < IRRADIANCE_SIZE; x++) {
                        row[x * 4 + 0] = hr16;
                        row[x * 4 + 1] = hg16;
                        row[x * 4 + 2] = hb16;
                        row[x * 4 + 3] = ha16;
                    }
                }
                stagingTex->Unmap(face, 0);
            }
        }

        // Copy staging texture to target array slice
        for (int face = 0; face < 6; face++) {
            uint32_t srcArraySlice = face;
            uint32_t dstArraySlice = sliceIndex * 6 + face;
            cmdList->CopyTextureSubresource(
                m_irradianceArray.get(), dstArraySlice, 0,
                stagingTex.get(), srcArraySlice, 0
            );
        }
    }

    // ============================================
    // Fill Prefiltered Array (128x128, 7 mips)
    // ============================================
    for (int mip = 0; mip < PREFILTERED_MIP_COUNT; mip++) {
        int mipSize = PREFILTERED_SIZE >> mip;
        if (mipSize < 1) mipSize = 1;

        // Create staging texture for this mip level
        RHI::TextureDesc stagingDesc;
        stagingDesc.width = mipSize;
        stagingDesc.height = mipSize;
        stagingDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
        stagingDesc.mipLevels = 1;
        stagingDesc.arraySize = 6;  // 6 faces
        stagingDesc.usage = RHI::ETextureUsage::Staging;
        stagingDesc.cpuAccess = RHI::ECPUAccess::Write;
        stagingDesc.debugName = "ReflectionProbeManager_PrefStagingTemp";

        std::unique_ptr<RHI::ITexture> stagingTex(renderContext->CreateTexture(stagingDesc));
        if (!stagingTex) continue;

        // Fill each face
        for (int face = 0; face < 6; face++) {
            RHI::MappedTexture mapped = stagingTex->Map(face, 0);
            if (mapped.pData) {
                for (int y = 0; y < mipSize; y++) {
                    uint16_t* row = reinterpret_cast<uint16_t*>(
                        reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.rowPitch);
                    for (int x = 0; x < mipSize; x++) {
                        row[x * 4 + 0] = hr16;
                        row[x * 4 + 1] = hg16;
                        row[x * 4 + 2] = hb16;
                        row[x * 4 + 3] = ha16;
                    }
                }
                stagingTex->Unmap(face, 0);
            }
        }

        // Copy staging texture to target array slice
        for (int face = 0; face < 6; face++) {
            uint32_t srcArraySlice = face;
            uint32_t dstArraySlice = sliceIndex * 6 + face;
            cmdList->CopyTextureSubresource(
                m_prefilteredArray.get(), dstArraySlice, mip,
                stagingTex.get(), srcArraySlice, 0
            );
        }
    }

    CFFLog::Info("[ReflectionProbeManager] Filled slice %d with solid color (%.2f, %.2f, %.2f)",
                sliceIndex, r, g, b);
}

void CReflectionProbeManager::updateConstantBuffer()
{
    if (!m_cbProbes) return;

    void* mapped = m_cbProbes->Map();
    if (mapped) {
        memcpy(mapped, &m_probeData, sizeof(CB_Probes));
        m_cbProbes->Unmap();
    }
}
