#include "SMAALookupTextures.h"
#include <cmath>
#include <algorithm>
#include <mutex>

// SMAA Lookup Table Generation
// Based on the SMAA reference implementation by Jorge Jimenez.
// https://github.com/iryoku/smaa

namespace SMAALookupTextures {
namespace {

constexpr float SMAA_AREATEX_MAX_DISTANCE = 16.0f;
constexpr int SMAA_AREATEX_SUBTEX_SIZE = 7;

// Calculate area coverage for orthogonal edge patterns
void CalculateOrthArea(float d1, float d2, float e1, float e2, float& a1, float& a2) {
    a1 = 0.0f;
    a2 = 0.0f;

    if (e1 > 0.0f && e2 > 0.0f) {
        // Both edges present: calculate crossing point
        float n = d1 + d2;
        if (n > 0.0f) {
            float t = d1 / n;
            a1 = 0.5f * t;
            a2 = 0.5f * (1.0f - t);
        }
    } else if (e1 > 0.0f) {
        float n = d1 + d2 + 1.0f;
        if (n > 0.0f) {
            a1 = 0.5f * d1 / n;
        }
    } else if (e2 > 0.0f) {
        float n = d1 + d2 + 1.0f;
        if (n > 0.0f) {
            a2 = 0.5f * d2 / n;
        }
    }

    a1 = std::clamp(a1, 0.0f, 1.0f);
    a2 = std::clamp(a2, 0.0f, 1.0f);
}

void GenerateAreaTexData(uint8_t* data) {
    const int width = AREATEX_WIDTH;
    const int height = AREATEX_HEIGHT;

    std::fill(data, data + AREATEX_SIZE, uint8_t(0));

    for (int e2 = 0; e2 < SMAA_AREATEX_SUBTEX_SIZE; e2++) {
        for (int e1 = 0; e1 < SMAA_AREATEX_SUBTEX_SIZE; e1++) {
            for (int d2 = 0; d2 < static_cast<int>(SMAA_AREATEX_MAX_DISTANCE); d2++) {
                for (int d1 = 0; d1 < static_cast<int>(SMAA_AREATEX_MAX_DISTANCE); d1++) {
                    int x = d1 + e1 * static_cast<int>(SMAA_AREATEX_MAX_DISTANCE);
                    int y = d2 + e2 * static_cast<int>(SMAA_AREATEX_MAX_DISTANCE);

                    if (x >= width || y >= height) continue;

                    float a1, a2;
                    float fe1 = static_cast<float>(e1) / static_cast<float>(SMAA_AREATEX_SUBTEX_SIZE - 1);
                    float fe2 = static_cast<float>(e2) / static_cast<float>(SMAA_AREATEX_SUBTEX_SIZE - 1);
                    CalculateOrthArea(static_cast<float>(d1), static_cast<float>(d2), fe1, fe2, a1, a2);

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

void GenerateSearchTexData(uint8_t* data) {
    const int width = SEARCHTEX_WIDTH;
    const int height = SEARCHTEX_HEIGHT;

    std::fill(data, data + SEARCHTEX_SIZE, uint8_t(0));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int e1 = x / 8;
            int e2 = x % 8;

            float searchDist = 0.0f;
            if (e1 > 0 || e2 > 0) {
                searchDist = static_cast<float>(y) / 16.0f;
            }

            data[y * width + x] = static_cast<uint8_t>(searchDist * 255.0f);
        }
    }
}

static std::once_flag s_initFlag;
static uint8_t s_areaTexData[AREATEX_SIZE] = {0};
static uint8_t s_searchTexData[SEARCHTEX_SIZE] = {0};

void EnsureTexturesGenerated() {
    std::call_once(s_initFlag, []() {
        GenerateAreaTexData(s_areaTexData);
        GenerateSearchTexData(s_searchTexData);
    });
}

} // anonymous namespace

const uint8_t* GetAreaTexData() {
    EnsureTexturesGenerated();
    return s_areaTexData;
}

const uint8_t* GetSearchTexData() {
    EnsureTexturesGenerated();
    return s_searchTexData;
}

} // namespace SMAALookupTextures
