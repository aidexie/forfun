// RHI/PerDrawSlots.h
#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace PerDrawSlots {

//==============================================
// Push Constants (space3)
// Size limit: 128 bytes (DX12 root constants)
//==============================================
namespace Push {
    constexpr uint32_t PerDraw = 0;
}

// Must fit in 128 bytes (32 DWORDs)
struct CB_PerDraw {
    DirectX::XMFLOAT4X4 World;             // 64 bytes
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 64 bytes
    // Total: 128 bytes
};

} // namespace PerDrawSlots
