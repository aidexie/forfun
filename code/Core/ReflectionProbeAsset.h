#pragma once
#include "RHI/RHIResources.h"
#include <string>
#include <memory>

// ============================================
// CReflectionProbeAsset - Reflection Probe Asset
// ============================================
// 管理 Reflection Probe 的资产文件（.ffasset + KTX2）
//
// 文件格式：
// {
//   "type": "reflection_probe",
//   "version": "1.0",
//   "resolution": 256,
//   "environmentMap": "env.ktx2",          // 相对于 .ffasset 的路径
//   "irradianceMap": "irradiance.ktx2",
//   "prefilteredMap": "prefiltered.ktx2"
// }
//
// 文件结构示例：
// E:/forfun/assets/probes/living_room/
// ├── living_room.ffasset
// ├── env.ktx2
// ├── irradiance.ktx2
// └── prefiltered.ktx2
// ============================================
class CReflectionProbeAsset
{
public:
    CReflectionProbeAsset() = default;
    ~CReflectionProbeAsset() = default;

    // ============================================
    // Asset Data (Public for serialization)
    // ============================================

    int m_resolution = 256;

    // 相对路径（相对于 .ffasset 所在目录）
    std::string m_environmentMap = "env.ktx2";
    std::string m_irradianceMap = "irradiance.ktx2";
    std::string m_prefilteredMap = "prefiltered.ktx2";

    // ============================================
    // Serialization
    // ============================================

    // 保存到 .ffasset 文件
    // path: 完整路径，如 "E:/forfun/assets/probes/living_room/living_room.ffasset"
    bool SaveToFile(const std::string& path);

    // 从 .ffasset 文件加载
    // path: 完整路径，如 "E:/forfun/assets/probes/living_room/living_room.ffasset"
    bool LoadFromFile(const std::string& path);

    // ============================================
    // Texture Loading (RHI Interface)
    // ============================================

    // 加载 Environment Cubemap
    // assetPath: .ffasset 的完整路径
    // 返回: RHI 纹理指针（调用者负责管理生命周期）
    RHI::ITexture* LoadEnvironmentTexture(const std::string& assetPath);

    // 加载 Irradiance Cubemap
    RHI::ITexture* LoadIrradianceTexture(const std::string& assetPath);

    // 加载 Pre-filtered Cubemap
    RHI::ITexture* LoadPrefilteredTexture(const std::string& assetPath);

private:
    // 辅助函数：从 .ffasset 路径构建纹理的完整路径
    // assetPath: "E:/forfun/assets/probes/living_room/living_room.ffasset"
    // relativePath: "env.ktx2"
    // 返回: "E:/forfun/assets/probes/living_room/env.ktx2"
    std::string buildTexturePath(const std::string& assetPath, const std::string& relativePath);
};
