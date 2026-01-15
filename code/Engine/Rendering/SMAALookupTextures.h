#pragma once
#include <cstdint>
#include <cstddef>

// SMAA Lookup Textures - Pre-computed lookup tables for SMAA blending weight calculation.
// Reference: "SMAA: Enhanced Subpixel Morphological Antialiasing" (Jimenez et al., 2012)
// https://github.com/iryoku/smaa

namespace SMAALookupTextures {

// Area Texture: 160x560 RG8 (~179KB) - Pre-computed area coverage
constexpr uint32_t AREATEX_WIDTH = 160;
constexpr uint32_t AREATEX_HEIGHT = 560;
constexpr size_t AREATEX_SIZE = AREATEX_WIDTH * AREATEX_HEIGHT * 2;

// Search Texture: 64x16 R8 (~1KB) - Pre-computed search distances
constexpr uint32_t SEARCHTEX_WIDTH = 64;
constexpr uint32_t SEARCHTEX_HEIGHT = 16;
constexpr size_t SEARCHTEX_SIZE = SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT;

// Get texture data (generated on first access)
const uint8_t* GetAreaTexData();
const uint8_t* GetSearchTexData();

} // namespace SMAALookupTextures
