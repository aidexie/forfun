#pragma once
#include <cstdint>
#include <cstddef>

// ============================================
// SMAALookupTextures - Pre-computed SMAA Lookup Tables
// ============================================
// Contains embedded texture data for SMAA blending weight calculation.
// These textures are generated from the SMAA reference implementation.
//
// Reference:
//   "SMAA: Enhanced Subpixel Morphological Antialiasing"
//   Jorge Jimenez et al. (2012)
//   https://github.com/iryoku/smaa
//
// Textures:
//   AreaTex:   160x560 RG8 (~179KB) - Pre-computed area coverage
//   SearchTex: 64x16 R8 (~1KB) - Pre-computed search distances
// ============================================

namespace SMAALookupTextures {

// ============================================
// Area Texture
// ============================================
// Format: RG8 (2 bytes per pixel)
// Size: 160 x 560 pixels
// Total: 179,200 bytes
//
// Contains pre-computed area coverage values for different
// edge patterns. Used in the blending weight calculation pass.
constexpr uint32_t AREATEX_WIDTH = 160;
constexpr uint32_t AREATEX_HEIGHT = 560;
constexpr size_t AREATEX_SIZE = AREATEX_WIDTH * AREATEX_HEIGHT * 2;  // RG8

extern const uint8_t AreaTexData[AREATEX_SIZE];

// ============================================
// Search Texture
// ============================================
// Format: R8 (1 byte per pixel)
// Size: 64 x 16 pixels
// Total: 1,024 bytes
//
// Contains pre-computed search distances for edge endpoint
// detection. Used to accelerate the edge search in the
// blending weight calculation pass.
constexpr uint32_t SEARCHTEX_WIDTH = 64;
constexpr uint32_t SEARCHTEX_HEIGHT = 16;
constexpr size_t SEARCHTEX_SIZE = SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT;  // R8

extern const uint8_t SearchTexData[SEARCHTEX_SIZE];

// ============================================
// Runtime Generation Functions
// ============================================
// Since the lookup textures require computation, these functions
// generate the data on first access and cache it.
const uint8_t* GetAreaTexData();
const uint8_t* GetSearchTexData();

} // namespace SMAALookupTextures
