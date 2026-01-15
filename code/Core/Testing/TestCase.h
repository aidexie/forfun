#pragma once
#include <functional>
#include <map>
#include <vector>
#include <string>
#include <DirectXMath.h>
#include "Engine/Rendering/ShowFlags.h"

// Forward declarations
class CScene;
class CTestContext;
class CRenderPipeline;

// Assertion macros (fail-fast: return immediately on failure)
#define ASSERT(ctx, condition, message) \
    if (!(ctx).Assert(condition, message)) return

#define ASSERT_EQUAL(ctx, actual, expected, message) \
    if (!(ctx).AssertEqual(actual, expected, message)) return

#define ASSERT_EQUAL_F(ctx, actual, expected, epsilon, message) \
    if (!(ctx).AssertEqual(actual, expected, epsilon, message)) return

#define ASSERT_NOT_NULL(ctx, ptr, message) \
    if (!(ctx).AssertNotNull(ptr, message)) return

#define ASSERT_IN_RANGE(ctx, actual, min, max, message) \
    if (!(ctx).AssertInRange(actual, min, max, message)) return

#define ASSERT_VEC3_EQUAL(ctx, actual, expected, epsilon, message) \
    if (!(ctx).AssertVector3Equal(actual, expected, epsilon, message)) return

// Helper functions for test paths
std::string GetTestDebugDir(const char* testName);
std::string GetTestLogPath(const char* testName);
std::string GetTestScreenshotPath(const char* testName, int frame);

// Test case interface
class ITestCase {
public:
    virtual ~ITestCase() = default;

    // Get test name
    virtual const char* GetName() const = 0;

    // Setup test flow (register frame callbacks)
    virtual void Setup(CTestContext& ctx) = 0;
};

// Test execution context
class CTestContext {
public:
    int currentFrame = 0;       // Current frame number
    bool testPassed = false;    // Test result
    const char* testName = nullptr;  // Test name for detailed logging
    CRenderPipeline* pipeline = nullptr;  // Access to rendering for screenshots
    std::vector<std::string> failures;  // Collected failures
    FShowFlags showFlags = FShowFlags::Editor();  // Rendering feature flags (tests can modify)

    // Register a callback for a specific frame
    void OnFrame(int frameNumber, std::function<void()> callback) {
        m_frameCallbacks[frameNumber] = callback;
    }

    // Execute callbacks for current frame
    void ExecuteFrame(int frame) {
        currentFrame = frame;
        auto it = m_frameCallbacks.find(frame);
        if (it != m_frameCallbacks.end()) {
            it->second();  // Execute callback
        }
    }

    // Mark test as finished
    void Finish() {
        m_finished = true;
    }

    // Check if test is finished
    bool IsFinished() const {
        return m_finished;
    }

    // Assertion methods (return false on failure)
    bool Assert(bool condition, const char* message);
    bool AssertEqual(int actual, int expected, const char* message);
    bool AssertEqual(float actual, float expected, float epsilon, const char* message);
    bool AssertEqual(const std::string& actual, const std::string& expected, const char* message);
    bool AssertNotNull(const void* ptr, const char* message);
    bool AssertInRange(float actual, float min, float max, const char* message);
    bool AssertVector3Equal(const DirectX::XMFLOAT3& actual, const DirectX::XMFLOAT3& expected, float epsilon, const char* message);

private:
    std::map<int, std::function<void()>> m_frameCallbacks;
    bool m_finished = false;

    // Record failure with detailed formatting
    void RecordFailure(const char* formatted_message);
};
