#pragma once
#include <cstddef>
#include <string>
#include "World.h"
#include "Rendering/Skybox.h"
#include "Rendering/IBLGenerator.h"

// Scene singleton - manages world, skybox, and IBL resources
class Scene {
public:
    // Singleton access
    static Scene& Instance() {
        static Scene instance;
        return instance;
    }

    // Delete copy/move constructors
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    // Initialize scene (load skybox and generate IBL)
    bool Initialize(const std::string& hdrPath = "skybox/newport_loft.hdr", int cubemapSize = 512);
    void Shutdown();

    // World access
    World& GetWorld() { return m_world; }
    const World& GetWorld() const { return m_world; }

    // Selection
    int GetSelected() const { return m_selected; }
    void SetSelected(int index) { m_selected = index; }
    GameObject* GetSelectedObject() {
        if (m_selected >= 0 && (std::size_t)m_selected < m_world.Count())
            return m_world.Get((std::size_t)m_selected);
        return nullptr;
    }

    // Skybox access
    Skybox& GetSkybox() { return m_skybox; }
    const Skybox& GetSkybox() const { return m_skybox; }

    // IBL access
    IBLGenerator& GetIBLGenerator() { return m_iblGen; }
    const IBLGenerator& GetIBLGenerator() const { return m_iblGen; }

private:
    // Private constructor for singleton
    Scene() = default;
    ~Scene() = default;

private:
    World m_world;
    int m_selected = -1;
    Skybox m_skybox;
    IBLGenerator m_iblGen;
    bool m_initialized = false;
};
