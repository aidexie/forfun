// Engine/Rendering/ComputePassLayout.h
// Unified descriptor set layout for all compute passes
//
// This provides a shared PerPass layout (Set 1, space1) that all compute passes
// can use. Each pass binds only what it needs - unused slots get null descriptors.
//
// Benefits:
// - Single root signature for all compute passes
// - Simplified PSO management
// - Consistent binding model across SSAO, HiZ, SSR, TAA, Bloom, etc.
//
#pragma once
#include "RHI/IDescriptorSet.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IRenderContext.h"

namespace ComputePassLayout {

//==============================================
// Slot Constants for Compute PerPass (space1)
//==============================================
namespace Slots {
    // Constant Buffer (b0, space1)
    constexpr uint32_t CB_PerPass = 0;

    // Texture SRVs (t0-t7, space1)
    constexpr uint32_t Tex_Input0 = 0;   // Primary input (e.g., depth)
    constexpr uint32_t Tex_Input1 = 1;   // Secondary input (e.g., normal)
    constexpr uint32_t Tex_Input2 = 2;   // Tertiary input (e.g., noise/history)
    constexpr uint32_t Tex_Input3 = 3;   // Additional input
    constexpr uint32_t Tex_Input4 = 4;   // Additional input
    constexpr uint32_t Tex_Input5 = 5;   // Additional input
    constexpr uint32_t Tex_Input6 = 6;   // Additional input
    constexpr uint32_t Tex_Input7 = 7;   // Additional input

    // UAVs (u0-u3, space1)
    constexpr uint32_t UAV_Output0 = 0;  // Primary output
    constexpr uint32_t UAV_Output1 = 1;  // Secondary output
    constexpr uint32_t UAV_Output2 = 2;  // Tertiary output
    constexpr uint32_t UAV_Output3 = 3;  // Additional output

    // Samplers (s0-s3, space1)
    constexpr uint32_t Samp_Point = 0;   // Point clamp sampler
    constexpr uint32_t Samp_Linear = 1;  // Linear clamp sampler
    constexpr uint32_t Samp_Aniso = 2;   // Anisotropic sampler
    constexpr uint32_t Samp_Extra = 3;   // Extra sampler slot
}

//==============================================
// Layout Creation
//==============================================

// Maximum CB size for compute passes (covers most use cases)
constexpr uint32_t MAX_COMPUTE_CB_SIZE = 512;

// Create the unified compute PerPass layout
// This layout supports:
// - 1 volatile CBV (up to 512 bytes)
// - 8 texture SRVs
// - 4 texture UAVs
// - 4 samplers
inline RHI::IDescriptorSetLayout* CreateComputePerPassLayout(RHI::IRenderContext* ctx) {
    using namespace RHI;

    BindingLayoutDesc desc("Compute_PerPass");

    // Constant buffer (b0, space1)
    desc.AddItem(BindingLayoutItem::VolatileCBV(Slots::CB_PerPass, MAX_COMPUTE_CB_SIZE));

    // Texture SRVs (t0-t7, space1)
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input0));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input1));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input2));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input3));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input4));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input5));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input6));
    desc.AddItem(BindingLayoutItem::Texture_SRV(Slots::Tex_Input7));

    // UAVs (u0-u3, space1)
    desc.AddItem(BindingLayoutItem::Texture_UAV(Slots::UAV_Output0));
    desc.AddItem(BindingLayoutItem::Texture_UAV(Slots::UAV_Output1));
    desc.AddItem(BindingLayoutItem::Texture_UAV(Slots::UAV_Output2));
    desc.AddItem(BindingLayoutItem::Texture_UAV(Slots::UAV_Output3));

    // Samplers (s0-s3, space1)
    desc.AddItem(BindingLayoutItem::Sampler(Slots::Samp_Point));
    desc.AddItem(BindingLayoutItem::Sampler(Slots::Samp_Linear));
    desc.AddItem(BindingLayoutItem::Sampler(Slots::Samp_Aniso));
    desc.AddItem(BindingLayoutItem::Sampler(Slots::Samp_Extra));

    return ctx->CreateDescriptorSetLayout(desc);
}

} // namespace ComputePassLayout
