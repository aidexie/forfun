#include "ReflectionProbeAsset.h"
#include "FFLog.h"
#include "TextureManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;

bool CReflectionProbeAsset::SaveToFile(const std::string& path)
{
    try {
        // 创建 JSON 对象
        json j;
        j["type"] = "reflection_probe";
        j["version"] = "1.0";
        j["resolution"] = m_resolution;
        j["environmentMap"] = m_environmentMap;
        j["irradianceMap"] = m_irradianceMap;
        j["prefilteredMap"] = m_prefilteredMap;

        // 确保目录存在
        std::filesystem::path filePath(path);
        std::filesystem::path dir = filePath.parent_path();
        if (!dir.empty() && !std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
            CFFLog::Info("Created directory: %s", dir.string().c_str());
        }

        // 写入文件
        std::ofstream file(path);
        if (!file.is_open()) {
            CFFLog::Error("Failed to open file for writing: %s", path.c_str());
            return false;
        }

        file << j.dump(2);  // Pretty print with 2-space indent
        file.close();

        CFFLog::Info("Saved ReflectionProbeAsset: %s", path.c_str());
        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error("Failed to save ReflectionProbeAsset: %s", e.what());
        return false;
    }
}

bool CReflectionProbeAsset::LoadFromFile(const std::string& path)
{
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            CFFLog::Error("Failed to open file: %s", path.c_str());
            return false;
        }

        json j = json::parse(file);
        file.close();

        // 验证文件类型
        if (j.value("type", "") != "reflection_probe") {
            CFFLog::Error("Invalid asset type: %s", path.c_str());
            return false;
        }

        // 加载数据
        m_resolution = j.value("resolution", 256);
        m_environmentMap = j.value("environmentMap", "env.ktx2");
        m_irradianceMap = j.value("irradianceMap", "irradiance.ktx2");
        m_prefilteredMap = j.value("prefilteredMap", "prefiltered.ktx2");

        CFFLog::Info("Loaded ReflectionProbeAsset: %s (resolution: %d)", path.c_str(), m_resolution);
        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error("Failed to load ReflectionProbeAsset: %s", e.what());
        return false;
    }
}

std::string CReflectionProbeAsset::buildTexturePath(const std::string& assetPath, const std::string& relativePath)
{
    // 从 assetPath 提取目录
    std::filesystem::path p(assetPath);
    std::filesystem::path dir = p.parent_path();

    // 拼接相对路径
    std::filesystem::path fullPath = dir / relativePath;

    return fullPath.string();
}

ID3D11ShaderResourceView* CReflectionProbeAsset::LoadEnvironmentSRV(const std::string& assetPath)
{
    std::string fullPath = buildTexturePath(assetPath, m_environmentMap);

    // 使用 TextureManager 加载 KTX2 纹理
    // 注意：TextureManager::LoadKTX2 返回的路径需要相对于 assets 目录
    // 这里假设 assetPath 是完整路径，需要转换为相对路径

    // TODO: 需要根据实际的 TextureManager API 调整
    // 临时方案：直接调用底层 KTX loader
    CFFLog::Warning("LoadEnvironmentSRV not fully implemented: %s", fullPath.c_str());
    return nullptr;
}

ID3D11ShaderResourceView* CReflectionProbeAsset::LoadIrradianceSRV(const std::string& assetPath)
{
    std::string fullPath = buildTexturePath(assetPath, m_irradianceMap);
    CFFLog::Warning("LoadIrradianceSRV not fully implemented: %s", fullPath.c_str());
    return nullptr;
}

ID3D11ShaderResourceView* CReflectionProbeAsset::LoadPrefilteredSRV(const std::string& assetPath)
{
    std::string fullPath = buildTexturePath(assetPath, m_prefilteredMap);
    CFFLog::Warning("LoadPrefilteredSRV not fully implemented: %s", fullPath.c_str());
    return nullptr;
}
