// RHI/PerDrawSlots.h
// Per-draw constant buffer for descriptor set path (space3)
#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace PerDrawSlots {

//==============================================
// Constant Buffers (b-registers, space3)
//==============================================
namespace CB {
    constexpr uint32_t PerDraw = 0;
}

//==============================================
// CB_PerDraw - Per-object data for geometry passes
// Uses VolatileCBV for efficient per-draw updates
//==============================================
struct alignas(16) CB_PerDraw {
    DirectX::XMFLOAT4X4 World;       // 64 bytes - Current frame world matrix
    DirectX::XMFLOAT4X4 WorldPrev;   // 64 bytes - Previous frame world matrix (for velocity)
    int lightmapIndex;               // 4 bytes  - Index into lightmap info buffer (-1 = no lightmap)
    int objectID;                    // 4 bytes  - Object ID for picking/debug
    float _pad[2];                   // 8 bytes  - Padding to 16-byte alignment
    // Total: 144 bytes
};

} // namespace PerDrawSlots
