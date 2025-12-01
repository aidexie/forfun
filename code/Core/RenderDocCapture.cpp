#include "RenderDocCapture.h"
#include "FFLog.h"

CRenderDocCapture::RENDERDOC_API_1_6_0* CRenderDocCapture::s_rdoc_api = nullptr;
void* CRenderDocCapture::s_device = nullptr;
void* CRenderDocCapture::s_wndHandle = nullptr;

bool CRenderDocCapture::Initialize()
{
    if (s_rdoc_api) {
        return true;  // 已经初始化
    }

    // 尝试从 renderdoc.dll 获取 API（如果 RenderDoc 已注入进程）
    HMODULE rdocModule = GetModuleHandleA("renderdoc.dll");
    if (!rdocModule) {
        CFFLog::Info("RenderDoc not detected (renderdoc.dll not loaded)");
        return false;
    }

    // 获取 RenderDoc API 函数
    typedef void* (*pRENDERDOC_GetAPI)(int version, void** outAPIPointers);
    pRENDERDOC_GetAPI getAPI = (pRENDERDOC_GetAPI)GetProcAddress(rdocModule, "RENDERDOC_GetAPI");
    if (!getAPI) {
        CFFLog::Error("Failed to get RENDERDOC_GetAPI");
        return false;
    }

    // 请求 API v1.6.0
    int version = 10600;  // 1.6.0
    void* apiPtr = nullptr;
    int ret = (int)(size_t)getAPI(version, &apiPtr);
    if (ret != 1 || !apiPtr) {
        CFFLog::Error("Failed to get RenderDoc API v1.6.0");
        return false;
    }

    s_rdoc_api = (RENDERDOC_API_1_6_0*)apiPtr;
    CFFLog::Info("RenderDoc API initialized successfully");
    return true;
}

void CRenderDocCapture::BeginFrameCapture()
{
    if (!s_rdoc_api) {
        return;
    }

    if (s_rdoc_api->IsFrameCapturing()) {
        CFFLog::Warning("RenderDoc is already capturing a frame");
        return;
    }

    s_rdoc_api->StartFrameCapture(s_device, s_wndHandle);
    CFFLog::Info("RenderDoc: Started frame capture");
}

void CRenderDocCapture::EndFrameCapture()
{
    if (!s_rdoc_api) {
        return;
    }

    if (!s_rdoc_api->IsFrameCapturing()) {
        CFFLog::Warning("RenderDoc is not capturing");
        return;
    }

    s_rdoc_api->EndFrameCapture(s_device, s_wndHandle);
    CFFLog::Info("RenderDoc: Ended frame capture");
}
