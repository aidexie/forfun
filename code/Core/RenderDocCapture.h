#pragma once
#include <windows.h>

// RenderDoc API 集成
// 用于在代码中触发帧捕获
class CRenderDocCapture
{
public:
    // 初始化 RenderDoc API（如果 RenderDoc 已注入）
    static bool Initialize();

    // 开始捕获下一帧
    static void BeginFrameCapture();

    // 结束捕获当前帧
    static void EndFrameCapture();

    // 检查 RenderDoc 是否可用
    static bool IsAvailable() { return s_rdoc_api != nullptr; }

    // RAII wrapper：自动捕获作用域内的渲染
    class ScopedCapture {
    public:
        ScopedCapture() { BeginFrameCapture(); }
        ~ScopedCapture() { EndFrameCapture(); }
    };

private:
    // RenderDoc API 函数指针类型
    typedef void* (*pRENDERDOC_GetAPI)(int version, void** outAPIPointers);

    // RenderDoc API v1.6.0 接口
    struct RENDERDOC_API_1_6_0
    {
        void* GetAPIVersion;
        void* SetCaptureOptionU32;
        void* SetCaptureOptionF32;
        void* GetCaptureOptionU32;
        void* GetCaptureOptionF32;
        void* SetFocusToggleKeys;
        void* SetCaptureKeys;
        void* GetOverlayBits;
        void* MaskOverlayBits;
        void* RemoveHooks;
        void* UnloadCrashHandler;
        void* SetCaptureFilePathTemplate;
        void* GetCaptureFilePathTemplate;
        void* GetNumCaptures;
        void* GetCapture;
        void (*TriggerCapture)();  // 我们需要这个
        int  (*IsTargetControlConnected)();
        void (*LaunchReplayUI)(unsigned int connectTargetControl, const char* cmdline);
        void (*SetActiveWindow)(void* device, void* wndHandle);
        void (*StartFrameCapture)(void* device, void* wndHandle);  // 我们需要这个
        int  (*IsFrameCapturing)();
        void (*EndFrameCapture)(void* device, void* wndHandle);    // 我们需要这个
        // ... 其他函数省略
    };

    static RENDERDOC_API_1_6_0* s_rdoc_api;
    static void* s_device;
    static void* s_wndHandle;
};
