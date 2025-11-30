#pragma once

// ============================================
// FShowFlags - 渲染特性控制标志
// ============================================
// 借鉴 Unreal Engine 的 ShowFlags 设计
// 用于控制 RenderPipeline 中哪些功能应该被渲染
//
// 使用场景：
// - Editor Scene View: 完整渲染（游戏内容 + 编辑器工具）
// - Game View: 只渲染游戏内容
// - Material Preview: 简化渲染（无阴影、无后处理）
// - Reflection Probe: 场景内容 + IBL，无编辑器工具
// ============================================
struct FShowFlags
{
    // ============================================
    // Game Rendering Features
    // ============================================

    // Lighting & Shadows
    bool Lighting = true;           // 是否渲染光照
    bool Shadows = true;            // 是否渲染阴影（CSM）
    bool IBL = true;                // 是否使用 IBL（Image-Based Lighting）

    // Scene Elements
    bool Skybox = true;             // 是否渲染 Skybox
    bool OpaqueObjects = true;      // 是否渲染不透明物体
    bool TransparentObjects = true; // 是否渲染透明物体

    // Post-Processing
    bool PostProcessing = true;     // 是否应用后处理（Tone Mapping + Gamma Correction）

    // ============================================
    // Editor Tools (Only in Editor)
    // ============================================

    bool Grid = false;              // 是否渲染编辑器网格
    bool DebugLines = false;        // 是否渲染调试线框（AABB, Rays, etc.）
    bool Gizmos = false;            // 是否渲染 Gizmo（Transform, Light icons, etc.）
    bool SelectionOutline = false;  // 是否渲染选中物体的轮廓线

    // ============================================
    // Debug Visualization
    // ============================================

    bool Wireframe = false;         // 是否渲染线框模式
    bool ShowCascades = false;      // 是否显示 CSM Cascade 分层（调试用）
    bool ShowClusters = false;      // 是否显示 Clustered Lighting 分簇（调试用）
    bool ShowAABB = false;          // 是否显示所有物体的 AABB 包围盒
    bool ShowNormals = false;       // 是否显示法线（调试用）

    // ============================================
    // Presets - 常用配置预设
    // ============================================

    // 编辑器场景视图（完整功能）
    static FShowFlags Editor()
    {
        FShowFlags flags;
        flags.Lighting = true;
        flags.Shadows = true;
        flags.IBL = true;
        flags.Skybox = true;
        flags.OpaqueObjects = true;
        flags.TransparentObjects = true;
        flags.PostProcessing = true;
        flags.Grid = true;              // ✅ 编辑器网格
        flags.DebugLines = true;        // ✅ 调试线框
        flags.Gizmos = false;           // TODO: 未来实现
        flags.SelectionOutline = false; // TODO: 未来实现
        return flags;
    }

    // 游戏视图（纯游戏渲染）
    static FShowFlags Game()
    {
        FShowFlags flags;
        flags.Lighting = true;
        flags.Shadows = true;
        flags.IBL = true;
        flags.Skybox = true;
        flags.OpaqueObjects = true;
        flags.TransparentObjects = true;
        flags.PostProcessing = true;
        // ❌ 无编辑器工具
        flags.Grid = false;
        flags.DebugLines = false;
        flags.Gizmos = false;
        flags.SelectionOutline = false;
        return flags;
    }

    // 材质/模型预览（简化渲染）
    static FShowFlags Preview()
    {
        FShowFlags flags;
        flags.Lighting = true;
        flags.Shadows = false;          // ❌ 预览不需要阴影
        flags.IBL = false;              // ❌ 预览不需要 IBL
        flags.Skybox = true;
        flags.OpaqueObjects = true;
        flags.TransparentObjects = true;
        flags.PostProcessing = false;   // ❌ 预览不需要后处理
        flags.Grid = false;
        flags.DebugLines = false;
        return flags;
    }

    // Reflection Probe 烘焙（场景内容 + IBL）
    static FShowFlags ReflectionProbe()
    {
        FShowFlags flags;
        flags.Lighting = true;
        flags.Shadows = false;          // ❌ Reflection Probe 不用阴影（性能考虑）
        flags.IBL = true;               // ✅ Reflection Probe 需要 IBL
        flags.Skybox = true;            // ✅ 反射需要天空盒
        flags.OpaqueObjects = true;
        flags.TransparentObjects = true;
        flags.PostProcessing = false;   // ❌ 输出 HDR，不做 Tone Mapping
        // ❌ 无编辑器工具
        flags.Grid = false;
        flags.DebugLines = false;
        flags.Gizmos = false;
        flags.SelectionOutline = false;
        return flags;
    }

    // 线框模式（调试用）
    static FShowFlags WireframeMode()
    {
        FShowFlags flags;
        flags.Lighting = false;
        flags.Shadows = false;
        flags.IBL = false;
        flags.Skybox = false;
        flags.OpaqueObjects = true;
        flags.TransparentObjects = false;
        flags.PostProcessing = false;
        flags.Wireframe = true;         // ✅ 线框模式
        flags.Grid = true;
        return flags;
    }
};
