#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>
#include <string>

// ============================================
// SReflectionProbe - Reflection Probe Component
// ============================================
// 用于在场景中烘焙局部反射的 Cubemap
//
// 工作流程：
// 1. 在场景中放置 GameObject + ReflectionProbe Component
// 2. 编辑器中点击 "Bake Now" 触发烘焙
// 3. Baker 从 Probe 位置渲染 6 个面 → Cubemap
// 4. 生成 IBL maps（Irradiance + Pre-filtered）
// 5. 保存为 .ffasset + KTX2 文件
// 6. 运行时加载并使用最近的 Probe
//
// 文件结构：
// E:/forfun/assets/probes/living_room/
// ├── living_room.ffasset   # Metadata + 路径引用
// ├── env.ktx2              # Environment cubemap
// ├── irradiance.ktx2       # Diffuse irradiance
// └── prefiltered.ktx2      # Specular pre-filtered
// ============================================
struct SReflectionProbe : public CComponent
{
    // ============================================
    // Baking Settings
    // ============================================

    // Cubemap 分辨率（128/256/512）
    // - 128: 低质量，节省内存，适合大量 Probe
    // - 256: 中等质量（推荐）
    // - 512: 高质量，内存开销大
    int resolution = 256;

    // ============================================
    // Influence Settings
    // ============================================

    // 影响范围（球形半径）
    // - 运行时用于选择最近的 Probe
    // - 编辑器中显示为半透明球体 Gizmo
    float radius = 10.0f;

    // ============================================
    // Asset Reference
    // ============================================

    // 烘焙后的资产路径（相对于 E:/forfun/assets/）
    // 例如: "probes/living_room/living_room.ffasset"
    // .ffasset 内部包含 3 个 KTX2 的相对路径
    std::string assetPath;

    // ============================================
    // Runtime State (Transient - 不序列化)
    // ============================================

    // 是否需要重新烘焙（场景变化时标记为 true）
    // 注意：这个字段不应该被序列化到场景文件
    bool isDirty = true;

    // ============================================
    // IComponent Interface
    // ============================================

    const char* GetTypeName() const override { return "ReflectionProbe"; }

    void VisitProperties(CPropertyVisitor& visitor) override
    {
        // Baking Settings
        visitor.VisitInt("resolution", resolution);

        // Influence Settings
        visitor.VisitFloat("radius", radius);

        // Asset Reference
        visitor.VisitString("assetPath", assetPath);

        // 注意：isDirty 不序列化（运行时状态）
    }
};

// 自动注册组件
REGISTER_COMPONENT(SReflectionProbe)
