// Core/DebugPaths.h
#pragma once
#include <string>
#include <windows.h>
#include "PathManager.h"  // FFPath namespace

// DebugPaths: Centralized management of debug output file paths
// All debug files (logs, screenshots, snapshots) are stored in FFPath::GetDebugDir()
class CDebugPaths {
public:
    // Root directory for all debug outputs
    static std::string GetDebugRoot() {
        return FFPath::GetDebugDir();
    }

    // Log file paths
    static std::string GetLogPath(const char* filename) {
        return FFPath::GetDebugDir() + "/logs/" + filename;
    }

    // Screenshot file paths
    static std::string GetScreenshotPath(const char* filename) {
        return FFPath::GetDebugDir() + "/screenshots/" + filename;
    }

    // Snapshot (scene state) file paths
    static std::string GetSnapshotPath(const char* filename) {
        return FFPath::GetDebugDir() + "/snapshots/" + filename;
    }

    // Ensure all debug directories exist (call once at startup)
    static void EnsureDirectoriesExist() {
        std::string debugDir = FFPath::GetDebugDir();
        CreateDirectoryA(FFPath::GetProjectRoot().c_str(), nullptr);
        CreateDirectoryA(debugDir.c_str(), nullptr);
        CreateDirectoryA((debugDir + "/logs").c_str(), nullptr);
        CreateDirectoryA((debugDir + "/screenshots").c_str(), nullptr);
        CreateDirectoryA((debugDir + "/snapshots").c_str(), nullptr);
    }
};
