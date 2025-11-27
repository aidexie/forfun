#include "ClusteredLightingPass.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/PointLight.h"
#include <d3dcompiler.h>
#include <cmath>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

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

void CClusteredLightingPass::Initialize(ID3D11Device* device) {
    CFFLog::Info("[ClusteredLightingPass] Initializing...");
    CreateBuffers(device);
    CreateShaders(device);
    CreateDebugShaders(device);
    CFFLog::Info("[ClusteredLightingPass] Initialized successfully");
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
    auto device = CDX11Context::Instance().GetDevice();
    CreateBuffers(device);
    m_clusterGridDirty = true;  // Force rebuild after resize
}

void CClusteredLightingPass::CreateBuffers(ID3D11Device* device) {
    HRESULT hr;

    // Cluster AABB Buffer (SClusterAABB[totalClusters])
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SClusterAABB) * m_totalClusters;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(SClusterAABB);

        hr = device->CreateBuffer(&desc, nullptr, m_clusterAABBBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster AABB buffer");
            return;
        }

        // UAV for compute shader write
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = m_totalClusters;
        hr = device->CreateUnorderedAccessView(m_clusterAABBBuffer.Get(), &uavDesc, m_clusterAABBUAV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster AABB UAV");
        }

        // SRV for compute shader read (CullLights)
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = m_totalClusters;
        hr = device->CreateShaderResourceView(m_clusterAABBBuffer.Get(), &srvDesc, m_clusterAABBSRV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster AABB SRV");
        }
    }

    // Cluster Data Buffer (SClusterData[totalClusters])
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SClusterData) * m_totalClusters;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(SClusterData);

        hr = device->CreateBuffer(&desc, nullptr, m_clusterDataBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster data buffer");
            return;
        }

        // SRV for pixel shader read
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = m_totalClusters;
        hr = device->CreateShaderResourceView(m_clusterDataBuffer.Get(), &srvDesc, m_clusterDataSRV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster data SRV");
        }

        // UAV for compute shader write
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = m_totalClusters;
        hr = device->CreateUnorderedAccessView(m_clusterDataBuffer.Get(), &uavDesc, m_clusterDataUAV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster data UAV");
        }
    }

    // Compact Light List Buffer (uint[MAX_TOTAL_LIGHT_REFS])
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(uint32_t) * ClusteredConfig::MAX_TOTAL_LIGHT_REFS;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(uint32_t);

        hr = device->CreateBuffer(&desc, nullptr, m_compactLightListBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create compact light list buffer");
            return;
        }

        // SRV for pixel shader read
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = ClusteredConfig::MAX_TOTAL_LIGHT_REFS;
        hr = device->CreateShaderResourceView(m_compactLightListBuffer.Get(), &srvDesc, m_compactLightListSRV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create compact light list SRV");
        }

        // UAV for compute shader write
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = ClusteredConfig::MAX_TOTAL_LIGHT_REFS;
        hr = device->CreateUnorderedAccessView(m_compactLightListBuffer.Get(), &uavDesc, m_compactLightListUAV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create compact light list UAV");
        }
    }

    // Point Light Buffer (SGpuPointLight[max 1024 lights for now])
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SGpuPointLight) * 1024;  // Support up to 1024 point lights
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(SGpuPointLight);

        hr = device->CreateBuffer(&desc, nullptr, m_pointLightBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create point light buffer");
            return;
        }

        // SRV for shaders
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = 1024;
        hr = device->CreateShaderResourceView(m_pointLightBuffer.Get(), &srvDesc, m_pointLightSRV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create point light SRV");
        }
    }

    // Global Counter Buffer (single uint for atomic operations)
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(uint32_t);

        hr = device->CreateBuffer(&desc, nullptr, m_globalCounterBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create global counter buffer");
            return;
        }

        // UAV for atomic operations
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 1;
        hr = device->CreateUnorderedAccessView(m_globalCounterBuffer.Get(), &uavDesc, m_globalCounterUAV.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create global counter UAV");
        }
    }

    // Constant Buffer
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(SClusterCB);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = device->CreateBuffer(&desc, nullptr, m_clusterCB.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            CFFLog::Error("[ClusteredLightingPass] Failed to create cluster constant buffer");
        }
    }
}

void CClusteredLightingPass::CreateShaders(ID3D11Device* device) {
    HRESULT hr;

    // Compile and create compute shaders
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> csBlob;
    ComPtr<ID3DBlob> errorBlob;

    // Build Cluster Grid Compute Shader
    hr = D3DCompileFromFile(
        L"E:/forfun/source/code/Shader/ClusteredLighting.compute.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSBuildClusterGrid", "cs_5_0",
        compileFlags, 0,
        csBlob.ReleaseAndGetAddressOf(),
        errorBlob.ReleaseAndGetAddressOf()
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            CFFLog::Error("[ClusteredLightingPass] Shader compilation error (BuildClusterGrid): {}",
                          (char*)errorBlob->GetBufferPointer());
        }
        return;
    }

    hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
                                     nullptr, m_buildClusterGridCS.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[ClusteredLightingPass] Failed to create BuildClusterGrid CS");
    }

    // Cull Lights Compute Shader
    hr = D3DCompileFromFile(
        L"E:/forfun/source/code/Shader/ClusteredLighting.compute.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSCullLights", "cs_5_0",
        compileFlags, 0,
        csBlob.ReleaseAndGetAddressOf(),
        errorBlob.ReleaseAndGetAddressOf()
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            CFFLog::Error("[ClusteredLightingPass] Shader compilation error (CullLights): {}",
                          (char*)errorBlob->GetBufferPointer());
        }
        return;
    }

    hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
                                     nullptr, m_cullLightsCS.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("[ClusteredLightingPass] Failed to create CullLights CS");
    }
}

void CClusteredLightingPass::CreateDebugShaders(ID3D11Device* device) {
    // TODO: Implement debug visualization shaders
    // Will be implemented after basic functionality works
}

void CClusteredLightingPass::BuildClusterGrid(ID3D11DeviceContext* context,
                                               const XMMATRIX& projection,
                                               float nearZ, float farZ) {
    if (!m_buildClusterGridCS) return;

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

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(m_clusterCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        SClusterCB* cb = (SClusterCB*)mapped.pData;

        XMMATRIX invProj = XMMatrixInverse(nullptr, projection);
        XMStoreFloat4x4(&cb->inverseProjection, XMMatrixTranspose(invProj));
        cb->nearZ = nearZ;
        cb->farZ = farZ;
        cb->numClustersX = m_numClustersX;
        cb->numClustersY = m_numClustersY;
        cb->numClustersZ = ClusteredConfig::DEPTH_SLICES;
        cb->screenWidth = m_screenWidth;
        cb->screenHeight = m_screenHeight;

        context->Unmap(m_clusterCB.Get(), 0);
    }

    // Bind resources
    context->CSSetConstantBuffers(0, 1, m_clusterCB.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, m_clusterAABBUAV.GetAddressOf(), nullptr);
    context->CSSetShader(m_buildClusterGridCS.Get(), nullptr, 0);

    // Dispatch (one thread per cluster)
    uint32_t groupsX = (m_numClustersX + 7) / 8;
    uint32_t groupsY = (m_numClustersY + 7) / 8;
    uint32_t groupsZ = ClusteredConfig::DEPTH_SLICES;  // numthreads Z is 1
    context->Dispatch(groupsX, groupsY, groupsZ);

    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
}

void CClusteredLightingPass::CullLights(ID3D11DeviceContext* context,
                                        CScene* scene,
                                        const XMMATRIX& view) {
    if (!m_cullLightsCS || !scene) return;

    // Gather point lights from scene
    std::vector<SGpuPointLight> gpuLights;
    auto& world = scene->GetWorld();
    for (auto& go : world.Objects()) {
        auto* light = go->GetComponent<SPointLight>();
        auto* transform = go->GetComponent<STransform>();
        if (light && transform) {
            SGpuPointLight gpuLight;
            gpuLight.position = transform->position;
            gpuLight.range = light->range;
            gpuLight.color = light->color;
            gpuLight.intensity = light->intensity;
            gpuLights.push_back(gpuLight);
        }
    }

    if (gpuLights.empty()) {
        // No lights, clear cluster data
        return;
    }

    // DEBUG: Log light count and first light details (commented out to avoid per-frame spam)
    // CFFLog::Info("[ClusteredLighting] Collected %d point lights for culling", (int)gpuLights.size());
    // if (!gpuLights.empty()) {
    //     auto& light0 = gpuLights[0];
    //     CFFLog::Info("[ClusteredLighting] Light[0]: pos(%.2f, %.2f, %.2f) range=%.2f intensity=%.2f",
    //         light0.position.x, light0.position.y, light0.position.z, light0.range, light0.intensity);
    // }

    // Upload point lights to GPU
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(m_pointLightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, gpuLights.data(), sizeof(SGpuPointLight) * gpuLights.size());
        context->Unmap(m_pointLightBuffer.Get(), 0);
    }

    // Reset global counter to 0
    uint32_t zero = 0;
    context->UpdateSubresource(m_globalCounterBuffer.Get(), 0, nullptr, &zero, 0, 0);

    // Update constant buffer for light culling
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(SLightCullingCB);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> lightCullingCB;
    hr = CDX11Context::Instance().GetDevice()->CreateBuffer(&cbDesc, nullptr, lightCullingCB.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = context->Map(lightCullingCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            SLightCullingCB* cb = (SLightCullingCB*)mapped.pData;
            XMStoreFloat4x4(&cb->view, XMMatrixTranspose(view));
            cb->numLights = (uint32_t)gpuLights.size();
            cb->numClustersX = m_numClustersX;
            cb->numClustersY = m_numClustersY;
            cb->numClustersZ = ClusteredConfig::DEPTH_SLICES;
            context->Unmap(lightCullingCB.Get(), 0);
        }
    }

    // Bind resources
    context->CSSetConstantBuffers(0, 1, lightCullingCB.GetAddressOf());
    ID3D11ShaderResourceView* cullSrvs[] = { m_pointLightSRV.Get(), m_clusterAABBSRV.Get() };
    context->CSSetShaderResources(0, 2, cullSrvs);

    ID3D11UnorderedAccessView* uavs[] = {
        m_clusterDataUAV.Get(),          // u0
        m_compactLightListUAV.Get(),     // u1
        m_globalCounterUAV.Get()         // u2
    };
    context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    context->CSSetShader(m_cullLightsCS.Get(), nullptr, 0);

    // Dispatch (one thread per cluster)
    uint32_t groupsX = (m_numClustersX + 7) / 8;
    uint32_t groupsY = (m_numClustersY + 7) / 8;
    uint32_t groupsZ = ClusteredConfig::DEPTH_SLICES;  // numthreads Z is 1
    // CFFLog::Info("[ClusteredLighting] Dispatching CullLights: groups(%d, %d, %d) clusters(%d, %d, %d)",
    //     groupsX, groupsY, groupsZ, m_numClustersX, m_numClustersY, ClusteredConfig::DEPTH_SLICES);
    context->Dispatch(groupsX, groupsY, groupsZ);

    // Unbind resources
    ID3D11UnorderedAccessView* nullUAVs[3] = {nullptr};
    context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr};
    context->CSSetShaderResources(0, 2, nullSRVs);
}

void CClusteredLightingPass::BindToMainPass(ID3D11DeviceContext* context) {
    // Bind cluster data to pixel shader slots t10, t11, t12
    ID3D11ShaderResourceView* srvs[] = {
        m_clusterDataSRV.Get(),        // t10
        m_compactLightListSRV.Get(),   // t11
        m_pointLightSRV.Get()          // t12
    };
    context->PSSetShaderResources(10, 3, srvs);
}

void CClusteredLightingPass::RenderDebug(ID3D11DeviceContext* context) {
    // TODO: Implement debug visualization
    // Will be implemented after basic functionality works
}

