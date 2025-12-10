#include "LightProbeManager.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/LightProbe.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "Core/FFLog.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

// ============================================
// Public Interface
// ============================================

bool CLightProbeManager::Initialize()
{
    if (m_initialized) return true;

    if (!createStructuredBuffer()) {
        CFFLog::Error("[LightProbeManager] Failed to create structured buffer");
        return false;
    }

    if (!createConstantBuffer()) {
        CFFLog::Error("[LightProbeManager] Failed to create constant buffer");
        return false;
    }

    // 设置默认参数
    m_params.probeCount = 0;
    m_params.blendFalloff = 2.0f;  // 距离权重衰减指数

    m_initialized = true;
    CFFLog::Info("[LightProbeManager] Initialized (max %d probes)", MAX_PROBES);
    return true;
}

void CLightProbeManager::Shutdown()
{
    m_probeBuffer.reset();
    m_cbParams.reset();
    m_probeData.clear();
    m_probeCount = 0;
    m_initialized = false;
}

void CLightProbeManager::LoadProbesFromScene(CScene& scene)
{
    if (!m_initialized) {
        CFFLog::Error("[LightProbeManager] Not initialized");
        return;
    }

    // 清空现有数据
    m_probeData.clear();
    m_probeCount = 0;

    // 遍历场景中所有 LightProbe 组件
    for (auto& objPtr : scene.GetWorld().Objects())
    {
        if (m_probeCount >= MAX_PROBES) {
            CFFLog::Warning("[LightProbeManager] Max probe count reached (%d)", MAX_PROBES);
            break;
        }

        auto* probeComp = objPtr->GetComponent<SLightProbe>();
        if (!probeComp) continue;

        auto* transform = objPtr->GetComponent<STransform>();
        if (!transform) continue;

        // 构建 GPU 数据
        LightProbeData gpuData{};
        gpuData.position = transform->position;
        gpuData.radius = probeComp->radius;

        // 拷贝 SH 系数（9 bands × RGB）
        for (int band = 0; band < 9; band++) {
            gpuData.shCoeffs[band] = probeComp->shCoeffs[band];
        }

        m_probeData.push_back(gpuData);
        m_probeCount++;

        CFFLog::Info("[LightProbeManager] Loaded probe '%s' at index %d (pos=%.1f,%.1f,%.1f r=%.1f)",
                    objPtr->GetName().c_str(), m_probeCount - 1,
                    transform->position.x, transform->position.y, transform->position.z,
                    probeComp->radius);
    }

    // 更新 GPU 数据
    if (m_probeCount > 0) {
        updateProbeBuffer();
    }

    m_params.probeCount = m_probeCount;
    updateConstantBuffer();

    CFFLog::Info("[LightProbeManager] Total light probes loaded: %d", m_probeCount);
}

void CLightProbeManager::Bind(RHI::ICommandList* cmdList)
{
    if (!m_initialized || !cmdList) return;

    // t15: LightProbeBuffer (StructuredBuffer)
    cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Pixel, 15, m_probeBuffer.get());

    // b5: CB_LightProbeParams
    cmdList->SetConstantBuffer(RHI::EShaderStage::Pixel, 5, m_cbParams.get());
}

void CLightProbeManager::BlendProbesForPosition(
    const XMFLOAT3& worldPos,
    XMFLOAT3 outShCoeffs[9]
) const
{
    // 初始化输出为 0
    for (int i = 0; i < 9; i++) {
        outShCoeffs[i] = XMFLOAT3(0, 0, 0);
    }

    if (m_probeCount == 0) {
        return;  // 没有 probe，返回全黑
    }

    // ============================================
    // 1. 收集附近的 probe（在 radius 范围内）
    // ============================================
    struct ProbeDistance {
        int index;
        float distance;
    };
    std::vector<ProbeDistance> nearby;

    for (int i = 0; i < m_probeCount; i++) {
        const auto& probe = m_probeData[i];
        float dx = worldPos.x - probe.position.x;
        float dy = worldPos.y - probe.position.y;
        float dz = worldPos.z - probe.position.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (dist < probe.radius) {
            nearby.push_back({ i, dist });
        }
    }

    // ============================================
    // 2. Fallback：没有 probe 覆盖 → 返回全黑（或全局 IBL）
    // ============================================
    if (nearby.empty()) {
        return;  // 全黑，Shader 会 fallback 到全局 IBL
    }

    // ============================================
    // 3. 按距离排序，取最近的 MAX_BLEND_PROBES 个
    // ============================================
    std::sort(nearby.begin(), nearby.end(), [](const ProbeDistance& a, const ProbeDistance& b) {
        return a.distance < b.distance;
    });

    int blendCount = std::min((int)nearby.size(), MAX_BLEND_PROBES);

    // ============================================
    // 4. 距离权重混合
    // ============================================
    float totalWeight = 0.0f;
    for (int i = 0; i < blendCount; i++) {
        float dist = nearby[i].distance;
        float weight = 1.0f / std::pow(dist + 0.1f, m_params.blendFalloff);  // 逆平方衰减
        totalWeight += weight;

        const auto& probe = m_probeData[nearby[i].index];
        for (int band = 0; band < 9; band++) {
            XMVECTOR v1 = XMLoadFloat3(&outShCoeffs[band]);
            XMVECTOR v2 = XMLoadFloat3(&probe.shCoeffs[band]);
            XMVECTOR result = XMVectorAdd(v1, XMVectorScale(v2, weight));
            XMStoreFloat3(&outShCoeffs[band], result);
        }
    }

    // 归一化
    if (totalWeight > 0.0f) {
        float invWeight = 1.0f / totalWeight;
        for (int band = 0; band < 9; band++) {
            XMVECTOR v = XMLoadFloat3(&outShCoeffs[band]);
            XMStoreFloat3(&outShCoeffs[band], XMVectorScale(v, invWeight));
        }
    }
}

// ============================================
// Internal Methods
// ============================================

bool CLightProbeManager::createStructuredBuffer()
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return false;

    // 创建 StructuredBuffer（最大容量）
    RHI::BufferDesc desc;
    desc.size = sizeof(LightProbeData) * MAX_PROBES;
    desc.usage = RHI::EBufferUsage::Structured | RHI::EBufferUsage::UnorderedAccess;  // For SRV
    desc.cpuAccess = RHI::ECPUAccess::Write;
    desc.structureByteStride = sizeof(LightProbeData);
    desc.debugName = "LightProbeManager_ProbeBuffer";

    m_probeBuffer.reset(renderContext->CreateBuffer(desc));
    if (!m_probeBuffer) {
        CFFLog::Error("[LightProbeManager] Failed to create probe buffer");
        return false;
    }

    return true;
}

bool CLightProbeManager::createConstantBuffer()
{
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return false;

    RHI::BufferDesc desc;
    desc.size = sizeof(CB_LightProbeParams);
    desc.usage = RHI::EBufferUsage::Constant;
    desc.cpuAccess = RHI::ECPUAccess::Write;
    desc.debugName = "LightProbeManager_CB_Params";

    m_cbParams.reset(renderContext->CreateBuffer(desc));
    if (!m_cbParams) {
        CFFLog::Error("[LightProbeManager] Failed to create constant buffer");
        return false;
    }

    return true;
}

void CLightProbeManager::updateProbeBuffer()
{
    if (m_probeData.empty() || !m_probeBuffer) return;

    void* mapped = m_probeBuffer->Map();
    if (mapped) {
        memcpy(mapped, m_probeData.data(), sizeof(LightProbeData) * m_probeCount);
        m_probeBuffer->Unmap();
    }
}

void CLightProbeManager::updateConstantBuffer()
{
    if (!m_cbParams) return;

    void* mapped = m_cbParams->Map();
    if (mapped) {
        memcpy(mapped, &m_params, sizeof(CB_LightProbeParams));
        m_cbParams->Unmap();
    }
}
