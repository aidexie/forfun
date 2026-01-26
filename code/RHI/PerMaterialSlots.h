// RHI/PerMaterialSlots.h
#pragma once
#include <cstdint>

namespace PerMaterialSlots {

//==============================================
// Constant Buffers (b-registers, space2)
//==============================================
namespace CB {
    constexpr uint32_t Material = 0;
}

//==============================================
// Textures (t-registers, space2)
// Range: t0-t7 (8 slots for PBR + extras)
//==============================================
namespace Tex {
    constexpr uint32_t Albedo            = 0;
    constexpr uint32_t Normal            = 1;
    constexpr uint32_t MetallicRoughness = 2;
    constexpr uint32_t Emissive          = 3;
    constexpr uint32_t AO                = 4;
    constexpr uint32_t Height            = 5;  // Future: parallax
    constexpr uint32_t DetailNormal      = 6;  // Future: detail
    constexpr uint32_t Mask              = 7;  // Future: custom mask
}

//==============================================
// Samplers (s-registers, space2)
//==============================================
namespace Samp {
    constexpr uint32_t Material = 0;
}

} // namespace PerMaterialSlots
