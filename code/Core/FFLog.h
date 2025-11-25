// Core/FFLog.h
#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <DirectXMath.h>

// DiagnosticLog: Unified logging system for debugging and automated testing
// Supports hierarchical logging with sessions, events, and details
// Output format is designed for both human readability and automated parsing
class CFFLog {
public:
    static CFFLog& Instance() {
        static CFFLog instance;
        return instance;
    }

    // Session management
    // sessionType: "AUTO_TEST" | "USER_SESSION" | "DEBUG" | "BENCHMARK"
    void BeginSession(const char* sessionType, const char* sessionName);
    void EndSession();

    // Event logging (creates new section)
    void LogEvent(const char* eventName);

    // Information logging
    void LogInfo(const char* format, ...);

    // Math data logging (formatted consistently)
    void LogVector(const char* name, const DirectX::XMFLOAT3& v);
    void LogMatrix(const char* name, const DirectX::XMMATRIX& m);
    void LogAABB(const char* name, const DirectX::XMFLOAT3& min, const DirectX::XMFLOAT3& max);

    // Result logging (with visual markers)
    void LogSuccess(const char* message);
    void LogFailure(const char* reason);

    // Test verification (for AUTO_TEST sessions)
    void LogExpectedValue(const char* name, const char* value);
    void LogActualValue(const char* name, const char* value);
    bool VerifyEqual(const char* expected, const char* actual);

    // Section markers (for visual separation)
    void LogSeparator(const char* label = nullptr);
    void LogSubsectionStart(const char* label);
    void LogSubsectionEnd();

    // File output
    void FlushToFile(const char* filepath);
    void AppendToFile(const char* filepath);

    // Clear buffer
    void Clear();

    // Convenience static methods for general logging (without explicit session management)
    // These write to a global RUNTIME log that persists throughout program execution
    static void Info(const char* format, ...);
    static void Warning(const char* format, ...);
    static void Error(const char* format, ...);

    // Set runtime log path (for test mode)
    static void SetRuntimeLogPath(const char* path);
    static const char* GetRuntimeLogPath();

private:
    CFFLog() = default;
    ~CFFLog() = default;
    CFFLog(const CFFLog&) = delete;
    CFFLog& operator=(const CFFLog&) = delete;

    void WriteLine(const char* line);
    void WriteIndented(const char* format, ...);
    std::string GetTimestamp() const;
    std::string GetIndent() const;

    std::vector<std::string> m_buffer;
    std::string m_sessionType;
    std::string m_sessionName;
    int m_indentLevel = 0;
    bool m_sessionActive = false;

    // Session timing
    std::chrono::steady_clock::time_point m_sessionStartTime;
};
