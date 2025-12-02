#pragma once
#include <cstddef>
#include <string>
#include "World.h"
#include "Rendering/Skybox.h"
#include "Rendering/IBLGenerator.h"
#include "Rendering/ReflectionProbeManager.h"
#include "SceneLightSettings.h"
#include "Camera.h"

// CScene singleton - manages CWorld, skybox, and IBL resources
class CScene {
public:
    // Singleton access
    static CScene& Instance() {
        static CScene instance;
        return instance;
    }

    // Delete copy/move constructors
    CScene(const CScene&) = delete;
    CScene& operator=(const CScene&) = delete;
    CScene(CScene&&) = delete;
    CScene& operator=(CScene&&) = delete;

    // === Lifecycle ===
    bool Initialize();   // Create GPU resources (call once at startup)
    void Shutdown();
    void Clear();        // Clear all GameObjects and reset selection

    // === Scene File Management ===
    bool LoadFromFile(const std::string& scenePath);
    bool SaveToFile(const std::string& scenePath);

    // === Environment Resource Management ===
    // Reload skybox display cubemap only
    bool ReloadSkybox(const std::string& envKtxPath);
    // Reload global IBL (irradiance + prefiltered)
    bool ReloadIBL(const std::string& irrPath, const std::string& prefilterPath);
    // Convenience: reload both skybox and IBL from .ffasset
    bool ReloadEnvironment(const std::string& ffassetPath);

    // === Reflection Probe Management ===
    // Reload all probes from scene's ReflectionProbe components
    void ReloadProbesFromScene();
    // Hot-reload single probe (after baking)
    bool ReloadProbe(int probeIndex, const std::string& assetPath);

    // CWorld access
    CWorld& GetWorld() { return m_world; }
    const CWorld& GetWorld() const { return m_world; }

    // Selection
    int GetSelected() const { return m_selected; }
    void SetSelected(int index) { m_selected = index; }
    CGameObject* GetSelectedObject() {
        if (m_selected >= 0 && (std::size_t)m_selected < m_world.Count())
            return m_world.Get((std::size_t)m_selected);
        return nullptr;
    }

    // Skybox access
    CSkybox& GetSkybox() { return m_skybox; }
    const CSkybox& GetSkybox() const { return m_skybox; }

    // IBL access
    CIBLGenerator& GetIBLGenerator() { return m_iblGen; }
    const CIBLGenerator& GetIBLGenerator() const { return m_iblGen; }

    // Reflection Probe Manager access
    CReflectionProbeManager& GetProbeManager() { return m_probeManager; }
    const CReflectionProbeManager& GetProbeManager() const { return m_probeManager; }

    // Light settings access
    CSceneLightSettings& GetLightSettings() { return m_lightSettings; }
    const CSceneLightSettings& GetLightSettings() const { return m_lightSettings; }

    // ✅ 编辑器相机访问（相机现在属于 Scene 逻辑层）
    CCamera& GetEditorCamera() { return m_editorCamera; }
    const CCamera& GetEditorCamera() const { return m_editorCamera; }

    // State query for AI testing
    std::string GenerateReport() const;
    // File path management
    const std::string& GetFilePath() const { return m_filePath; }
    void SetFilePath(const std::string& path) { m_filePath = path; }
    bool HasFilePath() const { return !m_filePath.empty(); }

    // Copy/Paste/Duplicate operations (for Hierarchy panel)
    void CopyGameObject(CGameObject* go);     // Copy to clipboard (JSON)
    CGameObject* PasteGameObject();           // Paste from clipboard
    CGameObject* DuplicateGameObject(CGameObject* go);  // Copy + Paste in one step

private:
    // Private constructor for singleton
    CScene() = default;
    ~CScene() = default;

private:
    CWorld m_world;
    int m_selected = -1;
    std::string m_filePath;  // Current scene file path
    CSkybox m_skybox;
    CIBLGenerator m_iblGen;
    CReflectionProbeManager m_probeManager;
    CSceneLightSettings m_lightSettings;
    bool m_initialized = false;

    CCamera m_editorCamera;
};



