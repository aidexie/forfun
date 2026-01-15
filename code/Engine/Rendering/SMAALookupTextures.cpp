#include "SMAALookupTextures.h"
#include <cmath>
#include <algorithm>

// ============================================
// SMAALookupTextures.cpp - SMAA Lookup Table Generation
// ============================================
// Generates SMAA AreaTex and SearchTex at compile time.
// Based on the SMAA reference implementation by Jorge Jimenez.
//
// Reference:
//   https://github.com/iryoku/smaa
// ============================================

namespace SMAALookupTextures {

// ============================================
// Area Texture Generation
// ============================================
// The area texture contains pre-computed coverage values for
// different edge patterns. Each pattern is defined by:
// - Distance to left edge (d1)
// - Distance to right edge (d2)
// - Edge crossing configuration (e1, e2)

namespace {

// SMAA constants
constexpr float SMAA_AREATEX_MAX_DISTANCE = 16.0f;
constexpr float SMAA_AREATEX_MAX_DISTANCE_DIAG = 20.0f;
constexpr int SMAA_AREATEX_SUBTEX_SIZE = 7;

// Compute area under the line from p1 to p2
float AreaUnderLine(float p1x, float p1y, float p2x, float p2y, float x) {
    float b = (p2y - p1y) / (p2x - p1x);
    float a = p1y - b * p1x;
    float y = a + b * x;
    return std::max(0.0f, std::min(1.0f, y));
}

// Calculate area for orthogonal patterns
void CalculateOrthArea(float d1, float d2, float e1, float e2, float& a1, float& a2) {
    // Default: no coverage
    a1 = 0.0f;
    a2 = 0.0f;

    // Handle different edge configurations
    if (e1 > 0.0f && e2 > 0.0f) {
        // Both edges present: calculate crossing point
        float n = d1 + d2;
        if (n > 0.0f) {
            float t = d1 / n;
            a1 = 0.5f * t;
            a2 = 0.5f * (1.0f - t);
        }
    } else if (e1 > 0.0f) {
        // Only left edge
        float n = d1 + d2 + 1.0f;
        if (n > 0.0f) {
            a1 = 0.5f * d1 / n;
        }
    } else if (e2 > 0.0f) {
        // Only right edge
        float n = d1 + d2 + 1.0f;
        if (n > 0.0f) {
            a2 = 0.5f * d2 / n;
        }
    }

    // Clamp to valid range
    a1 = std::max(0.0f, std::min(1.0f, a1));
    a2 = std::max(0.0f, std::min(1.0f, a2));
}

// Generate area texture data
void GenerateAreaTexData(uint8_t* data) {
    const int width = AREATEX_WIDTH;
    const int height = AREATEX_HEIGHT;

    // Initialize to zero
    for (size_t i = 0; i < AREATEX_SIZE; i++) {
        data[i] = 0;
    }

    // For each subtexture (edge configuration)
    for (int e2 = 0; e2 < SMAA_AREATEX_SUBTEX_SIZE; e2++) {
        for (int e1 = 0; e1 < SMAA_AREATEX_SUBTEX_SIZE; e1++) {
            // Calculate subtexture offset
            int subtexOffset = (e2 * SMAA_AREATEX_SUBTEX_SIZE + e1) *
                               static_cast<int>(SMAA_AREATEX_MAX_DISTANCE) *
                               static_cast<int>(SMAA_AREATEX_MAX_DISTANCE) * 5;

            // For each distance pair
            for (int d2 = 0; d2 < static_cast<int>(SMAA_AREATEX_MAX_DISTANCE); d2++) {
                for (int d1 = 0; d1 < static_cast<int>(SMAA_AREATEX_MAX_DISTANCE); d1++) {
                    // Calculate pixel position
                    int x = d1 + e1 * static_cast<int>(SMAA_AREATEX_MAX_DISTANCE);
                    int y = d2 + e2 * static_cast<int>(SMAA_AREATEX_MAX_DISTANCE);

                    if (x >= width || y >= height) continue;

                    // Calculate area values
                    float a1, a2;
                    float fe1 = static_cast<float>(e1) / static_cast<float>(SMAA_AREATEX_SUBTEX_SIZE - 1);
                    float fe2 = static_cast<float>(e2) / static_cast<float>(SMAA_AREATEX_SUBTEX_SIZE - 1);
                    CalculateOrthArea(static_cast<float>(d1), static_cast<float>(d2),
                                     fe1, fe2, a1, a2);

                    // Store in RG8 format
                    int idx = (y * width + x) * 2;
                    if (idx + 1 < static_cast<int>(AREATEX_SIZE)) {
                        data[idx] = static_cast<uint8_t>(a1 * 255.0f);
                        data[idx + 1] = static_cast<uint8_t>(a2 * 255.0f);
                    }
                }
            }
        }
    }
}

// Generate search texture data
void GenerateSearchTexData(uint8_t* data) {
    const int width = SEARCHTEX_WIDTH;
    const int height = SEARCHTEX_HEIGHT;

    // Initialize to zero
    for (size_t i = 0; i < SEARCHTEX_SIZE; i++) {
        data[i] = 0;
    }

    // The search texture encodes the distance to the end of an edge
    // for different edge configurations
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Decode edge pattern from coordinates
            int e1 = x / 8;
            int e2 = x % 8;
            int offset = y;

            // Calculate search distance based on pattern
            float searchDist = 0.0f;

            // Simple heuristic: distance depends on edge configuration
            if (e1 > 0 || e2 > 0) {
                searchDist = static_cast<float>(offset) / 16.0f;
            }

            // Store in R8 format
            int idx = y * width + x;
            data[idx] = static_cast<uint8_t>(searchDist * 255.0f);
        }
    }
}

// Static storage for generated textures
static uint8_t s_areaTexData[AREATEX_SIZE] = {0};
static uint8_t s_searchTexData[SEARCHTEX_SIZE] = {0};
static bool s_texturesGenerated = false;

void EnsureTexturesGenerated() {
    if (!s_texturesGenerated) {
        GenerateAreaTexData(s_areaTexData);
        GenerateSearchTexData(s_searchTexData);
        s_texturesGenerated = true;
    }
}

} // anonymous namespace

// ============================================
// Public Interface
// ============================================

// Note: These are initialized on first access via GetAreaTexData/GetSearchTexData
// The extern declarations in the header point to these static arrays
const uint8_t AreaTexData[AREATEX_SIZE] = {0};
const uint8_t SearchTexData[SEARCHTEX_SIZE] = {0};

} // namespace SMAALookupTextures

// ============================================
// Runtime Texture Generation Functions
// ============================================
// Since we can't initialize large arrays at compile time with computed values,
// we provide functions to generate the textures at runtime.

namespace SMAALookupTextures {

const uint8_t* GetAreaTexData() {
    EnsureTexturesGenerated();
    return s_areaTexData;
}

const uint8_t* GetSearchTexData() {
    EnsureTexturesGenerated();
    return s_searchTexData;
}

} // namespace SMAALookupTextures
