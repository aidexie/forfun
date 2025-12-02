#include "ReflectionProbeManager.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/ReflectionProbe.h"
#include "Core/ReflectionProbeAsset.h"
#include "Core/Loader/KTXLoader.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <DirectXPackedVector.h>
#include <filesystem>

using namespace DirectX;
using namespace DirectX::PackedVector;
using Microsoft::WRL::ComPtr;

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
    auto* context = CDX11Context::Instance().GetContext();
    context->UpdateSubresource(m_cbProbes.Get(), 0, nullptr, &m_probeData, 0, 0);

    m_initialized = true;
    CFFLog::Info("[ReflectionProbeManager] Initialized (max %d probes, default fallback IBL set)", MAX_PROBES);
    return true;
}

void CReflectionProbeManager::Shutdown()
{
    m_irradianceArray.Reset();
    m_prefilteredArray.Reset();
    m_irradianceArraySRV.Reset();
    m_prefilteredArraySRV.Reset();
    m_brdfLutSRV.Reset();
    m_cbProbes.Reset();
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

    auto* context = CDX11Context::Instance().GetContext();
    context->UpdateSubresource(m_cbProbes.Get(), 0, nullptr, &m_probeData, 0, 0);

    CFFLog::Info("[ReflectionProbeManager] Total probes loaded: %d", m_probeCount);
}

void CReflectionProbeManager::Bind(ID3D11DeviceContext* context)
{
    if (!m_initialized) return;

    // t3: IrradianceArray
    // t4: PrefilteredArray
    ID3D11ShaderResourceView* srvs[2] = {
        m_irradianceArraySRV.Get(),
        m_prefilteredArraySRV.Get()
    };
    context->PSSetShaderResources(3, 2, srvs);

    // b4: CB_Probes
    context->PSSetConstantBuffers(4, 1, m_cbProbes.GetAddressOf());
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
    auto* context = CDX11Context::Instance().GetContext();
    context->UpdateSubresource(m_cbProbes.Get(), 0, nullptr, &m_probeData, 0, 0);

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
    m_brdfLutSRV.Reset();
    m_brdfLutSRV = CKTXLoader::Load2DTextureSRVFromKTX2(brdfLutPath);

    if (!m_brdfLutSRV) {
        CFFLog::Error("[ReflectionProbeManager] Failed to load BRDF LUT: %s", brdfLutPath.c_str());
        return false;
    }

    CFFLog::Info("[ReflectionProbeManager] Loaded BRDF LUT: %s", brdfLutPath.c_str());
    return true;
}

// ============================================
// Internal Methods
// ============================================

bool CReflectionProbeManager::createCubeArrays()
{
    auto* device = CDX11Context::Instance().GetDevice();
    if (!device) return false;

    // ============================================
    // Irradiance Array: 32x32, 1 mip, 8 slices
    // ============================================
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = IRRADIANCE_SIZE;
        desc.Height = IRRADIANCE_SIZE;
        desc.MipLevels = 1;
        desc.ArraySize = MAX_PROBES * 6;  // 8 probes * 6 faces
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_irradianceArray);
        if (FAILED(hr)) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create irradiance array");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        srvDesc.TextureCubeArray.MipLevels = 1;
        srvDesc.TextureCubeArray.MostDetailedMip = 0;
        srvDesc.TextureCubeArray.First2DArrayFace = 0;
        srvDesc.TextureCubeArray.NumCubes = MAX_PROBES;

        hr = device->CreateShaderResourceView(m_irradianceArray.Get(), &srvDesc, &m_irradianceArraySRV);
        if (FAILED(hr)) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create irradiance array SRV");
            return false;
        }
    }

    // ============================================
    // Prefiltered Array: 128x128, 7 mips, 8 slices
    // ============================================
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = PREFILTERED_SIZE;
        desc.Height = PREFILTERED_SIZE;
        desc.MipLevels = PREFILTERED_MIP_COUNT;
        desc.ArraySize = MAX_PROBES * 6;  // 8 probes * 6 faces
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_prefilteredArray);
        if (FAILED(hr)) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create prefiltered array");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        srvDesc.TextureCubeArray.MipLevels = PREFILTERED_MIP_COUNT;
        srvDesc.TextureCubeArray.MostDetailedMip = 0;
        srvDesc.TextureCubeArray.First2DArrayFace = 0;
        srvDesc.TextureCubeArray.NumCubes = MAX_PROBES;

        hr = device->CreateShaderResourceView(m_prefilteredArray.Get(), &srvDesc, &m_prefilteredArraySRV);
        if (FAILED(hr)) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create prefiltered array SRV");
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
    auto* device = CDX11Context::Instance().GetDevice();
    if (!device) return false;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(CB_Probes);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    HRESULT hr = device->CreateBuffer(&desc, nullptr, &m_cbProbes);
    if (FAILED(hr)) {
        CFFLog::Error("[ReflectionProbeManager] Failed to create CB_Probes");
        return false;
    }

    return true;
}

bool CReflectionProbeManager::copyCubemapToArray(
    ID3D11Texture2D* srcCubemap,
    ID3D11Texture2D* dstArray,
    int sliceIndex,
    int expectedSize,
    int mipCount)
{
    auto* context = CDX11Context::Instance().GetContext();
    if (!context || !srcCubemap || !dstArray) return false;

    // 验证源纹理大小
    D3D11_TEXTURE2D_DESC srcDesc;
    srcCubemap->GetDesc(&srcDesc);

    if (srcDesc.Width != expectedSize || srcDesc.Height != expectedSize) {
        CFFLog::Warning("[ReflectionProbeManager] Source cubemap size mismatch: expected %d, got %d",
                       expectedSize, srcDesc.Width);
        // 继续尝试拷贝（可能需要 resize，但这里简单处理）
    }

    // 拷贝每个 face 的每个 mip
    for (int face = 0; face < 6; face++) {
        for (int mip = 0; mip < mipCount && mip < (int)srcDesc.MipLevels; mip++) {
            UINT srcSubresource = D3D11CalcSubresource(mip, face, srcDesc.MipLevels);
            UINT dstSubresource = D3D11CalcSubresource(mip, sliceIndex * 6 + face, mipCount);

            context->CopySubresourceRegion(
                dstArray, dstSubresource, 0, 0, 0,
                srcCubemap, srcSubresource, nullptr
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
    // 加载 KTX2 Cubemap
    ID3D11Texture2D* cubemap = CKTXLoader::LoadCubemapFromKTX2(ktx2Path);
    if (!cubemap) {
        CFFLog::Error("[ReflectionProbeManager] Failed to load: %s", ktx2Path.c_str());
        return false;
    }

    // 拷贝到 Array
    bool success = false;
    if (isIrradiance) {
        success = copyCubemapToArray(cubemap, m_irradianceArray.Get(), sliceIndex, IRRADIANCE_SIZE, 1);
    } else {
        success = copyCubemapToArray(cubemap, m_prefilteredArray.Get(), sliceIndex, PREFILTERED_SIZE, PREFILTERED_MIP_COUNT);
    }

    cubemap->Release();
    return success;
}

void CReflectionProbeManager::fillSliceWithSolidColor(int sliceIndex, float r, float g, float b)
{
    auto* device = CDX11Context::Instance().GetDevice();
    auto* context = CDX11Context::Instance().GetContext();
    if (!device || !context) return;

    // ============================================
    // 填充 Irradiance Array (32x32, 1 mip)
    // ============================================
    {
        // 创建临时 staging texture
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = IRRADIANCE_SIZE;
        desc.Height = IRRADIANCE_SIZE;
        desc.MipLevels = 1;
        desc.ArraySize = 6;  // 6 faces
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;  // Staging 不需要 TEXTURECUBE flag

        ComPtr<ID3D11Texture2D> stagingTex;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &stagingTex);
        if (FAILED(hr)) {
            CFFLog::Error("[ReflectionProbeManager] Failed to create irradiance staging texture");
            return;
        }

        // 填充每个 face
        for (int face = 0; face < 6; face++) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = context->Map(stagingTex.Get(), face, D3D11_MAP_WRITE, 0, &mapped);
            if (SUCCEEDED(hr)) {
                // R16G16B16A16_FLOAT: 每像素 8 bytes (4 * 2 bytes)
                uint16_t* pixels = reinterpret_cast<uint16_t*>(mapped.pData);

                // 转换 float 到 half (简化版，使用 DirectX 的转换)
                uint16_t hr16 = XMConvertFloatToHalf(r);
                uint16_t hg16 = XMConvertFloatToHalf(g);
                uint16_t hb16 = XMConvertFloatToHalf(b);
                uint16_t ha16 = XMConvertFloatToHalf(1.0f);

                for (int y = 0; y < IRRADIANCE_SIZE; y++) {
                    uint16_t* row = reinterpret_cast<uint16_t*>(
                        reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                    for (int x = 0; x < IRRADIANCE_SIZE; x++) {
                        row[x * 4 + 0] = hr16;
                        row[x * 4 + 1] = hg16;
                        row[x * 4 + 2] = hb16;
                        row[x * 4 + 3] = ha16;
                    }
                }
                context->Unmap(stagingTex.Get(), face);
            }
        }

        // 拷贝到目标 array
        for (int face = 0; face < 6; face++) {
            UINT srcSubresource = D3D11CalcSubresource(0, face, 1);
            UINT dstSubresource = D3D11CalcSubresource(0, sliceIndex * 6 + face, 1);
            context->CopySubresourceRegion(
                m_irradianceArray.Get(), dstSubresource, 0, 0, 0,
                stagingTex.Get(), srcSubresource, nullptr);
        }
    }

    // ============================================
    // 填充 Prefiltered Array (128x128, 7 mips)
    // ============================================
    {
        for (int mip = 0; mip < PREFILTERED_MIP_COUNT; mip++) {
            int mipSize = PREFILTERED_SIZE >> mip;
            if (mipSize < 1) mipSize = 1;

            // 创建临时 staging texture (每个 mip 单独创建)
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = mipSize;
            desc.Height = mipSize;
            desc.MipLevels = 1;
            desc.ArraySize = 6;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;

            ComPtr<ID3D11Texture2D> stagingTex;
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &stagingTex);
            if (FAILED(hr)) continue;

            uint16_t hr16 = XMConvertFloatToHalf(r);
            uint16_t hg16 = XMConvertFloatToHalf(g);
            uint16_t hb16 = XMConvertFloatToHalf(b);
            uint16_t ha16 = XMConvertFloatToHalf(1.0f);

            for (int face = 0; face < 6; face++) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                hr = context->Map(stagingTex.Get(), face, D3D11_MAP_WRITE, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    for (int y = 0; y < mipSize; y++) {
                        uint16_t* row = reinterpret_cast<uint16_t*>(
                            reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                        for (int x = 0; x < mipSize; x++) {
                            row[x * 4 + 0] = hr16;
                            row[x * 4 + 1] = hg16;
                            row[x * 4 + 2] = hb16;
                            row[x * 4 + 3] = ha16;
                        }
                    }
                    context->Unmap(stagingTex.Get(), face);
                }
            }

            // 拷贝到目标 array
            for (int face = 0; face < 6; face++) {
                UINT srcSubresource = D3D11CalcSubresource(0, face, 1);
                UINT dstSubresource = D3D11CalcSubresource(mip, sliceIndex * 6 + face, PREFILTERED_MIP_COUNT);
                context->CopySubresourceRegion(
                    m_prefilteredArray.Get(), dstSubresource, 0, 0, 0,
                    stagingTex.Get(), srcSubresource, nullptr);
            }
        }
    }

    CFFLog::Info("[ReflectionProbeManager] Filled slice %d with solid color (%.2f, %.2f, %.2f)",
                sliceIndex, r, g, b);
}
