#include "DX12GenerateMipsPass.h"
#include "DX12CommandList.h"
#include "DX12Context.h"
#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "DX12Common.h"
#include "../RHIManager.h"
#include "../ShaderCompiler.h"
#include "../../Core/FFLog.h"
#include "../../Core/PathManager.h"
#include <algorithm>
#include <vector>

namespace RHI {
namespace DX12 {

CDX12GenerateMipsPass::~CDX12GenerateMipsPass() {
    Shutdown();
}

bool CDX12GenerateMipsPass::Initialize() {
    if (m_initialized) {
        return true;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[GenerateMipsPass] No render context");
        return false;
    }

#ifdef _DEBUG
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile the GenerateMips compute shader for array/cubemap textures
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/GenerateMips.cs.hlsl";
    SCompiledShader compiled = CompileShaderFromFile(shaderPath, "main", "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[GenerateMipsPass] Array shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    // Create shader for array/cubemap
    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    m_csArray.reset(ctx->CreateShader(shaderDesc));
    if (!m_csArray) {
        CFFLog::Error("[GenerateMipsPass] Failed to create array shader");
        return false;
    }

    // Create compute PSO for array/cubemap
    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = m_csArray.get();
    m_psoArray.reset(ctx->CreateComputePipelineState(psoDesc));
    if (!m_psoArray) {
        CFFLog::Error("[GenerateMipsPass] Failed to create array PSO");
        return false;
    }

    // Compile the GenerateMips compute shader for 2D textures
    std::string shader2DPath = FFPath::GetSourceDir() + "/Shader/GenerateMips2D.cs.hlsl";
    SCompiledShader compiled2D = CompileShaderFromFile(shader2DPath, "main", "cs_5_0", nullptr, debugShaders);
    if (!compiled2D.success) {
        CFFLog::Error("[GenerateMipsPass] 2D shader compilation failed: %s", compiled2D.errorMessage.c_str());
        return false;
    }

    // Create shader for 2D textures
    ShaderDesc shader2DDesc;
    shader2DDesc.type = EShaderType::Compute;
    shader2DDesc.bytecode = compiled2D.bytecode.data();
    shader2DDesc.bytecodeSize = compiled2D.bytecode.size();
    m_cs2D.reset(ctx->CreateShader(shader2DDesc));
    if (!m_cs2D) {
        CFFLog::Error("[GenerateMipsPass] Failed to create 2D shader");
        return false;
    }

    // Create compute PSO for 2D textures
    ComputePipelineDesc pso2DDesc;
    pso2DDesc.computeShader = m_cs2D.get();
    m_pso2D.reset(ctx->CreateComputePipelineState(pso2DDesc));
    if (!m_pso2D) {
        CFFLog::Error("[GenerateMipsPass] Failed to create 2D PSO");
        return false;
    }

    // Create linear sampler for bilinear filtering
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Clamp;
    samplerDesc.addressV = ETextureAddressMode::Clamp;
    samplerDesc.addressW = ETextureAddressMode::Clamp;
    m_sampler.reset(ctx->CreateSampler(samplerDesc));
    if (!m_sampler) {
        CFFLog::Error("[GenerateMipsPass] Failed to create sampler");
        return false;
    }

    m_initialized = true;
    CFFLog::Info("[GenerateMipsPass] Initialized");
    return true;
}

void CDX12GenerateMipsPass::Shutdown() {
    m_sampler.reset();
    m_pso2D.reset();
    m_psoArray.reset();
    m_cs2D.reset();
    m_csArray.reset();
    m_initialized = false;
}

void CDX12GenerateMipsPass::Execute(CDX12CommandList* cmdList, ITexture* texture) {
    if (!texture) {
        CFFLog::Warning("[GenerateMipsPass] Execute: null texture");
        return;
    }

    // Lazy initialization
    if (!m_initialized && !Initialize()) {
        CFFLog::Error("[GenerateMipsPass] Execute: failed to initialize");
        return;
    }

    CDX12Texture* dx12Texture = static_cast<CDX12Texture*>(texture);
    TextureDesc desc = dx12Texture->GetDesc();  // Copy, not const ref, so we can modify

    // Handle mipLevels == 0 (auto-calculate)
    if (desc.mipLevels == 0) {
        uint32_t maxDim = std::max(desc.width, desc.height);
        desc.mipLevels = 1;
        while (maxDim > 1) {
            maxDim >>= 1;
            desc.mipLevels++;
        }
        CFFLog::Warning("[GenerateMipsPass] mipLevels was 0, calculated %u", desc.mipLevels);
    }

    // Validate texture can have mips generated
    if (desc.mipLevels <= 1) {
        return;  // Nothing to generate
    }

    // Check if UAV flag is set (required for compute shader write)
    if (!(desc.usage & ETextureUsage::UnorderedAccess)) {
        CFFLog::Warning("[GenerateMipsPass] texture lacks UnorderedAccess flag");
        return;
    }

    // Determine if this is a 2D texture or array/cubemap
    bool is2D = (desc.dimension == ETextureDimension::Tex2D) && (desc.arraySize == 1);

    // Get array size for iteration
    uint32_t arraySize = 1;
    if (!is2D) {
        if (desc.dimension == ETextureDimension::TexCube) {
            arraySize = 6;
        } else if (desc.dimension == ETextureDimension::TexCubeArray) {
            arraySize = desc.arraySize;
        } else {
            arraySize = desc.arraySize;
        }
    }

    // Determine if source is SRGB (for correct gamma handling)
    bool isSRGB = (desc.srvFormat == ETextureFormat::R8G8B8A8_UNORM_SRGB ||
                   desc.srvFormat == ETextureFormat::B8G8R8A8_UNORM_SRGB ||
                   desc.format == ETextureFormat::R8G8B8A8_UNORM_SRGB ||
                   desc.format == ETextureFormat::B8G8R8A8_UNORM_SRGB);

    // Select PSO based on texture dimension
    IPipelineState* pso = is2D ? m_pso2D.get() : m_psoArray.get();
    cmdList->SetPipelineState(pso);
    cmdList->SetSampler(EShaderStage::Compute, 0, m_sampler.get());

    // Get the D3D12 resource for per-subresource barriers
    ID3D12Resource* d3dResource = dx12Texture->GetD3D12Resource();
    ID3D12GraphicsCommandList* d3dCmdList = cmdList->GetD3D12CommandList();

    // Create a UNORM SRV for GenerateMips (no automatic sRGB->linear conversion)
    SDescriptorHandle unormSrvHandle = {};
    {
        unormSrvHandle = CDX12DescriptorHeapManager::Instance().AllocateCBVSRVUAV();
        if (!unormSrvHandle.IsValid()) {
            CFFLog::Error("[GenerateMipsPass] Failed to allocate UNORM SRV descriptor");
            return;
        }

        // Determine UNORM format (strip SRGB suffix for explicit gamma handling)
        DXGI_FORMAT srvFormat = (desc.srvFormat != ETextureFormat::Unknown) ?
                                 ToDXGIFormat(desc.srvFormat) : ToDXGIFormat(desc.format);
        DXGI_FORMAT unormFormat = srvFormat;
        if (isSRGB) {
            switch (srvFormat) {
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: unormFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: unormFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
                default: break;
            }
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = unormFormat;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (is2D) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = desc.mipLevels;
            srvDesc.Texture2D.PlaneSlice = 0;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        } else {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = desc.mipLevels;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = arraySize;
            srvDesc.Texture2DArray.PlaneSlice = 0;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        }

        CDX12Context::Instance().GetDevice()->CreateShaderResourceView(
            d3dResource, &srvDesc, unormSrvHandle.cpuHandle);
    }

    // Get the current state of the texture
    D3D12_RESOURCE_STATES currentState = dx12Texture->GetCurrentState();

    // Generate each mip level
    for (uint32_t mip = 1; mip < desc.mipLevels; ++mip) {
        uint32_t srcWidth = std::max(1u, desc.width >> (mip - 1));
        uint32_t srcHeight = std::max(1u, desc.height >> (mip - 1));
        uint32_t dstWidth = std::max(1u, desc.width >> mip);
        uint32_t dstHeight = std::max(1u, desc.height >> mip);

        // Transition source mip to SRV state, dest mip to UAV state
        std::vector<D3D12_RESOURCE_BARRIER> barriers;

        for (uint32_t slice = 0; slice < arraySize; ++slice) {
            uint32_t srcSubresource = CalcSubresource(mip - 1, slice, 0, desc.mipLevels, arraySize);
            uint32_t dstSubresource = CalcSubresource(mip, slice, 0, desc.mipLevels, arraySize);

            D3D12_RESOURCE_STATES srcStateBefore = (mip == 1) ? currentState : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            D3D12_RESOURCE_BARRIER srcBarrier = {};
            srcBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            srcBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            srcBarrier.Transition.pResource = d3dResource;
            srcBarrier.Transition.Subresource = srcSubresource;
            srcBarrier.Transition.StateBefore = srcStateBefore;
            srcBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            if (srcBarrier.Transition.StateBefore != srcBarrier.Transition.StateAfter) {
                barriers.push_back(srcBarrier);
            }

            D3D12_RESOURCE_BARRIER dstBarrier = {};
            dstBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            dstBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            dstBarrier.Transition.pResource = d3dResource;
            dstBarrier.Transition.Subresource = dstSubresource;
            dstBarrier.Transition.StateBefore = currentState;
            dstBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers.push_back(dstBarrier);
        }

        if (!barriers.empty()) {
            d3dCmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        // For each array slice / cubemap face
        for (uint32_t slice = 0; slice < arraySize; ++slice) {
            // Setup constant buffer
            CB_GenerateMips cb;
            cb.srcMipSizeX = srcWidth;
            cb.srcMipSizeY = srcHeight;
            cb.dstMipSizeX = dstWidth;
            cb.dstMipSizeY = dstHeight;
            cb.srcMipLevel = mip - 1;
            cb.arraySlice = slice;
            cb.isSRGB = isSRGB ? 1 : 0;
            cb.padding = 0;

            cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

            // Bind source SRV (UNORM format - shader handles gamma)
            cmdList->SetPendingSRV(0, unormSrvHandle.cpuHandle);

            // Bind destination UAV
            SDescriptorHandle uavHandle = dx12Texture->GetOrCreateUAVSlice(mip);
            cmdList->SetPendingUAV(0, uavHandle.cpuHandle);

            // Dispatch compute shader (numthreads 8,8,1)
            uint32_t groupsX = (dstWidth + 7) / 8;
            uint32_t groupsY = (dstHeight + 7) / 8;
            cmdList->Dispatch(groupsX, groupsY, 1);
        }

        // UAV barrier between mip levels
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        uavBarrier.UAV.pResource = d3dResource;
        d3dCmdList->ResourceBarrier(1, &uavBarrier);
    }

    // Transition all mips to shader resource state
    {
        std::vector<D3D12_RESOURCE_BARRIER> finalBarriers;
        D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
            for (uint32_t slice = 0; slice < arraySize; ++slice) {
                uint32_t subresource = CalcSubresource(mip, slice, 0, desc.mipLevels, arraySize);

                D3D12_RESOURCE_STATES stateBefore;
                if (mip == desc.mipLevels - 1) {
                    stateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                } else {
                    stateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }

                if (stateBefore == targetState) continue;

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = d3dResource;
                barrier.Transition.Subresource = subresource;
                barrier.Transition.StateBefore = stateBefore;
                barrier.Transition.StateAfter = targetState;
                finalBarriers.push_back(barrier);
            }
        }
        if (!finalBarriers.empty()) {
            d3dCmdList->ResourceBarrier(static_cast<UINT>(finalBarriers.size()), finalBarriers.data());
        }
    }

    // Update tracked state
    dx12Texture->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Clear UAV bindings
    cmdList->SetPendingUAV(0, {});
}

} // namespace DX12
} // namespace RHI
