// Editor/DiagnosticLog.cpp
#include "FFLog.h"
#include "Console.h"
#include <ctime>
#include <cstdarg>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <windows.h>

using namespace DirectX;

void CFFLog::BeginSession(const char* sessionType, const char* sessionName) {
    if (m_sessionActive) {
        EndSession();  // Auto-close previous session
    }

    m_sessionType = sessionType;
    m_sessionName = sessionName;
    m_sessionActive = true;
    m_indentLevel = 0;
    m_sessionStartTime = std::chrono::steady_clock::now();

    WriteLine("================================");
    WriteIndented("[%s: %s] %s", sessionType, sessionName, GetTimestamp().c_str());
    WriteLine("================================");
    WriteLine("");
}

void CFFLog::EndSession() {
    if (!m_sessionActive) return;

    // Calculate session duration
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_sessionStartTime);

    WriteLine("");
    WriteLine("================================");
    WriteIndented("[SESSION END] Duration: %lldms", duration.count());
    WriteLine("================================");
    WriteLine("");

    m_sessionActive = false;
}

void CFFLog::LogEvent(const char* eventName) {
    if (!m_sessionActive) return;

    WriteLine("");
    WriteIndented("[%s]", eventName);
    m_indentLevel++;
}

void CFFLog::LogInfo(const char* format, ...) {
    if (!m_sessionActive) return;

    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    WriteIndented("%s", buffer);

    va_end(args);
}

void CFFLog::LogVector(const char* name, const XMFLOAT3& v) {
    if (!m_sessionActive) return;
    WriteIndented("%-20s (%7.3f, %7.3f, %7.3f)", name, v.x, v.y, v.z);
}

void CFFLog::LogMatrix(const char* name, const XMMATRIX& m) {
    if (!m_sessionActive) return;

    XMFLOAT4X4 mat;
    XMStoreFloat4x4(&mat, m);

    WriteIndented("%s:", name);
    m_indentLevel++;
    WriteIndented("Row0: [%7.3f, %7.3f, %7.3f, %7.3f]", mat._11, mat._12, mat._13, mat._14);
    WriteIndented("Row1: [%7.3f, %7.3f, %7.3f, %7.3f]", mat._21, mat._22, mat._23, mat._24);
    WriteIndented("Row2: [%7.3f, %7.3f, %7.3f, %7.3f]", mat._31, mat._32, mat._33, mat._34);
    WriteIndented("Row3: [%7.3f, %7.3f, %7.3f, %7.3f]", mat._41, mat._42, mat._43, mat._44);
    m_indentLevel--;
}

void CFFLog::LogAABB(const char* name, const XMFLOAT3& min, const XMFLOAT3& max) {
    if (!m_sessionActive) return;

    WriteIndented("%s:", name);
    m_indentLevel++;
    WriteIndented("min = (%7.3f, %7.3f, %7.3f)", min.x, min.y, min.z);
    WriteIndented("max = (%7.3f, %7.3f, %7.3f)", max.x, max.y, max.z);
    m_indentLevel--;
}

void CFFLog::LogSuccess(const char* message) {
    if (!m_sessionActive) return;
    WriteIndented("✓ %s", message);
}

void CFFLog::LogFailure(const char* reason) {
    if (!m_sessionActive) return;
    WriteIndented("✗ %s", reason);
}

void CFFLog::LogExpectedValue(const char* name, const char* value) {
    if (!m_sessionActive) return;
    WriteIndented("[Expected] %s = \"%s\"", name, value);
}

void CFFLog::LogActualValue(const char* name, const char* value) {
    if (!m_sessionActive) return;
    WriteIndented("[Actual] %s = \"%s\"", name, value);
}

bool CFFLog::VerifyEqual(const char* expected, const char* actual) {
    bool equal = (strcmp(expected, actual) == 0);
    if (equal) {
        LogSuccess("Values match");
    } else {
        LogFailure("Values do not match");
    }
    return equal;
}

void CFFLog::LogSeparator(const char* label) {
    if (!m_sessionActive) return;

    if (label) {
        WriteIndented("--------------------------------");
        WriteIndented("[%s]", label);
        WriteIndented("--------------------------------");
    } else {
        WriteIndented("--------------------------------");
    }
}

void CFFLog::LogSubsectionStart(const char* label) {
    if (!m_sessionActive) return;

    WriteIndented("┌─ %s ─────────────", label);
    m_indentLevel++;
}

void CFFLog::LogSubsectionEnd() {
    if (!m_sessionActive) return;

    m_indentLevel--;
    WriteIndented("└────────────────────────────");
}

void CFFLog::FlushToFile(const char* filepath) {
    std::ofstream file(filepath, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        OutputDebugStringA(("Failed to open log file: " + std::string(filepath) + "\n").c_str());
        return;
    }

    for (const auto& line : m_buffer) {
        file << line << "\n";
    }

    file.close();
}

void CFFLog::AppendToFile(const char* filepath) {
    std::ofstream file(filepath, std::ios::out | std::ios::app);
    if (!file.is_open()) {
        OutputDebugStringA(("Failed to open log file: " + std::string(filepath) + "\n").c_str());
        return;
    }

    for (const auto& line : m_buffer) {
        file << line << "\n";
    }

    file.close();
}

void CFFLog::Clear() {
    m_buffer.clear();
    m_sessionActive = false;
    m_indentLevel = 0;
}

void CFFLog::WriteLine(const char* line) {
    m_buffer.push_back(line);
}

void CFFLog::WriteIndented(const char* format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    std::string line = GetIndent() + std::string(buffer);
    m_buffer.push_back(line);

    va_end(args);
}

std::string CFFLog::GetTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

std::string CFFLog::GetIndent() const {
    return std::string(m_indentLevel * 2, ' ');
}

// Convenience static logging methods
void CFFLog::Info(const char* format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    // Get timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);

    char timestamp[32];
    strftime(timestamp, 32, "%H:%M:%S", &timeinfo);

    // Format log line
    std::string logLine = std::string("[") + timestamp + "] [INFO] " + buffer;

    // Append to runtime log file
    std::ofstream file("E:/forfun/debug/logs/runtime.log", std::ios::app);
    if (file.is_open()) {
        file << logLine << "\n";
        file.close();
    }

    // Output to custom console
    Core::Console::PrintUTF8(logLine + "\n");
}

void CFFLog::Warning(const char* format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);

    char timestamp[32];
    strftime(timestamp, 32, "%H:%M:%S", &timeinfo);

    std::string logLine = std::string("[") + timestamp + "] [WARNING] " + buffer;

    std::ofstream file("E:/forfun/debug/logs/runtime.log", std::ios::app);
    if (file.is_open()) {
        file << logLine << "\n";
        file.close();
    }

    Core::Console::PrintUTF8(logLine + "\n");
}

void CFFLog::Error(const char* format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);

    char timestamp[32];
    strftime(timestamp, 32, "%H:%M:%S", &timeinfo);

    std::string logLine = std::string("[") + timestamp + "] [ERROR] " + buffer;

    std::ofstream file("E:/forfun/debug/logs/runtime.log", std::ios::app);
    if (file.is_open()) {
        file << logLine << "\n";
        file.close();
    }

    Core::Console::PrintUTF8(logLine + "\n");
}
