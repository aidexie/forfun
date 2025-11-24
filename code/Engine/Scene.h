#pragma once
#include <cstddef>
#include <string>
#include "World.h"
#include "Rendering/Skybox.h"
#include "Rendering/IBLGenerator.h"

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

    // Initialize CScene (load skybox and generate IBL)
    bool Initialize(const std::string& skybox_path);
    void Shutdown();

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

private:
    // Private constructor for singleton
    CScene() = default;
    ~CScene() = default;

private:
    CWorld m_world;
    int m_selected = -1;
    CSkybox m_skybox;
    CIBLGenerator m_iblGen;
    bool m_initialized = false;
};



