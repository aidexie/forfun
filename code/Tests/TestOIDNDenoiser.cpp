#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/Rendering/Lightmap/LightmapDenoiser.h"
#include <vector>
#include <cmath>
#include <fstream>
#include <filesystem>

// ============================================
// TestOIDNDenoiser
// ============================================
// Tests Intel OIDN denoiser functionality:
// 1. Initialize OIDN device
// 2. Create synthetic noisy HDR image
// 3. Run denoiser
// 4. Verify noise reduction
// 5. Save before/after images for visual inspection

// Helper functions
static float CalculateNoiseMSE(const std::vector<float>& noisy,
                               const std::vector<float>& clean,
                               int width, int height) {
    float mse = 0.0f;
    int count = width * height * 3;
    for (int i = 0; i < count; i++) {
        float diff = noisy[i] - clean[i];
        mse += diff * diff;
    }
    return mse / count;
}

static float CalculateImageDifference(const std::vector<float>& img1,
                                      const std::vector<float>& img2,
                                      int width, int height) {
    float diff = 0.0f;
    int count = width * height * 3;
    for (int i = 0; i < count; i++) {
        diff += std::abs(img1[i] - img2[i]);
    }
    return diff / count;
}

static float CalculateLocalVariance(const std::vector<float>& image, int width, int height) {
    // Calculate average local variance (3x3 neighborhood)
    float totalVariance = 0.0f;
    int count = 0;

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            float mean = 0.0f;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int idx = ((y + dy) * width + (x + dx)) * 3;
                    mean += (image[idx] + image[idx + 1] + image[idx + 2]) / 3.0f;
                }
            }
            mean /= 9.0f;

            float variance = 0.0f;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int idx = ((y + dy) * width + (x + dx)) * 3;
                    float lum = (image[idx] + image[idx + 1] + image[idx + 2]) / 3.0f;
                    variance += (lum - mean) * (lum - mean);
                }
            }
            variance /= 9.0f;

            totalVariance += variance;
            count++;
        }
    }

    return totalVariance / count;
}

static void SavePPM(const std::vector<float>& image, int width, int height, const std::string& path) {
    std::filesystem::path filePath(path);
    std::filesystem::create_directories(filePath.parent_path());

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        CFFLog::Warning("[TestOIDNDenoiser] Could not save: %s", path.c_str());
        return;
    }

    // PPM header
    file << "P6\n" << width << " " << height << "\n255\n";

    // Write pixels
    for (int i = 0; i < width * height; i++) {
        auto tonemap = [](float v) -> uint8_t {
            v = std::max(0.0f, std::min(1.0f, v));
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        };

        uint8_t r = tonemap(image[i * 3 + 0]);
        uint8_t g = tonemap(image[i * 3 + 1]);
        uint8_t b = tonemap(image[i * 3 + 2]);

        file.write(reinterpret_cast<char*>(&r), 1);
        file.write(reinterpret_cast<char*>(&g), 1);
        file.write(reinterpret_cast<char*>(&b), 1);
    }

    CFFLog::Info("[TestOIDNDenoiser] Saved: %s", path.c_str());
}

class CTestOIDNDenoiser : public ITestCase {
public:
    const char* GetName() const override {
        return "TestOIDNDenoiser";
    }

    void Setup(CTestContext& ctx) override {
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestOIDNDenoiser: Frame 1 - OIDN Denoiser Test");
            CFFLog::Info("========================================");

            // Test 1: Initialize denoiser
            CFFLog::Info("Test 1: Initialize OIDN denoiser");
            CLightmapDenoiser denoiser;
            if (!denoiser.Initialize()) {
                CFFLog::Error("[TestOIDNDenoiser] FAILED: Could not initialize OIDN denoiser");
                CFFLog::Error("[TestOIDNDenoiser] Error: %s", denoiser.GetLastError());
                ctx.failures.push_back("OIDN initialization failed");
                ctx.Finish();
                return;
            }
            CFFLog::Info("[TestOIDNDenoiser] PASS: OIDN initialized successfully");

            // Test 2: Create synthetic noisy image (256x256)
            CFFLog::Info("Test 2: Create synthetic noisy image");
            const int width = 256;
            const int height = 256;
            std::vector<float> noisyImage(width * height * 3);
            std::vector<float> originalImage(width * height * 3);

            srand(42);  // Reproducible
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int idx = (y * width + x) * 3;

                    float baseR = static_cast<float>(x) / width;
                    float baseG = static_cast<float>(y) / height;
                    float baseB = 0.5f;

                    float noiseScale = 0.3f;
                    float noiseR = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 2.0f * noiseScale;
                    float noiseG = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 2.0f * noiseScale;
                    float noiseB = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 2.0f * noiseScale;

                    originalImage[idx + 0] = baseR;
                    originalImage[idx + 1] = baseG;
                    originalImage[idx + 2] = baseB;

                    noisyImage[idx + 0] = std::max(0.0f, baseR + noiseR);
                    noisyImage[idx + 1] = std::max(0.0f, baseG + noiseG);
                    noisyImage[idx + 2] = std::max(0.0f, baseB + noiseB);
                }
            }

            float noiseBefore = CalculateNoiseMSE(noisyImage, originalImage, width, height);
            CFFLog::Info("[TestOIDNDenoiser] Noise MSE before denoising: %.6f", noiseBefore);

            std::string debugDir = GetTestDebugDir("TestOIDNDenoiser");
            SavePPM(noisyImage, width, height, debugDir + "/noisy_before.ppm");

            // Test 3: Run denoiser
            CFFLog::Info("Test 3: Run OIDN denoise");
            std::vector<float> denoisedImage = noisyImage;

            if (!denoiser.Denoise(denoisedImage.data(), width, height)) {
                CFFLog::Error("[TestOIDNDenoiser] FAILED: Denoising failed");
                CFFLog::Error("[TestOIDNDenoiser] Error: %s", denoiser.GetLastError());
                ctx.failures.push_back("OIDN denoising failed");
                ctx.Finish();
                return;
            }
            CFFLog::Info("[TestOIDNDenoiser] PASS: Denoising completed");

            // Test 4: Verify noise reduction
            CFFLog::Info("Test 4: Verify noise reduction");
            float noiseAfter = CalculateNoiseMSE(denoisedImage, originalImage, width, height);
            CFFLog::Info("[TestOIDNDenoiser] Noise MSE after denoising: %.6f", noiseAfter);

            float noiseReduction = (noiseBefore - noiseAfter) / noiseBefore * 100.0f;
            CFFLog::Info("[TestOIDNDenoiser] Noise reduction: %.1f%%", noiseReduction);

            SavePPM(denoisedImage, width, height, debugDir + "/denoised_after.ppm");

            float changeAmount = CalculateImageDifference(noisyImage, denoisedImage, width, height);
            CFFLog::Info("[TestOIDNDenoiser] Image change amount: %.6f", changeAmount);

            if (changeAmount < 0.0001f) {
                CFFLog::Error("[TestOIDNDenoiser] FAILED: Denoiser did not modify the image!");
                ctx.failures.push_back("Denoiser did not modify image");
            } else if (noiseReduction < 10.0f) {
                CFFLog::Warning("[TestOIDNDenoiser] WARNING: Noise reduction is low (%.1f%%)", noiseReduction);
            } else {
                CFFLog::Info("[TestOIDNDenoiser] PASS: Noise significantly reduced");
            }

            // Test 5: Realistic lightmap test
            CFFLog::Info("Test 5: Realistic lightmap scenario");
            TestRealisticLightmap(denoiser, debugDir);

            denoiser.Shutdown();
            CFFLog::Info("[TestOIDNDenoiser] All tests complete. Check debug folder for images.");

            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }

private:
    static void TestRealisticLightmap(CLightmapDenoiser& denoiser, const std::string& debugDir) {
        const int width = 512;
        const int height = 512;
        std::vector<float> lightmap(width * height * 3);

        srand(12345);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 3;

                float base = 0.05f;
                float cx1 = 128, cy1 = 128;
                float cx2 = 384, cy2 = 384;
                float d1 = std::sqrt(float((x - cx1) * (x - cx1) + (y - cy1) * (y - cy1)));
                float d2 = std::sqrt(float((x - cx2) * (x - cx2) + (y - cy2) * (y - cy2)));

                float light1 = std::max(0.0f, 1.0f - d1 / 100.0f) * 0.8f;
                float light2 = std::max(0.0f, 1.0f - d2 / 80.0f) * 0.6f;
                float brightness = base + light1 + light2;

                float noiseScale = 0.15f + brightness * 0.1f;
                float noise = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * noiseScale;

                lightmap[idx + 0] = std::max(0.0f, brightness * 1.0f + noise);
                lightmap[idx + 1] = std::max(0.0f, brightness * 0.9f + noise * 0.8f);
                lightmap[idx + 2] = std::max(0.0f, brightness * 0.7f + noise * 0.6f);
            }
        }

        SavePPM(lightmap, width, height, debugDir + "/lightmap_before.ppm");

        float varianceBefore = CalculateLocalVariance(lightmap, width, height);
        CFFLog::Info("[TestOIDNDenoiser] Lightmap local variance before: %.6f", varianceBefore);

        if (!denoiser.Denoise(lightmap.data(), width, height)) {
            CFFLog::Error("[TestOIDNDenoiser] Lightmap denoising failed: %s", denoiser.GetLastError());
            return;
        }

        SavePPM(lightmap, width, height, debugDir + "/lightmap_after.ppm");

        float varianceAfter = CalculateLocalVariance(lightmap, width, height);
        CFFLog::Info("[TestOIDNDenoiser] Lightmap local variance after: %.6f", varianceAfter);

        float varianceReduction = (varianceBefore - varianceAfter) / varianceBefore * 100.0f;
        CFFLog::Info("[TestOIDNDenoiser] Lightmap variance reduction: %.1f%%", varianceReduction);

        if (varianceReduction > 30.0f) {
            CFFLog::Info("[TestOIDNDenoiser] PASS: Lightmap denoising effective");
        } else {
            CFFLog::Warning("[TestOIDNDenoiser] WARNING: Lightmap denoising may not be working correctly");
        }
    }
};

REGISTER_TEST(CTestOIDNDenoiser)
