// Engine/Rendering/PassLayouts.h
// Factory functions for creating per-pass descriptor set layouts.
// Uses slot 0 for all bindings within the pass's space (space1).
// NOTE: Future migration to PerPassSlots constants requires updating
// both shaders and root signatures simultaneously.
#pragma once
#include "RHI/IRenderContext.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerPassSlots.h"

namespace PassLayouts {

//==============================================
// FXAA Pass Layout
// Uses: b0 (CB), t0 (input texture), s0 (sampler) in space1
//==============================================
inline RHI::IDescriptorSetLayout* CreateFXAALayout(
    RHI::IRenderContext* ctx,
    uint32_t cbSize)
{
    using namespace RHI;

    return ctx->CreateDescriptorSetLayout(
        BindingLayoutDesc("FXAA")
            .AddItem(BindingLayoutItem::VolatileCBV(0, cbSize))
            .AddItem(BindingLayoutItem::Texture_SRV(0))
            .AddItem(BindingLayoutItem::Sampler(0))
    );
}

//==============================================
// Bloom Pass Layout (placeholder for future)
//==============================================
inline RHI::IDescriptorSetLayout* CreateBloomLayout(
    RHI::IRenderContext* ctx,
    uint32_t cbSize)
{
    using namespace RHI;

    return ctx->CreateDescriptorSetLayout(
        BindingLayoutDesc("Bloom")
            .AddItem(BindingLayoutItem::VolatileCBV(0, cbSize))
            .AddItem(BindingLayoutItem::Texture_SRV(0))
            .AddItem(BindingLayoutItem::Sampler(0))
    );
}

} // namespace PassLayouts
