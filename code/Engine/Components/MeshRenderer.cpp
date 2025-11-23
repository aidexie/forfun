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

bool SMeshRenderer::GetLocalBounds(DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax) const {
    if (meshes.empty() || !meshes[0]->hasBounds) {
        return false;
    }
    outMin = meshes[0]->localBoundsMin;
    outMax = meshes[0]->localBoundsMax;
    return true;
}
