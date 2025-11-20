// main.cpp
#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <d3d11.h>
#include <direct.h>  // For _chdir, _getcwd
#include <string>
#include <iostream>
#include <filesystem>

#include "DX11Context.h"  // 单例：仅负责 DX11(设备/上下文/交换链/RTV/DSV)
#include "Engine/Rendering/MainPass.h"  // 主渲染流程
#include "Engine/Rendering/ShadowPass.h"  // 阴影渲染流程
#include "Engine/Rendering/IBLGenerator.h"  // IBL生成器
#include "Console.h"

// ImGui
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "Panels.h"   // Panels::DrawDockspace / DrawHierarchy / DrawInspector / DrawViewport
#include "Scene.h"    // 面板的场景数据结构
#include "Camera.h"   // EditorCamera（Viewport 面板用）
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include "SceneSerializer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------------------------------------------
// 全局
// -----------------------------------------------------------------------------
static const wchar_t* kWndClass = L"ForFunEditorWindowClass";
static const wchar_t* kWndTitle = L"ForFunEditor";
static MainPass gMainPass;
static ShadowPass gShadowPass;
static bool gMinimized = false;
static POINT gLastMouse = { 0, 0 };

// Scene is now a singleton - access via Scene::Instance()
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
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        gMainPass.OnRButton(true);
        SetCapture(hWnd);
        return 0;
    case WM_RBUTTONUP:
        gMainPass.OnRButton(false);
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (wParam & MK_RBUTTON) {
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int dx = p.x - gLastMouse.x;
            int dy = p.y - gLastMouse.y;
            gMainPass.OnMouseDelta(dx, dy);
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
    const char* kAssets = "E:\\forfun\\assets";

    std::cout << "exists? " << std::filesystem::exists(kAssets) << "\n";

    int rc = _chdir(kAssets);
    if (rc != 0) {
        std::cout << "_chdir rc=" << rc << " errno=" << errno << "\n";
        perror("_chdir");
    }

    char buf[512];
    _getcwd(buf, 512);
    std::cout << "cwd after _chdir: " << buf << "\n";

    // 尝试 WinAPI 的方式再改一次，看看是否成功
    if (!SetCurrentDirectoryA(kAssets)) {
        std::cout << "SetCurrentDirectoryA failed, GetLastError="
            << GetLastError() << "\n";
    }
    GetCurrentDirectoryA(512, buf);
    std::cout << "cwd after SetCurrentDirectoryA: " << buf << "\n";
}

// -----------------------------------------------------------------------------
// WinMain
// -----------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int nCmdShow)
{
    int exitCode = 0;

    MSG msg{};
    LARGE_INTEGER freq{}, prev{}, curr{};
    // Initialization status flags
    bool dx11Initialized = false;
    bool imguiInitialized = false;
    bool sceneInitialized = false;
    bool mainPassInitialized = false;
    bool shadowPassInitialized = false;

    Core::Console::InitUTF8();
    ForceWorkDir();

    // 1) 窗口
    const int initW = 1600, initH = 900;
    HWND hwnd = CreateMainWindow(hInstance, initW, initH, WndProc);
    if (!hwnd) {
        std::cerr << "ERROR: Failed to create window!" << std::endl;
        exitCode = -1;
        goto cleanup;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 2) DX11 初始化（仅DX）
    if (!DX11Context::Instance().Initialize(hwnd, initW, initH)) {
        std::cerr << "ERROR: Failed to initialize DX11 context!" << std::endl;
        exitCode = -2;
        goto cleanup;
    }
    dx11Initialized = true;

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
    imguiInitialized = true;

    gEditorCam.aspect = (float)initW / (float)initH;     // 初始宽高比

    // 4) Scene initialization (includes Skybox and IBL auto-generation)
    std::cout << "Initializing Scene (loading skybox and generating IBL)..." << std::endl;
    if (!Scene::Instance().Initialize("skybox/afrikaans_church_exterior_1k.hdr", 512))
    {
        std::cerr << "ERROR: Failed to initialize Scene!" << std::endl;
        exitCode = -3;
        goto cleanup;
    }
    sceneInitialized = true;

    // 5) MainPass and ShadowPass initialization
    if (!gMainPass.Initialize())
    {
        std::cerr << "ERROR: Failed to initialize MainPass!" << std::endl;
        exitCode = -4;
        goto cleanup;
    }
    mainPassInitialized = true;

    if (!gShadowPass.Initialize())
    {
        std::cerr << "ERROR: Failed to initialize ShadowPass!" << std::endl;
        exitCode = -5;
        goto cleanup;
    }
    shadowPassInitialized = true;

    // // --- Setup World/GameObjects ---
    // {
    //     // ensure Assets cwd (already attempted earlier)
    //     // Create ground plane
    //     auto* g0 = gScene.world.Create("Ground");
    //     auto* t0 = g0->AddComponent<Transform>();
    //     t0->scale = { 10,0.1,10 };
    //     auto* m0 = g0->AddComponent<MeshRenderer>();
    //     m0->path = "mesh/cube.obj";

    //     // Create cube
    //     auto* g1 = gScene.world.Create("Cube");
    //     auto* t1 = g1->AddComponent<Transform>();
    //     t1->position = { 0,2.5f,0 };
    //     auto* m1 = g1->AddComponent<MeshRenderer>();
    //     m1->path = "mesh/sphere.obj";

    //     // Create bunny (glTF)
    //     auto* g2 = gScene.world.Create("Bunny");
    //     auto* t2 = g2->AddComponent<Transform>();
    //     t2->position = { 1.5f,0,0 };
    //     t2->scale = { 0.8f,0.8f,0.8f };
    //     auto* m2 = g2->AddComponent<MeshRenderer>();
    //     m2->path = "bunny-pbr-gltf/scene_small.gltf";
    //     Scene::Instance().SetSelected(1); // select cube by default
    // }
    {
        SceneSerializer::LoadScene(Scene::Instance(), "E:\\forfun\\assets\\assets\\scenes\\simple.scene");
    }
    // 6) 主循环
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

        // 5.3 渲染 3D 场景
        ImVec2 vpSize = Panels::GetViewportLastSize();
        UINT vpW = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.x : DX11Context::Instance().GetWidth();
        UINT vpH = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.y : DX11Context::Instance().GetHeight();

        // Update camera matrices first (needed for tight frustum fitting)
        gMainPass.UpdateCamera(vpW, vpH, dt);

        // Collect DirectionalLight from scene
        DirectionalLight* dirLight = nullptr;
        for (auto& objPtr : Scene::Instance().GetWorld().Objects()) {
            dirLight = objPtr->GetComponent<DirectionalLight>();
            if (dirLight) break;
        }

        // Shadow Pass (render shadow map if DirectionalLight exists)
        // Uses tight frustum fitting based on camera frustum
        if (dirLight) {
            gShadowPass.Render(Scene::Instance(), dirLight,
                              gMainPass.GetCameraViewMatrix(),
                              gMainPass.GetCameraProjMatrix());
        }

        // Main Pass (use shadow output bundle)
        gMainPass.Render(Scene::Instance(), vpW, vpH, dt, &gShadowPass.GetOutput());

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
            Panels::DrawDockspace(&dockOpen, Scene::Instance(), &gMainPass); // DockSpace 容器
            Panels::DrawHierarchy(Scene::Instance());            // 层级面板
            Panels::DrawInspector(Scene::Instance());            // 检视面板
            Panels::DrawViewport(Scene::Instance(), gEditorCam,
                gMainPass.GetOffscreenSRV(),
                gMainPass.GetOffscreenWidth(),
                gMainPass.GetOffscreenHeight()); // 视口面板（使用你已有的离屏示例）
            Panels::DrawIrradianceDebug();   // IBL debug 窗口（包含 Irradiance/PreFiltered/Environment/BRDF LUT 四个 Tab）
        }

        // 5.5 提交 ImGui
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // 5.6 Present
        ctx.Present(1, 0);
    }

    // Main loop finished normally
    exitCode = (int)msg.wParam;

cleanup:
    // 7) 统一清理出口（按初始化相反顺序清理）
    std::cout << "\n=== Shutting down (exit code: " << exitCode << ") ===" << std::endl;

    if (shadowPassInitialized) {
        std::cout << "Shutting down ShadowPass..." << std::endl;
        gShadowPass.Shutdown();
    }

    if (mainPassInitialized) {
        std::cout << "Shutting down MainPass..." << std::endl;
        gMainPass.Shutdown();
    }

    if (sceneInitialized) {
        std::cout << "Shutting down Scene..." << std::endl;
        Scene::Instance().Shutdown();
    }

    if (imguiInitialized) {
        std::cout << "Shutting down ImGui..." << std::endl;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (dx11Initialized) {
        std::cout << "Shutting down DX11..." << std::endl;
        DX11Context::Instance().Shutdown();
    }

    std::cout << "Shutdown complete." << std::endl;
    return exitCode;
}