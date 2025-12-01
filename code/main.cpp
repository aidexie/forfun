// main.cpp
#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <d3d11.h>
#include <direct.h>  // For _chdir, _getcwd
#include <string>
#include <iostream>
#include <filesystem>
#include <algorithm>  // For std::transform (fuzzy matching)

#include "DX11Context.h"  // 单例：仅负责 DX11(设备/上下文/交换链/RTV/DSV)
#include "Engine/Rendering/ForwardRenderPipeline.h"  // ✅ Forward 渲染流程
#include "Engine/Rendering/ShowFlags.h"  // ✅ 渲染标志
#include "Engine/Rendering/IBLGenerator.h"  // IBL生成器
#include "Engine/Rendering/DebugRenderSystem.h"  // Debug 几何渲染
#include "Core/RenderDocCapture.h"  // RenderDoc API
#include "Console.h"

// Test framework
#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/DebugEvent.h"

// ImGui
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "ImGuizmo.h"

#include "Panels.h"   // Panels::DrawDockspace / DrawHierarchy / DrawInspector / DrawViewport
#include "Scene.h"    // 面板的场景数据结构
#include "Camera.h"   // CCamera（Viewport 面板用）
#include "EditorContext.h"  // ✅ 编辑器交互管理（相机控制）
#include "Components/DirectionalLight.h"
#include "SceneSerializer.h"
#include "DebugPaths.h"  // Debug output directories
#include "FFLog.h"  // Logging system

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------------------------------------------
// 全局
// -----------------------------------------------------------------------------
static const wchar_t* kWndClass = L"ForFunEditorWindowClass";
static const wchar_t* kWndTitle = L"ForFunEditor";
static CForwardRenderPipeline g_pipeline;  // ✅ Forward 渲染 Pipeline
static bool g_minimized = false;
static POINT g_last_mouse = { 0, 0 };

// ✅ Scene 现在管理 EditorCamera（通过 CScene::Instance().GetEditorCamera()）
// ✅ EditorContext 现在管理相机交互（通过 CEditorContext::Instance()）

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
        if (wParam == SIZE_MINIMIZED) { g_minimized = true; return 0; }
        g_minimized = false;
        const UINT newW = LOWORD(lParam);
        const UINT newH = HIWORD(lParam);
        if (CDX11Context::Instance().GetSwapChain()) {
            CDX11Context::Instance().OnResize(newW, newH);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        CEditorContext::Instance().OnRButton(true);  // ✅ 使用 EditorContext
        SetCapture(hWnd);
        return 0;
    case WM_RBUTTONUP:
        CEditorContext::Instance().OnRButton(false);  // ✅ 使用 EditorContext
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (wParam & MK_RBUTTON) {
            // Only update camera if not using ImGuizmo
            if (!ImGuizmo::IsUsing()) {
                POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                int dx = p.x - g_last_mouse.x;
                int dy = p.y - g_last_mouse.y;
                // ✅ 使用 EditorContext，传入 Scene 的相机
                CEditorContext::Instance().OnMouseDelta(dx, dy, CScene::Instance().GetEditorCamera());
                g_last_mouse = p;
            }
        }
        else {
            g_last_mouse.x = GET_X_LPARAM(lParam);
            g_last_mouse.y = GET_Y_LPARAM(lParam);
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

    CFFLog::Info("Asset directory exists: %d", std::filesystem::exists(kAssets));

    int rc = _chdir(kAssets);
    if (rc != 0) {
        CFFLog::Warning("_chdir failed: rc=%d, errno=%d", rc, errno);
        perror("_chdir");
    }

    char buf[512];
    _getcwd(buf, 512);
    CFFLog::Info("Current working directory after _chdir: %s", buf);

    // 尝试 WinAPI 的方式再改一次，看看是否成功
    if (!SetCurrentDirectoryA(kAssets)) {
        CFFLog::Warning("SetCurrentDirectoryA failed: GetLastError=%lu", GetLastError());
    }
    GetCurrentDirectoryA(512, buf);
    CFFLog::Info("Current working directory after SetCurrentDirectoryA: %s", buf);
}

// -----------------------------------------------------------------------------
// List all available tests
// -----------------------------------------------------------------------------
static void ListAllTests() {
    auto testNames = CTestRegistry::Instance().GetAllTestNames();
    CFFLog::Info("=== Available Tests ===");
    if (testNames.empty()) {
        CFFLog::Warning("No tests registered!");
    } else {
        CFFLog::Info("Total: %zu test(s)", testNames.size());
        for (const auto& name : testNames) {
            CFFLog::Info("  - %s", name.c_str());
        }
    }
    CFFLog::Info("=======================");
    CFFLog::Info("Usage: forfun.exe --test <TestName>");
}

// -----------------------------------------------------------------------------
// Parse command line for test mode
// -----------------------------------------------------------------------------
static ITestCase* ParseCommandLineForTest(LPWSTR lpCmdLine) {
    std::wstring cmdLine(lpCmdLine);
    if (cmdLine.find(L"--test") == std::wstring::npos) {
        return nullptr;  // Not in test mode
    }

    // Extract test name
    size_t pos = cmdLine.find(L"--test");
    size_t start = pos + 7;  // Skip "--test "
    size_t end = cmdLine.find(L' ', start);
    if (end == std::wstring::npos) end = cmdLine.length();

    std::wstring testNameW = cmdLine.substr(start, end - start);

    // ✅ 正确的 wstring → string 转换（使用 Windows API）
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, testNameW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string testName(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, testNameW.c_str(), -1, &testName[0], sizeNeeded, nullptr, nullptr);
    testName.resize(sizeNeeded - 1); // Remove null terminator

    // Remove leading/trailing spaces
    testName.erase(0, testName.find_first_not_of(" \t\n\r"));
    testName.erase(testName.find_last_not_of(" \t\n\r") + 1);

    ITestCase* test = CTestRegistry::Instance().Get(testName);
    if (test) {
        CFFLog::Info("=== Starting Test: %s ===", test->GetName());
        return test;
    }

    // Test not found, provide suggestions
    CFFLog::Error("Test not found: %s", testName.c_str());

    // Try fuzzy matching for suggestions
    auto testNames = CTestRegistry::Instance().GetAllTestNames();
    std::vector<std::string> suggestions;
    for (const auto& name : testNames) {
        // Check if registered name contains the input or vice versa
        std::string lowerName = name;
        std::string lowerInput = testName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(), ::tolower);

        if (lowerName.find(lowerInput) != std::string::npos ||
            lowerInput.find(lowerName) != std::string::npos) {
            suggestions.push_back(name);
        }
    }

    if (!suggestions.empty()) {
        CFFLog::Info("Did you mean:");
        for (const auto& s : suggestions) {
            CFFLog::Info("  - %s", s.c_str());
        }
    }

    CFFLog::Info("Available tests:");
    for (const auto& name : testNames) {
        CFFLog::Info("  - %s", name.c_str());
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// WinMain
// -----------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    int exitCode = 0;

    MSG msg{};
    LARGE_INTEGER freq{}, prev{}, curr{};
    int frameCount = 0;
    // Initialization status flags
    bool dx11Initialized = false;
    bool imguiInitialized = false;
    bool sceneInitialized = false;
    bool pipelineInitialized = false;

    Core::Console::InitUTF8();
    ForceWorkDir();

    // Initialize RenderDoc API (if RenderDoc is attached)
    CRenderDocCapture::Initialize();

    // Check for --list-tests command
    std::wstring cmdLine(lpCmdLine);
    if (cmdLine.find(L"--list-tests") != std::wstring::npos) {
        ListAllTests();
        return 0;
    }

    // Parse command line for test mode
    ITestCase* activeTest = ParseCommandLineForTest(lpCmdLine);
    CTestContext testContext;
    // Setup test if in test mode
    if (activeTest) {
        testContext.pipeline = &g_pipeline;  // Give test access to ForwardRenderPipeline for screenshots
        testContext.testName = activeTest->GetName();  // Set test name for detailed logging

        // Set test-specific runtime log path
        std::string runtimeLogPath = GetTestDebugDir(activeTest->GetName()) + "/runtime.log";
        CFFLog::SetRuntimeLogPath(runtimeLogPath.c_str());
        CFFLog::Info("Test mode: runtime log redirected to %s", runtimeLogPath.c_str());

        activeTest->Setup(testContext);
        CFFLog::Info("Test setup complete, starting main loop");
    }
    // 0) Debug directories (ensure they exist for logging)
    CDebugPaths::EnsureDirectoriesExist();

    // 1) 窗口
    const int initW = 1600, initH = 900;
    HWND hwnd = CreateMainWindow(hInstance, initW, initH, WndProc);
    if (!hwnd) {
        CFFLog::Error("Failed to create window!");
        exitCode = -1;
        goto cleanup;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    CFFLog::Info("Window created: %dx%d", initW, initH);

    // 2) DX11 初始化（仅DX）
    if (!CDX11Context::Instance().Initialize(hwnd, initW, initH)) {
        CFFLog::Error("Failed to initialize DX11 context!");
        exitCode = -2;
        goto cleanup;
    }
    dx11Initialized = true;
    CFFLog::Info("DX11 context initialized");

    // 3) ImGui 初始化（在 main.cpp 内）
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(
        CDX11Context::Instance().GetDevice(),
        CDX11Context::Instance().GetContext()
    );
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable; // 如不需要可去掉
    imguiInitialized = true;
    CFFLog::Info("ImGui initialized");

    // ✅ 初始化编辑器相机的宽高比
    CScene::Instance().GetEditorCamera().aspectRatio = (float)initW / (float)initH;


    // 5) ✅ ForwardRenderPipeline initialization
    if (!g_pipeline.Initialize())
    {
        CFFLog::Error("Failed to initialize ForwardRenderPipeline!");
        exitCode = -4;
        goto cleanup;
    }
    pipelineInitialized = true;
    CFFLog::Info("ForwardRenderPipeline initialized");

    if (!activeTest)
    {
        CSceneSerializer::LoadScene(CScene::Instance(), "E:\\forfun\\assets\\assets\\scenes\\simple.scene");
        sceneInitialized = true;
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
        if (g_minimized) { Sleep(16); continue; }

        frameCount++;

        // delta time
        QueryPerformanceCounter(&curr);
        float dt = static_cast<float>(double(curr.QuadPart - prev.QuadPart) / double(freq.QuadPart));
        prev = curr;

        // Execute test frame if in test mode
        if (activeTest) {
            testContext.ExecuteFrame(frameCount);

            // Check if test is finished
            if (testContext.IsFinished()) {
                CFFLog::Info("=== Test Finished ===");
                PostQuitMessage(testContext.testPassed ? 0 : 1);
                break;
            }

            // Timeout protection
            if (frameCount > 1000) {
                CFFLog::Error("Test timeout after 1000 frames");
                PostQuitMessage(1);
                break;
            }
        }

        // 5.1 ImGui 帧开始
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        // 5.2 绑定 backbuffer + 清屏
        auto& ctx = CDX11Context::Instance();

        // 深度清除交给 Renderer::Render 内部处理（避免与 UI 冲突）

        // 5.3 渲染 3D 场景
        ImVec2 vpSize = Panels::GetViewportLastSize();
        UINT vpW = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.x : CDX11Context::Instance().GetWidth();
        UINT vpH = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.y : CDX11Context::Instance().GetHeight();

        // ✅ 更新相机（aspect ratio + WASD 移动）
        CCamera& editorCamera = CScene::Instance().GetEditorCamera();
        float aspect = (vpH > 0) ? (float)vpW / (float)vpH : 1.0f;
        editorCamera.aspectRatio = aspect;
        CEditorContext::Instance().Update(dt, editorCamera);

        // Clear debug line buffer at start of frame
        g_pipeline.GetDebugLinePass().BeginFrame();

        // Collect and render debug geometry (AABB, rays, gizmos, etc.)
        CDebugRenderSystem::Instance().CollectAndRender(CScene::Instance(), g_pipeline.GetDebugLinePass());

        // Forward Rendering Pipeline
        {CScopedDebugEvent evtScene(ctx.GetContext(), L"forward pipeline");
        // Setup RenderContext for editor viewport
        CRenderPipeline::RenderContext renderCtx{
            editorCamera,               // camera
            CScene::Instance(),         // scene
            vpW,                        // width
            vpH,                        // height
            dt,                         // deltaTime
            FShowFlags::Editor()        // showFlags (full editor features)
            // finalOutputRTV = nullptr (use pipeline's internal buffers)
            // outputFormat = LDR (default, for editor display)
        };
        g_pipeline.Render(renderCtx);
        }

        ID3D11RenderTargetView* rtv = ctx.GetBackbufferRTV();
        ID3D11DepthStencilView* dsv = ctx.GetDSV();
        ctx.BindRenderTargets(rtv, dsv);
        ctx.SetViewport(0, 0, (float)ctx.GetWidth(), (float)ctx.GetHeight());

        const float clearCol[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
        ctx.ClearRTV(rtv, clearCol);
        // main.cpp  —— 每帧渲染里，渲染完 3D 场景之后，提交 ImGui 之前
        {
            bool dockOpen = true;
            Panels::DrawDockspace(&dockOpen, CScene::Instance(), &g_pipeline); // DockSpace 容器
            Panels::DrawHierarchy(CScene::Instance());            // 层级面板
            Panels::DrawInspector(CScene::Instance());            // 检视面板
            // ✅ 传入 Scene 的编辑器相机
            Panels::DrawViewport(CScene::Instance(), CScene::Instance().GetEditorCamera(),
                g_pipeline.GetOffscreenSRV(),
                g_pipeline.GetOffscreenWidth(),
                g_pipeline.GetOffscreenHeight(),
                &g_pipeline); // 视口面板（使用你已有的离屏示例）
            Panels::DrawIrradianceDebug();   // IBL debug 窗口（包含 Irradiance/PreFiltered/Environment/BRDF LUT 四个 Tab）
            Panels::DrawHDRExportWindow();   // HDR Export 窗口
            Panels::DrawSceneLightSettings(&g_pipeline);  // Scene Light Settings 窗口
            Panels::DrawMaterialEditor();    // Material Editor 窗口
        }

        // 5.5 提交 ImGui
        {CScopedDebugEvent evtScene(ctx.GetContext(), L"imgui pass");
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        // 5.6 Present
        ctx.Present(1, 0);
    }

    // Main loop finished normally
    exitCode = (int)msg.wParam;

cleanup:
    // 7) 统一清理出口（按初始化相反顺序清理）
    CFFLog::Info("=== Shutting down (exit code: %d) ===", exitCode);

    if (pipelineInitialized) {
        CFFLog::Info("Shutting down ForwardRenderPipeline...");
        g_pipeline.Shutdown();
    }

    if (sceneInitialized) {
        CFFLog::Info("Shutting down Scene...");
        CScene::Instance().Shutdown();
    }

    if (imguiInitialized) {
        CFFLog::Info("Shutting down ImGui...");
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (dx11Initialized) {
        CFFLog::Info("Shutting down DX11...");
        CDX11Context::Instance().Shutdown();
    }

    CFFLog::Info("Shutdown complete.");
    return exitCode;
}