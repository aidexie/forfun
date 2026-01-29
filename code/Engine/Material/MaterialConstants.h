// Engine/Material/MaterialConstants.h
// Constant buffer structures for material data in descriptor set path
#pragma once
#include <DirectXMath.h>
#include <cstdint>

namespace MaterialConstants {

//==============================================
// CB_Material - Per-material constant buffer (space2, b0)
// Used by GBufferPass descriptor set path
//==============================================
struct alignas(16) CB_Material {
    DirectX::XMFLOAT3 albedo;              // Base color multiplier
    float metallic;                         // 16 bytes

    DirectX::XMFLOAT3 emissive;            // Emissive color
    float roughness;                        // 16 bytes

    float emissiveStrength;                 // Emissive intensity multiplier
    int hasMetallicRoughnessTexture;        // 1 if texture bound, 0 otherwise
    int hasEmissiveMap;                     // 1 if emissive texture bound
    int alphaMode;                          // 0=Opaque, 1=Mask, 2=Blend (16 bytes)

    float alphaCutoff;                      // Alpha test threshold (for Mask mode)
    float materialID;                       // Material type ID (for deferred lighting)
    float _pad[2];                          // 16 bytes

    // Total: 64 bytes
};

} // namespace MaterialConstants
