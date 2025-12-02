#pragma once
#include <string>

// ============================================
// FFPath - Unified Asset Path Management
// ============================================
//
// Design Principles:
// - External input: flexible (absolute or relative, / or \)
// - Internal storage: always normalized relative path
// - Internal format: "folder/file.ext" (no leading /, uses / separator)
//
// Key APIs (all auto-normalize input):
// - GetAbsolutePath(anyPath) → absolute path for file operations
// - Normalize(anyPath)       → normalized relative path for storage
//
// Examples:
//   FFPath::GetAbsolutePath("mat/wood.ffasset")                 → "E:/forfun/assets/mat/wood.ffasset"
//   FFPath::GetAbsolutePath("E:/forfun/assets/mat/wood.ffasset") → "E:/forfun/assets/mat/wood.ffasset"
//   FFPath::Normalize("E:/forfun/assets/mat/wood.ffasset")       → "mat/wood.ffasset"
//   FFPath::Normalize("mat\\wood.ffasset")                       → "mat/wood.ffasset"
// ============================================
namespace FFPath {

    // === Initialization ===
    // Call once at startup with project root (e.g., "E:/forfun")
    void Initialize(const std::string& projectRoot);
    bool IsInitialized();

    // === Directory Accessors ===
    const std::string& GetProjectRoot();
    const std::string& GetAssetsDir();
    const std::string& GetDebugDir();
    const std::string& GetSourceDir();

    // === Primary APIs (auto-normalize input) ===

    // Convert any path to absolute path for file operations
    // Input: any format (absolute, relative, mixed separators)
    // Output: "E:/forfun/assets/folder/file.ext"
    std::string GetAbsolutePath(const std::string& anyPath);

    // Convert any path to normalized relative path for internal storage
    // Input: any format (absolute, relative, mixed separators)
    // Output: "folder/file.ext"
    std::string Normalize(const std::string& anyPath);

    // === Utilities ===
    bool IsAbsolutePath(const std::string& path);
    bool IsUnderAssetsDir(const std::string& absolutePath);

    // Normalize separators only (\ → /)
    std::string NormalizeSeparators(const std::string& path);

}  // namespace FFPath
