// Editor/DebugPaths.h
#pragma once
#include <string>
#include <windows.h>

// DebugPaths: Centralized management of debug output file paths
// All debug files (logs, screenshots, snapshots) are stored in E:/forfun/debug/
class CDebugPaths {
public:
    // Root directory for all debug outputs
    static const char* GetDebugRoot() {
        return "E:/forfun/debug";
    }

    // Log file paths
    static std::string GetLogPath(const char* filename) {
        return std::string(GetDebugRoot()) + "/logs/" + filename;
    }

    // Screenshot file paths
    static std::string GetScreenshotPath(const char* filename) {
        return std::string(GetDebugRoot()) + "/screenshots/" + filename;
    }

    // Snapshot (scene state) file paths
    static std::string GetSnapshotPath(const char* filename) {
        return std::string(GetDebugRoot()) + "/snapshots/" + filename;
    }

    // Ensure all debug directories exist (call once at startup)
    static void EnsureDirectoriesExist() {
        CreateDirectoryA("E:/forfun", nullptr);
        CreateDirectoryA("E:/forfun/debug", nullptr);
        CreateDirectoryA("E:/forfun/debug/logs", nullptr);
        CreateDirectoryA("E:/forfun/debug/screenshots", nullptr);
        CreateDirectoryA("E:/forfun/debug/snapshots", nullptr);
    }
};
