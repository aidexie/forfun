#pragma once
#include <DirectXMath.h>
#include <vector>
#include <memory>

// Forward declaration
class GpuMeshResource;

// 纯数据结构：单个可渲染对象
// 不依赖任何业务对象（GameObject/Component等）
struct MeshRenderItem {
    std::shared_ptr<GpuMeshResource> gpuMesh;  // GPU资源
    DirectX::XMMATRIX worldMatrix;             // 世界矩阵
};

// 渲染列表：在Core和Engine边界传递的数据
using RenderList = std::vector<MeshRenderItem>;
