#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>
#include <string>

// ============================================
// SLightProbe - Light Probe Component
// ============================================
// 用于存储场景中某个位置的漫反射环境光（球谐系数）
//
// 与 Reflection Probe 的区别：
// - Reflection Probe: 镜面反射（高频），存储 Cubemap
// - Light Probe: 漫反射环境光（低频），存储 SH 系数
//
// 工作流程：
// 1. 在场景中手动放置 GameObject + LightProbe Component
// 2. Light Settings 面板点击 "Bake All Light Probes"
// 3. Baker 从 Probe 位置渲染低分辨率 Cubemap（16x16 或 32x32）
// 4. 投影到 L2 球谐系数（9 个系数 × RGB = 27 floats）
// 5. 序列化到场景文件（.scene）
// 6. 运行时距离权重混合最近的 4 个 Probe
//
// 数据格式：
// - L2 球谐（9 个系数）
// - 每个系数是 RGB（3 floats）
// - 总共 27 floats per probe
// ============================================
struct SLightProbe : public CComponent
{
    // ============================================
    // Influence Settings
    // ============================================

    // 影响范围（球形半径）
    // - 运行时用于选择参与混合的 Probe
    // - 编辑器中显示为半透明球体 Gizmo
    float radius = 10.0f;

    // ============================================
    // Spherical Harmonics Coefficients (L2)
    // ============================================
    // 存储格式：shCoeffs[band] = RGB
    // - band: 0-8 (L0=1, L1=3, L2=5, total=9)
    // - 每个系数是 RGB 向量
    //
    // 初始值：全黑（未烘焙状态）
    // 烘焙后：从 Cubemap 投影得到的系数
    DirectX::XMFLOAT3 shCoeffs[9] = {
        {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        {0, 0, 0}, {0, 0, 0}, {0, 0, 0}
    };

    // ============================================
    // Runtime State (Transient - 不序列化)
    // ============================================

    // 是否需要重新烘焙（场景变化时标记为 true）
    bool isDirty = true;

    // ============================================
    // IComponent Interface
    // ============================================

    const char* GetTypeName() const override { return "LightProbe"; }

    void VisitProperties(CPropertyVisitor& visitor) override
    {
        // Influence Settings
        visitor.VisitFloat("radius", radius);

        // SH Coefficients (9 x RGB = 27 floats, stored as flat array)
        visitor.VisitFloat3Array("shCoeffs", shCoeffs, 9);

        // 注意：isDirty 不序列化（运行时状态）
    }
};

// 自动注册组件
REGISTER_COMPONENT(SLightProbe)
