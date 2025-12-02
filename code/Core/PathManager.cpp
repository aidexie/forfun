#include "PathManager.h"
#include "FFLog.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
    // Module-level state
    std::string g_projectRoot;  // "E:/forfun"
    std::string g_assetsDir;    // "E:/forfun/assets"
    std::string g_debugDir;     // "E:/forfun/debug"
    std::string g_sourceDir;    // "E:/forfun/source/code"
    bool g_initialized = false;
}

namespace FFPath {

void Initialize(const std::string& projectRoot) {
    if (g_initialized) {
        CFFLog::Warning("[FFPath] Already initialized");
        return;
    }

    // Normalize the project root
    g_projectRoot = NormalizeSeparators(projectRoot);

    // Remove trailing slash if present
    if (!g_projectRoot.empty() && g_projectRoot.back() == '/') {
        g_projectRoot.pop_back();
    }

    g_assetsDir = g_projectRoot + "/assets";
    g_debugDir = g_projectRoot + "/debug";
    g_sourceDir = g_projectRoot + "/source/code";
    g_initialized = true;

    CFFLog::Info("[FFPath] Initialized:");
    CFFLog::Info("  Project Root: %s", g_projectRoot.c_str());
    CFFLog::Info("  Assets Dir:   %s", g_assetsDir.c_str());
    CFFLog::Info("  Debug Dir:    %s", g_debugDir.c_str());
}

bool IsInitialized() {
    return g_initialized;
}

const std::string& GetProjectRoot() {
    return g_projectRoot;
}

const std::string& GetAssetsDir() {
    return g_assetsDir;
}

const std::string& GetDebugDir() {
    return g_debugDir;
}

const std::string& GetSourceDir() {
    return g_sourceDir;
}

std::string NormalizeSeparators(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) return false;

    // Windows: "C:/" or "D:/" etc.
    if (path.length() >= 2 && path[1] == ':') {
        return true;
    }

    // Unix: starts with /
    if (path[0] == '/') {
        return true;
    }

    return false;
}

bool IsUnderAssetsDir(const std::string& absolutePath) {
    if (!g_initialized) return false;

    std::string normalized = NormalizeSeparators(absolutePath);

    // Case-insensitive comparison for Windows
    std::string lowerPath = normalized;
    std::string lowerAssets = g_assetsDir;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    std::transform(lowerAssets.begin(), lowerAssets.end(), lowerAssets.begin(), ::tolower);

    return lowerPath.find(lowerAssets) == 0;
}

std::string Normalize(const std::string& anyPath) {
    if (anyPath.empty()) return "";
    if (!g_initialized) {
        CFFLog::Error("[FFPath] Not initialized! Call Initialize() first.");
        return anyPath;
    }

    // Step 1: Normalize separators
    std::string path = NormalizeSeparators(anyPath);

    // Step 2: If absolute path, remove assets prefix
    if (IsAbsolutePath(path)) {
        // Case-insensitive prefix removal for Windows
        std::string lowerPath = path;
        std::string lowerAssetsPrefix = g_assetsDir + "/";
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        std::transform(lowerAssetsPrefix.begin(), lowerAssetsPrefix.end(), lowerAssetsPrefix.begin(), ::tolower);

        if (lowerPath.find(lowerAssetsPrefix) == 0) {
            // Remove the prefix (use original path to preserve case)
            path = path.substr(g_assetsDir.length() + 1);
        } else {
            // Absolute path not under assets - log warning and return as-is
            CFFLog::Warning("[FFPath] Path not under assets dir: %s", anyPath.c_str());
            return path;
        }
    }

    // Step 3: Remove leading "./" if present
    while (path.length() >= 2 && path[0] == '.' && path[1] == '/') {
        path = path.substr(2);
    }

    // Step 4: Remove leading "/" if present
    while (!path.empty() && path[0] == '/') {
        path = path.substr(1);
    }

    // Step 5: Remove trailing "/" if present
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }

    return path;
}

std::string GetAbsolutePath(const std::string& anyPath) {
    if (anyPath.empty()) return "";
    if (!g_initialized) {
        CFFLog::Error("[FFPath] Not initialized! Call Initialize() first.");
        return anyPath;
    }

    // First normalize the input
    std::string normalized = NormalizeSeparators(anyPath);

    // If already absolute and under assets, just normalize separators
    if (IsAbsolutePath(normalized)) {
        return normalized;
    }

    // Normalize to relative path first (handles ./ and other edge cases)
    std::string relPath = Normalize(anyPath);

    // Then convert to absolute
    return g_assetsDir + "/" + relPath;
}

}  // namespace FFPath
