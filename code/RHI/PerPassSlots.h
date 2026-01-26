// RHI/PerPassSlots.h
#pragma once
#include <cstdint>

namespace PerPassSlots {

//==============================================
// Constant Buffers (b-registers, space1)
//==============================================
namespace CB {
    constexpr uint32_t PerPass = 0;
}

//==============================================
// Textures (t-registers, space1)
// Range: t0-t15 (16 slots)
//==============================================
namespace Tex {
    // G-Buffer (t0-t5)
    constexpr uint32_t GBuffer_Albedo   = 0;
    constexpr uint32_t GBuffer_Normal   = 1;
    constexpr uint32_t GBuffer_WorldPos = 2;
    constexpr uint32_t GBuffer_Emissive = 3;
    constexpr uint32_t GBuffer_Velocity = 4;
    constexpr uint32_t GBuffer_Depth    = 5;

    // Post-process inputs (t6-t11)
    constexpr uint32_t SceneColor = 6;
    constexpr uint32_t SSAO       = 7;
    constexpr uint32_t SSR        = 8;
    constexpr uint32_t Bloom      = 9;
    constexpr uint32_t PrevFrame  = 10;
    constexpr uint32_t DepthHiZ   = 11;

    // t12-t15: Reserved
}

//==============================================
// Samplers (s-registers, space1)
//==============================================
namespace Samp {
    constexpr uint32_t PointClamp  = 0;
    constexpr uint32_t LinearClamp = 1;
}

//==============================================
// UAVs (u-registers, space1)
//==============================================
namespace UAV {
    constexpr uint32_t Output0 = 0;
    constexpr uint32_t Output1 = 1;
}

} // namespace PerPassSlots
