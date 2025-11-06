// main.cpp
#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <d3d11.h>
#include <string>
#include <iostream>
#include <filesystem>

#include "DX11Context.h"  // 单例：仅负责 DX11(设备/上下文/交换链/RTV/DSV)
#include "Renderer.h"     // 仅负责场景渲染(不创建窗口/交换链/不Present)
#include "Console.h"

// ImGui
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "Panels.h"   // Panels::DrawDockspace / DrawHierarchy / DrawInspector / DrawViewport
#include "Scene.h"    // 面板的场景数据结构
#include "Camera.h"   // EditorCamera（Viewport 面板用）

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------------------------------------------
// 全局
// -----------------------------------------------------------------------------
static const wchar_t* kWndClass = L"ForFunEditorWindowClass";
static const wchar_t* kWndTitle = L"ForFunEditor";
static Renderer gRenderer;
static bool gMinimized = false;
static POINT gLastMouse = { 0, 0 };

static Scene gScene;
static EditorCamera gEditorCam;

// -----------------------------------------------------------------------------
// WndProc
// -----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
    {
        if (wParam == SIZE_MINIMIZED) { gMinimized = true; return 0; }
        gMinimized = false;
        const UINT newW = LOWORD(lParam);
        const UINT newH = HIWORD(lParam);
        if (DX11Context::Instance().GetSwapChain()) {
            DX11Context::Instance().OnResize(newW, newH);
            gRenderer.SetSize(newW, newH);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        gRenderer.OnRButton(true);
        SetCapture(hWnd);
        return 0;
    case WM_RBUTTONUP:
        gRenderer.OnRButton(false);
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (wParam & MK_RBUTTON) {
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int dx = p.x - gLastMouse.x;
            int dy = p.y - gLastMouse.y;
            gRenderer.OnMouseDelta(dx, dy);
            gLastMouse = p;
        }
        else {
            gLastMouse.x = GET_X_LPARAM(lParam);
            gLastMouse.y = GET_Y_LPARAM(lParam);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

// -----------------------------------------------------------------------------
// 创建窗口
// -----------------------------------------------------------------------------
static HWND CreateMainWindow(HINSTANCE hInst, int width, int height, WNDPROC proc)
{
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassEx(&wc);

    RECT rc{ 0,0,width,height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(
        0, kWndClass, kWndTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    return hwnd;
}


void ForceWorkDir() {
    const wchar_t* kAssets = L"E:\\forfun\\assets";

    std::wcout << L"exists? " << std::filesystem::exists(kAssets) << L"\n";

    int rc = _wchdir(kAssets);
    if (rc != 0) {
        std::wcout << L"_wchdir rc=" << rc << L" errno=" << errno << L"\n";
        _wperror(L"_wchdir");
    }

    wchar_t buf[512];
    _wgetcwd(buf, 512);
    std::wcout << L"cwd after _wchdir: " << buf << L"\n";

    // 尝试 WinAPI 的方式再改一次，看看是否成功
    if (!SetCurrentDirectoryW(kAssets)) {
        std::wcout << L"SetCurrentDirectoryW failed, GetLastError="
            << GetLastError() << L"\n";
    }
    DWORD n = GetCurrentDirectoryW(512, buf);
    std::wcout << L"cwd after SetCurrentDirectoryW: " << buf << L"\n";
}

// -----------------------------------------------------------------------------
// WinMain
// -----------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int nCmdShow)
{
    Core::Console::InitUTF8();
    ForceWorkDir();

    // 1) 窗口
    const int initW = 1600, initH = 900;
    HWND hwnd = CreateMainWindow(hInstance, initW, initH, WndProc);
    if (!hwnd) return -1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 2) DX11 初始化（仅DX）
    if (!DX11Context::Instance().Initialize(hwnd, initW, initH))
        return -2;

    // 3) ImGui 初始化（在 main.cpp 内）
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(
        DX11Context::Instance().GetDevice(),
        DX11Context::Instance().GetContext()
    );
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable; // 如不需要可去掉

    gScene.selected = -1;                                // 默认无选中
    gEditorCam.aspect = (float)initW / (float)initH;     // 初始宽高比

    // 4) Renderer 初始化（仅拿 device/context/尺寸）
    if (!gRenderer.Initialize(
        DX11Context::Instance().GetDevice(),
        DX11Context::Instance().GetContext(),
        initW, initH))
    {
        return -3;
    }

    // 5) 主循环
    MSG msg{};
    LARGE_INTEGER freq{}, prev{}, curr{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        if (gMinimized) { Sleep(16); continue; }

        // delta time
        QueryPerformanceCounter(&curr);
        float dt = static_cast<float>(double(curr.QuadPart - prev.QuadPart) / double(freq.QuadPart));
        prev = curr;

        // 5.1 ImGui 帧开始
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 5.2 绑定 backbuffer + 清屏
        auto& ctx = DX11Context::Instance();

        // 深度清除交给 Renderer::Render 内部处理（避免与 UI 冲突）

        // 5.3 渲染 3D 场景（bunny）
        //gRenderer.Render(rtv, dsv, dt);
        ImVec2 vpSize = Panels::GetViewportLastSize();
        UINT vpW = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.x : DX11Context::Instance().GetWidth();
        UINT vpH = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.y : DX11Context::Instance().GetHeight();

        // First: render into renderer’s offscreen
        gRenderer.RenderToOffscreen(vpW, vpH, dt);

        ID3D11RenderTargetView* rtv = ctx.GetBackbufferRTV();
        ID3D11DepthStencilView* dsv = ctx.GetDSV();
        ctx.BindRenderTargets(rtv, dsv);
        ctx.SetViewport(0, 0, (float)ctx.GetWidth(), (float)ctx.GetHeight());

        const float clearCol[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
        ctx.ClearRTV(rtv, clearCol);
        //// 5.4 编辑器 UI（示例，可替换为你的面板）
        //ImGui::Begin("ForFunEditor");
        //ImGui::Text("Window: %u x %u", ctx.GetWidth(), ctx.GetHeight());
        //ImGui::Text("dt: %.3f ms (%.1f FPS)", dt * 1000.f, 1.0f / (dt > 0 ? dt : 1e-6f));
        //ImGui::End();
        // main.cpp  —— 每帧渲染里，渲染完 3D 场景之后，提交 ImGui 之前
        {
            bool dockOpen = true;
            Panels::DrawDockspace(&dockOpen);         // DockSpace 容器
            Panels::DrawHierarchy(gScene);            // 层级面板
            Panels::DrawInspector(gScene);            // 检视面板
            Panels::DrawViewport(gScene, gEditorCam,
                gRenderer.GetOffscreenSRV(),
                gRenderer.GetOffscreenWidth(),
                gRenderer.GetOffscreenHeight()); // 视口面板（使用你已有的离屏示例）
        }

        // 5.5 提交 ImGui
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // 5.6 Present
        ctx.Present(1, 0);
    }

    // 6) 清理
    gRenderer.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DX11Context::Instance().Shutdown();

    return (int)msg.wParam;
}
/*
下面准备加入的功能是游戏对象(GameObject)，组件系统(Component)，以及简单的场景管理(World)。
GameObject是场景中的实体，每个GameObject可以包含多个组件(Component)，组件定义了GameObject的行为和属性。
World类负责管理所有的GameObject，提供添加、删除和查找GameObject的功能。
组件负责具体的功能，最开始试用两个组件，一个是Transform组件，负责位置、旋转和缩放信息；另一个是MeshRenderer组件，负责渲染网格。
将上面的功能接入我的引擎代码，并在viewprot中接入三个GameObject，一个是地面平面，一个是简单的立方体网格，一个是Bunny网格。
*/