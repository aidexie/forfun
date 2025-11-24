#pragma once
#include <functional>
#include <map>

// Forward declarations
class CScene;
class CTestContext;

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

private:
    std::map<int, std::function<void()>> m_frameCallbacks;
    bool m_finished = false;
};
