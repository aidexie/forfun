// Engine/Components/MeshRenderer.cpp
#include "MeshRenderer.h"
#include "Core/MeshResourceManager.h"
#include "Core/GpuMeshResource.h"
#include <d3d11.h>
#include <iostream>

bool SMeshRenderer::EnsureUploaded() {
    if (!meshes.empty()) return true;

    // Empty path means no mesh
    if (path.empty()) return false;

    // Load or retrieve cached resources via MeshResourceManager
    meshes = CMeshResourceManager::Instance().GetOrLoad(path);

    if (meshes.empty()) {
        std::cerr << "ERROR: Failed to load mesh from: " << path << std::endl;
    }

    return !meshes.empty();
}
