// Engine/Components/MeshRenderer.cpp
#include "MeshRenderer.h"
#include "Core/MeshResourceManager.h"
#include "Core/GpuMeshResource.h"
#include <d3d11.h>

bool MeshRenderer::EnsureUploaded() {
    if (!meshes.empty()) return true;

    // Empty path means no mesh
    if (path.empty()) return false;

    // Load or retrieve cached resources via MeshResourceManager
    meshes = MeshResourceManager::Instance().GetOrLoad(path);

    return !meshes.empty();
}
