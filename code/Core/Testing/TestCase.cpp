#include "TestCase.h"
#include "Core/FFLog.h"
#include <cstdio>
#include <cmath>

void CTestContext::RecordFailure(const char* formatted_message) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[%s:Frame%d] %s",
             testName ? testName : "Unknown",
             currentFrame,
             formatted_message);

    failures.push_back(buf);
    CFFLog::Error("✗ %s", buf);
    testPassed = false;
}

bool CTestContext::Assert(bool condition, const char* message) {
    if (!condition) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Assertion failed: %s", message);
        RecordFailure(buf);
        return false;
    }
    return true;
}

bool CTestContext::AssertEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: expected %d, got %d",
                 message, expected, actual);
        RecordFailure(buf);
        return false;
    }
    return true;
}

bool CTestContext::AssertEqual(float actual, float expected, float epsilon, const char* message) {
    if (fabs(actual - expected) > epsilon) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: expected %.3f, got %.3f (epsilon: %.3f)",
                 message, expected, actual, epsilon);
        RecordFailure(buf);
        return false;
    }
    return true;
}

bool CTestContext::AssertEqual(const std::string& actual, const std::string& expected, const char* message) {
    if (actual != expected) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: expected \"%s\", got \"%s\"",
                 message, expected.c_str(), actual.c_str());
        RecordFailure(buf);
        return false;
    }
    return true;
}

bool CTestContext::AssertNotNull(const void* ptr, const char* message) {
    if (ptr == nullptr) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Null pointer: %s", message);
        RecordFailure(buf);
        return false;
    }
    return true;
}

bool CTestContext::AssertInRange(float actual, float min, float max, const char* message) {
    if (actual < min || actual > max) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: expected [%.3f, %.3f], got %.3f",
                 message, min, max, actual);
        RecordFailure(buf);
        return false;
    }
    return true;
}

bool CTestContext::AssertVector3Equal(const DirectX::XMFLOAT3& actual,
                                      const DirectX::XMFLOAT3& expected,
                                      float epsilon,
                                      const char* message) {
    float dx = fabs(actual.x - expected.x);
    float dy = fabs(actual.y - expected.y);
    float dz = fabs(actual.z - expected.z);

    if (dx > epsilon || dy > epsilon || dz > epsilon) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s: expected (%.3f, %.3f, %.3f), got (%.3f, %.3f, %.3f) (epsilon: %.3f)",
                 message,
                 expected.x, expected.y, expected.z,
                 actual.x, actual.y, actual.z,
                 epsilon);
        RecordFailure(buf);
        return false;
    }
    return true;
}
