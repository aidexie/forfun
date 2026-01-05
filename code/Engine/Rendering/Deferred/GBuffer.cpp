#include "GBuffer.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "Core/FFLog.h"

using namespace RHI;

bool CGBuffer::Initialize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) {
        CFFLog::Error("CGBuffer::Initialize: Invalid dimensions (%u x %u)", width, height);
        return false;
    }

    createRenderTargets(width, height);

    CFFLog::Info("CGBuffer initialized (%u x %u)", width, height);
    return true;
}

void CGBuffer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    createRenderTargets(width, height);
    CFFLog::Info("CGBuffer resized to %u x %u", width, height);
}

void CGBuffer::Shutdown()
{
    for (int i = 0; i < RT_Count; ++i) {
        m_renderTargets[i].reset();
    }
    m_depth.reset();
    m_width = 0;
    m_height = 0;
}

RHI::ITexture* CGBuffer::GetRenderTarget(EGBufferRT index) const
{
    if (index >= RT_Count) return nullptr;
    return m_renderTargets[index].get();
}

void CGBuffer::GetRenderTargets(RHI::ITexture** outRTs, uint32_t& outCount) const
{
    outCount = RT_Count;
    for (uint32_t i = 0; i < RT_Count; ++i) {
        outRTs[i] = m_renderTargets[i].get();
    }
}

void CGBuffer::createRenderTargets(uint32_t width, uint32_t height)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    m_width = width;
    m_height = height;

    // ============================================
    // RT0: WorldPosition.xyz + Metallic (R16G16B16A16_FLOAT)
    // ============================================
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "GBuffer_WorldPosMetallic";
        m_renderTargets[RT_WorldPosMetallic].reset(ctx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // RT1: Normal.xyz + Roughness (R16G16B16A16_FLOAT)
    // ============================================
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "GBuffer_NormalRoughness";
        m_renderTargets[RT_NormalRoughness].reset(ctx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // RT2: Albedo.rgb + AO (R8G8B8A8_UNORM_SRGB)
    // ============================================
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R8G8B8A8_UNORM_SRGB);
        desc.debugName = "GBuffer_AlbedoAO";
        m_renderTargets[RT_AlbedoAO].reset(ctx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // RT3: Emissive.rgb + MaterialID (R16G16B16A16_FLOAT)
    // ============================================
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "GBuffer_EmissiveMaterialID";
        m_renderTargets[RT_EmissiveMaterialID].reset(ctx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // RT4: Velocity.xy (R16G16_FLOAT)
    // ============================================
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R16G16_FLOAT);
        desc.debugName = "GBuffer_Velocity";
        m_renderTargets[RT_Velocity].reset(ctx->CreateTexture(desc, nullptr));
    }

    // ============================================
    // Depth Buffer (D32_FLOAT with SRV access)
    // ============================================
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = ETextureFormat::R32_TYPELESS;
        desc.dimension = ETextureDimension::Tex2D;
        desc.usage = ETextureUsage::DepthStencil | ETextureUsage::ShaderResource;
        desc.dsvFormat = ETextureFormat::D32_FLOAT;
        desc.srvFormat = ETextureFormat::R32_FLOAT;  // DX12 requires typed format for SRV
        desc.debugName = "GBuffer_Depth";
        m_depth.reset(ctx->CreateTexture(desc, nullptr));
    }
}
