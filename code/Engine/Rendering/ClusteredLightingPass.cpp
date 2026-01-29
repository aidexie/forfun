#include "ClusteredLightingPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/PointLight.h"
#include "Engine/Components/SpotLight.h"
#include <cmath>

using namespace DirectX;
using namespace RHI;

// Constant buffer for cluster building
struct SClusterCB {
    XMFLOAT4X4 inverseProjection;
    float nearZ;
    float farZ;
    uint32_t numClustersX;
    uint32_t numClustersY;
    uint32_t numClustersZ;
    uint32_t screenWidth;
    uint32_t screenHeight;
    uint32_t padding;
};

// Constant buffer for light culling
struct SLightCullingCB {
    XMFLOAT4X4 view;
    uint32_t numLights;
    uint32_t numClustersX;
    uint32_t numClustersY;
    uint32_t numClustersZ;
};

CClusteredLightingPass::CClusteredLightingPass() = default;
CClusteredLightingPass::~CClusteredLightingPass() = default;

void CClusteredLightingPass::Initialize() {
    if (m_initialized) return;

    CFFLog::Info("[ClusteredLightingPass] Initializing...");
    // Note: Don't create buffers here - they need valid screen dimensions
    // CreateBuffers() will be called from Resize() when dimensions are known
    CreateShaders();
    CreateDebugShaders();
    m_initialized = true;
    CFFLog::Info("[ClusteredLightingPass] Initialized successfully");
}

void CClusteredLightingPass::Shutdown() {
    m_clusterAABBBuffer.reset();
    m_clusterDataBuffer.reset();
    m_compactLightListBuffer.reset();
    m_pointLightBuffer.reset();
    m_globalCounterBuffer.reset();
    m_buildClusterGridCS.reset();
    m_cullLightsCS.reset();
    m_debugVS.reset();
    m_debugHeatmapPS.reset();
    m_debugAABBPS.reset();
    m_initialized = false;
}

void CClusteredLightingPass::Resize(uint32_t width, uint32_t height) {
    // Check if size actually changed
    if (m_screenWidth == width && m_screenHeight == height) {
        return;  // No resize needed
    }

    m_screenWidth = width;
    m_screenHeight = height;

    // Calculate cluster grid dimensions (32×32 tiles)
    m_numClustersX = (width + ClusteredConfig::TILE_SIZE - 1) / ClusteredConfig::TILE_SIZE;
    m_numClustersY = (height + ClusteredConfig::TILE_SIZE - 1) / ClusteredConfig::TILE_SIZE;
    m_totalClusters = m_numClustersX * m_numClustersY * ClusteredConfig::DEPTH_SLICES;

    CFFLog::Info("[ClusteredLightingPass] Resized to %ux%u, Cluster Grid: %ux%ux%u = %u clusters",
                 width, height,
                 m_numClustersX, m_numClustersY, ClusteredConfig::DEPTH_SLICES,
                 m_totalClusters);

    // Recreate cluster AABB and data buffers with new size
    CreateBuffers();
    m_clusterGridDirty = true;  // Force rebuild after resize
}

void CClusteredLightingPass::CreateBuffers() {
    // Guard: Don't create zero-sized buffers
    if (m_totalClusters == 0) {
        CFFLog::Warning("[ClusteredLightingPass] CreateBuffers skipped - totalClusters is 0");
        return;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Cluster AABB Buffer (SClusterAABB[totalClusters]) - needs SRV + UAV
    {
        BufferDesc desc;
        desc.size = sizeof(SClusterAABB) * m_totalClusters;
        desc.usage = EBufferUsage::Structured | EBufferUsage::UnorderedAccess;
        desc.structureByteStride = sizeof(SClusterAABB);
        desc.debugName = "ClusterAABBBuffer";

        m_clusterAABBBuffer.reset(ctx->CreateBuffer(desc));
        if (!m_clusterAABBBuffer) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster AABB buffer");
            return;
        }
    }

    // Cluster Data Buffer (SClusterData[totalClusters]) - needs SRV + UAV
    {
        BufferDesc desc;
        desc.size = sizeof(SClusterData) * m_totalClusters;
        desc.usage = EBufferUsage::Structured | EBufferUsage::UnorderedAccess;
        desc.structureByteStride = sizeof(SClusterData);
        desc.debugName = "ClusterDataBuffer";

        m_clusterDataBuffer.reset(ctx->CreateBuffer(desc));
        if (!m_clusterDataBuffer) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster data buffer");
            return;
        }
    }

    // Compact Light List Buffer (uint[MAX_TOTAL_LIGHT_REFS]) - needs SRV + UAV
    {
        BufferDesc desc;
        desc.size = sizeof(uint32_t) * ClusteredConfig::MAX_TOTAL_LIGHT_REFS;
        desc.usage = EBufferUsage::Structured | EBufferUsage::UnorderedAccess;
        desc.structureByteStride = sizeof(uint32_t);
        desc.debugName = "CompactLightListBuffer";

        m_compactLightListBuffer.reset(ctx->CreateBuffer(desc));
        if (!m_compactLightListBuffer) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create compact light list buffer");
            return;
        }
    }

    // Light Buffer (SGpuLight[max 1024 lights]) - needs SRV only, CPU write
    {
        BufferDesc desc;
        desc.size = sizeof(SGpuLight) * 1024;
        desc.usage = EBufferUsage::Structured;
        desc.cpuAccess = ECPUAccess::Write;
        desc.structureByteStride = sizeof(SGpuLight);
        desc.debugName = "PointLightBuffer";

        m_pointLightBuffer.reset(ctx->CreateBuffer(desc));
        if (!m_pointLightBuffer) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create point light buffer");
            return;
        }
    }

    // Global Counter Buffer (single uint for atomic operations) - needs UAV only
    // Note: Use Raw buffer (not Structured) to allow ClearUnorderedAccessViewUint
    {
        BufferDesc desc;
        desc.size = sizeof(uint32_t);
        desc.usage = EBufferUsage::UnorderedAccess;  // Raw UAV buffer, not Structured
        desc.structureByteStride = 0;  // 0 = raw buffer (required for ClearUnorderedAccessViewUint)
        desc.debugName = "GlobalCounterBuffer";

        m_globalCounterBuffer.reset(ctx->CreateBuffer(desc));
        if (!m_globalCounterBuffer) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create global counter buffer");
            return;
        }
    }

}

void CClusteredLightingPass::CreateShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Build shader path
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/ClusteredLighting.compute.hlsl";

    // Build Cluster Grid Compute Shader
    SCompiledShader buildGridCompiled = CompileShaderFromFile(shaderPath, "CSBuildClusterGrid", "cs_5_0", nullptr, debugShaders);
    if (!buildGridCompiled.success) {
        CFFLog::Error("[ClusteredLightingPass] Shader compilation error (BuildClusterGrid): %s",
                      buildGridCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc buildGridDesc;
    buildGridDesc.type = EShaderType::Compute;
    buildGridDesc.bytecode = buildGridCompiled.bytecode.data();
    buildGridDesc.bytecodeSize = buildGridCompiled.bytecode.size();
    m_buildClusterGridCS.reset(ctx->CreateShader(buildGridDesc));

    // Cull Lights Compute Shader
    SCompiledShader cullLightsCompiled = CompileShaderFromFile(shaderPath, "CSCullLights", "cs_5_0", nullptr, debugShaders);
    if (!cullLightsCompiled.success) {
        CFFLog::Error("[ClusteredLightingPass] Shader compilation error (CullLights): %s",
                      cullLightsCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc cullLightsDesc;
    cullLightsDesc.type = EShaderType::Compute;
    cullLightsDesc.bytecode = cullLightsCompiled.bytecode.data();
    cullLightsDesc.bytecodeSize = cullLightsCompiled.bytecode.size();
    m_cullLightsCS.reset(ctx->CreateShader(cullLightsDesc));

    // Create compute pipeline states (cached to avoid per-frame creation overhead)
    ComputePipelineDesc buildGridPsoDesc;
    buildGridPsoDesc.computeShader = m_buildClusterGridCS.get();
    buildGridPsoDesc.debugName = "Clustered_BuildGrid_PSO";
    m_buildClusterGridPSO.reset(ctx->CreateComputePipelineState(buildGridPsoDesc));

    ComputePipelineDesc cullLightsPsoDesc;
    cullLightsPsoDesc.computeShader = m_cullLightsCS.get();
    cullLightsPsoDesc.debugName = "Clustered_CullLights_PSO";
    m_cullLightsPSO.reset(ctx->CreateComputePipelineState(cullLightsPsoDesc));

    CFFLog::Info("[ClusteredLightingPass] Compute shaders and PSOs created");
}

void CClusteredLightingPass::CreateDebugShaders() {
    // TODO: Implement debug visualization shaders
    // Will be implemented after basic functionality works
}

void CClusteredLightingPass::BuildClusterGrid(ICommandList* cmdList,
                                               const XMMATRIX& projection,
                                               float nearZ, float farZ) {
#ifdef FF_LEGACY_BINDING_DISABLED
    (void)cmdList;
    (void)projection;
    (void)nearZ;
    (void)farZ;
    static bool warned = false;
    if (!warned) {
        CFFLog::Warning("[ClusteredLightingPass] BuildClusterGrid uses legacy binding - not yet migrated to descriptor sets");
        warned = true;
    }
#else
    if (!m_buildClusterGridPSO || !cmdList) return;

    // Extract FovY from projection matrix for dirty checking
    // For perspective projection: tan(FovY/2) = 1 / m11
    XMFLOAT4X4 projF;
    XMStoreFloat4x4(&projF, projection);
    float fovY = 2.0f * atan(1.0f / projF._22);

    // Check if projection parameters changed
    const float epsilon = 0.001f;
    bool projChanged = (abs(fovY - m_cachedFovY) > epsilon) ||
                       (abs(nearZ - m_cachedNearZ) > epsilon) ||
                       (abs(farZ - m_cachedFarZ) > epsilon);

    if (!projChanged && !m_clusterGridDirty) {
        // Cluster grid is up-to-date, skip rebuild
        return;
    }

    // Cache new parameters
    m_cachedFovY = fovY;
    m_cachedNearZ = nearZ;
    m_cachedFarZ = farZ;
    m_clusterGridDirty = false;

    // Build constant buffer data
    SClusterCB cb{};
    XMMATRIX invProj = XMMatrixInverse(nullptr, projection);
    XMStoreFloat4x4(&cb.inverseProjection, XMMatrixTranspose(invProj));
    cb.nearZ = nearZ;
    cb.farZ = farZ;
    cb.numClustersX = m_numClustersX;
    cb.numClustersY = m_numClustersY;
    cb.numClustersZ = ClusteredConfig::DEPTH_SLICES;
    cb.screenWidth = m_screenWidth;
    cb.screenHeight = m_screenHeight;

    // Bind resources using cached PSO
    cmdList->SetPipelineState(m_buildClusterGridPSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(SClusterCB));
    cmdList->SetUnorderedAccess(0, m_clusterAABBBuffer.get());

    // Dispatch (one thread per cluster)
    uint32_t groupsX = (m_numClustersX + 7) / 8;
    uint32_t groupsY = (m_numClustersY + 7) / 8;
    uint32_t groupsZ = ClusteredConfig::DEPTH_SLICES;  // numthreads Z is 1
    cmdList->Dispatch(groupsX, groupsY, groupsZ);

    // Unbind UAVs
    cmdList->SetUnorderedAccess(0, nullptr);
#endif
}

void CClusteredLightingPass::CullLights(ICommandList* cmdList,
                                        CScene* scene,
                                        const XMMATRIX& view) {
#ifdef FF_LEGACY_BINDING_DISABLED
    (void)cmdList;
    (void)scene;
    (void)view;
    static bool warned = false;
    if (!warned) {
        CFFLog::Warning("[ClusteredLightingPass] CullLights uses legacy binding - not yet migrated to descriptor sets");
        warned = true;
    }
#else
    if (!m_cullLightsPSO || !scene || !cmdList) return;

    // Unbind cluster buffers from pixel shader before using them as UAVs
    // This prevents D3D11 resource hazard warnings
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 8, nullptr);
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 9, nullptr);

    // Gather all lights (Point + Spot) from scene
    std::vector<SGpuLight> gpuLights;
    auto& world = scene->GetWorld();
    for (auto& go : world.Objects()) {
        auto* transform = go->GetComponent<STransform>();
        if (!transform) continue;

        // Check for Point Light
        auto* pointLight = go->GetComponent<SPointLight>();
        if (pointLight) {
            SGpuLight gpuLight = {};  // Zero-initialize
            gpuLight.position = transform->position;
            gpuLight.range = pointLight->range;
            gpuLight.color = pointLight->color;
            gpuLight.intensity = pointLight->intensity;
            gpuLight.type = (uint32_t)ELightType::Point;
            // direction, innerConeAngle, outerConeAngle remain zero (unused for point lights)
            gpuLights.push_back(gpuLight);
        }

        // Check for Spot Light
        auto* spotLight = go->GetComponent<SSpotLight>();
        if (spotLight) {
            SGpuLight gpuLight = {};  // Zero-initialize
            gpuLight.position = transform->position;
            gpuLight.range = spotLight->range;
            gpuLight.color = spotLight->color;
            gpuLight.intensity = spotLight->intensity;
            gpuLight.type = (uint32_t)ELightType::Spot;

            // Transform local direction to world space
            XMVECTOR localDir = XMLoadFloat3(&spotLight->direction);
            localDir = XMVector3Normalize(localDir);  // Ensure normalized
            XMMATRIX rotation = transform->GetRotationMatrix();
            XMVECTOR worldDir = XMVector3TransformNormal(localDir, rotation);
            worldDir = XMVector3Normalize(worldDir);
            XMStoreFloat3(&gpuLight.direction, worldDir);

            // Precompute cos(angle) for shader (degrees to radians, then cos)
            float innerRadians = XMConvertToRadians(spotLight->innerConeAngle);
            float outerRadians = XMConvertToRadians(spotLight->outerConeAngle);
            gpuLight.innerConeAngle = cosf(innerRadians);
            gpuLight.outerConeAngle = cosf(outerRadians);

            gpuLights.push_back(gpuLight);
        }
    }

    if (gpuLights.empty()) {
        // No lights, clear cluster data
        return;
    }

    // DEBUG: Log light count and types (temp debug for spot light)
    static int logCounter = 0;
    if (logCounter++ < 3) {  // Only log first 3 frames
        int pointCount = 0, spotCount = 0;
        for (const auto& light : gpuLights) {
            if (light.type == (uint32_t)ELightType::Point) pointCount++;
            else if (light.type == (uint32_t)ELightType::Spot) spotCount++;
        }
        CFFLog::Info("[ClusteredLighting] Collected %d lights (Point: %d, Spot: %d)",
                     (int)gpuLights.size(), pointCount, spotCount);

        // Log first spot light details
        for (const auto& light : gpuLights) {
            if (light.type == (uint32_t)ELightType::Spot) {
                CFFLog::Info("[ClusteredLighting] Spot Light: pos(%.2f,%.2f,%.2f) dir(%.2f,%.2f,%.2f) range=%.2f intensity=%.2f cosInner=%.3f cosOuter=%.3f",
                    light.position.x, light.position.y, light.position.z,
                    light.direction.x, light.direction.y, light.direction.z,
                    light.range, light.intensity, light.innerConeAngle, light.outerConeAngle);
                break;  // Only log first one
            }
        }
    }

    // Upload all lights to GPU
    void* mapped = m_pointLightBuffer->Map();
    if (mapped) {
        memcpy(mapped, gpuLights.data(), sizeof(SGpuLight) * gpuLights.size());
        m_pointLightBuffer->Unmap();
    }

    // Reset global counter to 0 for atomic operations
    const uint32_t zeroValues[4] = {0, 0, 0, 0};
    cmdList->ClearUnorderedAccessViewUint(m_globalCounterBuffer.get(), zeroValues);

    // Build constant buffer data for light culling
    SLightCullingCB cb{};
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(view));
    cb.numLights = (uint32_t)gpuLights.size();
    cb.numClustersX = m_numClustersX;
    cb.numClustersY = m_numClustersY;
    cb.numClustersZ = ClusteredConfig::DEPTH_SLICES;

    // Bind resources using cached PSO
    cmdList->SetPipelineState(m_cullLightsPSO.get());
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(SLightCullingCB));
    cmdList->SetShaderResourceBuffer(EShaderStage::Compute, 0, m_pointLightBuffer.get());
    cmdList->SetShaderResourceBuffer(EShaderStage::Compute, 1, m_clusterAABBBuffer.get());
    cmdList->SetUnorderedAccess(0, m_clusterDataBuffer.get());
    cmdList->SetUnorderedAccess(1, m_compactLightListBuffer.get());
    cmdList->SetUnorderedAccess(2, m_globalCounterBuffer.get());

    // Dispatch (one thread per cluster)
    uint32_t groupsX = (m_numClustersX + 7) / 8;
    uint32_t groupsY = (m_numClustersY + 7) / 8;
    uint32_t groupsZ = ClusteredConfig::DEPTH_SLICES;  // numthreads Z is 1
    cmdList->Dispatch(groupsX, groupsY, groupsZ);

    // Unbind resources
    cmdList->SetUnorderedAccess(0, nullptr);
    cmdList->SetUnorderedAccess(1, nullptr);
    cmdList->SetUnorderedAccess(2, nullptr);
    cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 2);

    // Transition buffers from UAV to SRV for consumers (deferred lighting pass)
    cmdList->Barrier(m_clusterDataBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
    cmdList->Barrier(m_compactLightListBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
#endif
}

void CClusteredLightingPass::BindToMainPass(ICommandList* cmdList) {
#ifdef FF_LEGACY_BINDING_DISABLED
    (void)cmdList;
    static bool warned = false;
    if (!warned) {
        CFFLog::Warning("[ClusteredLightingPass] BindToMainPass uses legacy binding - not yet migrated to descriptor sets");
        warned = true;
    }
#else
    if (!cmdList) return;

    // Bind clustered params constant buffer (b3)
    CB_ClusteredParams clusteredParams{};
    clusteredParams.nearZ = m_cachedNearZ;
    clusteredParams.farZ = m_cachedFarZ;
    clusteredParams.numClustersX = m_numClustersX;
    clusteredParams.numClustersY = m_numClustersY;
    clusteredParams.numClustersZ = GetNumClustersZ();
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 3, &clusteredParams, sizeof(CB_ClusteredParams));

    // Bind cluster data to pixel shader slots t8, t9, t10 (contiguous after IBL at t5-t7)
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 8, m_clusterDataBuffer.get());
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 9, m_compactLightListBuffer.get());
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 10, m_pointLightBuffer.get());
#endif
}

void CClusteredLightingPass::RenderDebug(ICommandList* cmdList) {
    (void)cmdList;  // Unused for now
    // TODO: Implement debug visualization
    // Will be implemented after basic functionality works
}

void CClusteredLightingPass::PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) {
    using namespace PerFrameSlots;

    if (!perFrameSet) return;

    // Bind clustered lighting buffers to PerFrame set
    perFrameSet->Bind({
        BindingSetItem::Buffer_SRV(Tex::Clustered_LightGrid, m_clusterDataBuffer.get()),
        BindingSetItem::Buffer_SRV(Tex::Clustered_LightIndexList, m_compactLightListBuffer.get()),
        BindingSetItem::Buffer_SRV(Tex::Clustered_LightData, m_pointLightBuffer.get()),
    });
}

