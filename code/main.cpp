// main.cpp
#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <d3d11.h>
#include <d3d12.h>
#include <direct.h>  // For _chdir, _getcwd
#include <string>
#include <iostream>
#include <filesystem>
#include <algorithm>  // For std::transform (fuzzy matching)

#include "RHI/RHIManager.h"  // RHI 全局管理器
#include "RHI/RHIHelpers.h"  // RHI helper functions
#include "RHI/DX12/DX12Context.h"  // DX12 Context for ImGui
#include "RHI/DX12/DX12Common.h"   // NUM_FRAMES_IN_FLIGHT
#include "Engine/Rendering/ForwardRenderPipeline.h"  // ✅ Forward 渲染流程
#include "Engine/Rendering/ShowFlags.h"  // ✅ 渲染标志
#include "Engine/Rendering/IBLGenerator.h"  // IBL生成器
#include "Engine/Rendering/DebugRenderSystem.h"  // Debug 几何渲染
#include "Core/RenderDocCapture.h"  // RenderDoc API
#include "Core/RenderConfig.h"  // ✅ Render configuration
#include "Console.h"

// Test framework
#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "RHI/ICommandList.h"  // For RHI::CScopedDebugEvent

// ImGui
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "ImGuizmo.h"

#include "Panels.h"   // Panels::DrawDockspace / DrawHierarchy / DrawInspector / DrawViewport
#include "Scene.h"    // 面板的场景数据结构
#include "Camera.h"   // CCamera（Viewport 面板用）
#include "EditorContext.h"  // ✅ 编辑器交互管理（相机控制）
#include "Core/TextureManager.h"  // Texture cache manager
#include "Components/DirectionalLight.h"
#include "DebugPaths.h"  // Debug output directories
#include "FFLog.h"  // Logging system
#include "PathManager.h"  // Unified path management

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------------------------------------------
// 全局
// -----------------------------------------------------------------------------
static const wchar_t* kWndClass = L"ForFunEditorWindowClass";
static const wchar_t* kWndTitle = L"ForFunEditor";
static CForwardRenderPipeline g_pipeline;  // ✅ Forward 渲染 Pipeline
static bool g_minimized = false;
static POINT g_last_mouse = { 0, 0 };
static SRenderConfig g_renderConfig;  // ✅ Global render configuration

// DX12 viewport texture tracking
static uint64_t g_dx12ViewportGpuHandle = 0;  // ImGui texture ID for DX12 viewport
static uint32_t g_dx12ViewportSlot = 0;       // Allocated slot in ImGui SRV heap
static unsigned int g_dx12ViewportLastWidth = 0;
static unsigned int g_dx12ViewportLastHeight = 0;

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
        RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
        if (rhiCtx && rhiCtx->GetWidth() > 0) {
            rhiCtx->OnResize(newW, newH);
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
    HWND hwnd = nullptr;

    MSG msg{};
    LARGE_INTEGER freq{}, prev{}, curr{};
    int frameCount = 0;
    // Initialization status flags
    bool dx11Initialized = false;
    bool imguiInitialized = false;
    bool sceneInitialized = false;
    bool pipelineInitialized = false;
    bool defaultSceneLoaded = false;

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

    // 1) FFPath initialization (must be first - config paths depend on it)
    FFPath::Initialize("E:/forfun");

    // 1.5) Initialize logging (clears old log file)
    CFFLog::Initialize();

    // 2) Load render configuration
    {
        std::string configPath = SRenderConfig::GetDefaultPath();
        if (!SRenderConfig::Load(configPath, g_renderConfig)) {
            // Config not found - save defaults for user reference
            CFFLog::Info("[Main] Creating default config at %s", configPath.c_str());
            // Ensure config directory exists
            std::filesystem::create_directories(std::filesystem::path(configPath).parent_path());
            SRenderConfig::Save(configPath, g_renderConfig);
        }
    }

    // 3) 窗口 (use config dimensions)
    {
        int initW = static_cast<int>(g_renderConfig.windowWidth);
        int initH = static_cast<int>(g_renderConfig.windowHeight);
        hwnd = CreateMainWindow(hInstance, initW, initH, WndProc);
        if (!hwnd) {
            CFFLog::Error("Failed to create window!");
            exitCode = -1;
            goto cleanup;
        }
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
        CFFLog::Info("Window created: %dx%d", initW, initH);
    }

    // 4) RHI Manager 初始化 (use config backend)
    {
        const char* backendName = (g_renderConfig.backend == RHI::EBackend::DX12) ? "DX12" : "DX11";
        CFFLog::Info("[Main] Initializing RHI with %s backend...", backendName);

        if (!RHI::CRHIManager::Instance().Initialize(
                g_renderConfig.backend,
                hwnd,
                g_renderConfig.windowWidth,
                g_renderConfig.windowHeight)) {
            CFFLog::Error("Failed to initialize RHI Manager!");
            exitCode = -2;
            goto cleanup;
        }
        dx11Initialized = true;  // Renamed variable still tracks RHI init
        CFFLog::Info("RHI Manager initialized (%s backend)", backendName);
    }

    // 5) ImGui 初始化（根据 backend 选择）
    {
        RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(hwnd);

        if (g_renderConfig.backend == RHI::EBackend::DX12) {
            // DX12 backend - use new InitInfo struct
            auto& dx12Ctx = RHI::DX12::CDX12Context::Instance();

            ImGui_ImplDX12_InitInfo initInfo = {};
            initInfo.Device = dx12Ctx.GetDevice();
            initInfo.CommandQueue = dx12Ctx.GetCommandQueue();  // Required for texture uploads
            initInfo.NumFramesInFlight = RHI::DX12::NUM_FRAMES_IN_FLIGHT;
            initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            initInfo.SrvDescriptorHeap = dx12Ctx.GetImGuiSrvHeap();
            initInfo.LegacySingleSrvCpuDescriptor = dx12Ctx.GetImGuiSrvCpuHandle();
            initInfo.LegacySingleSrvGpuDescriptor = dx12Ctx.GetImGuiSrvGpuHandle();

            if (!ImGui_ImplDX12_Init(&initInfo)) {
                CFFLog::Error("[Main] ImGui_ImplDX12_Init failed");
                exitCode = -3;
                goto cleanup;
            }

            CFFLog::Info("[Main] ImGui DX12 backend initialized");
        } else {
            // DX11 backend
            ImGui_ImplDX11_Init(
                static_cast<ID3D11Device*>(rhiCtx->GetNativeDevice()),
                static_cast<ID3D11DeviceContext*>(rhiCtx->GetNativeContext())
            );
        }
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }
    imguiInitialized = true;
    CFFLog::Info("ImGui initialized");

    // ✅ 初始化编辑器相机的宽高比
    CScene::Instance().GetEditorCamera().aspectRatio =
        static_cast<float>(g_renderConfig.windowWidth) / static_cast<float>(g_renderConfig.windowHeight);


    // 6) CScene GPU resource initialization
    // NOTE: For DX12, initialization is deferred to main loop (after command list is open)
    // For DX11, initialize here
    if (g_renderConfig.backend == RHI::EBackend::DX11) {
        if (!CScene::Instance().Initialize()) {
            CFFLog::Error("Failed to initialize CScene!");
            exitCode = -4;
            goto cleanup;
        }
        sceneInitialized = true;
    }

    // 6) ✅ ForwardRenderPipeline initialization
    // NOTE: For DX12, initialization is deferred to main loop (after command list is open)
    // For DX11, initialize here
    if (g_renderConfig.backend == RHI::EBackend::DX11) {
        if (!g_pipeline.Initialize()) {
            CFFLog::Error("Failed to initialize ForwardRenderPipeline!");
            exitCode = -5;
            goto cleanup;
        }
        pipelineInitialized = true;
        CFFLog::Info("ForwardRenderPipeline initialized");
    };

    // 7) Load default scene (if not in test mode)
    // NOTE: For DX12, scene loading is deferred to main loop (after command list is open)
    // For DX11, load here
    if (g_renderConfig.backend == RHI::EBackend::DX11) {
        if (!activeTest) {
            //std::string scenePath = FFPath::GetAbsolutePath("scenes/simple_test_dx12.scene");
            std::string scenePath = FFPath::GetAbsolutePath("scenes/simple_step_by_step.scene");
            CScene::Instance().LoadFromFile(scenePath);
            defaultSceneLoaded = true;
        }
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

        RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();

        // ============================================
        // DX12 Loop: Using RHI Layer
        // ============================================
        if (g_renderConfig.backend == RHI::EBackend::DX12) {
            auto& dx12Ctx = RHI::DX12::CDX12Context::Instance();

            // 1. RHI BeginFrame (resets command list, transitions backbuffer)
            rhiCtx->BeginFrame();

            // 1.5 Deferred initialization (must be after command list is open)
            if (!sceneInitialized) {
                if (!CScene::Instance().Initialize()) {
                    CFFLog::Error("Failed to initialize CScene!");
                    PostQuitMessage(-4);
                    break;
                }
                sceneInitialized = true;
            }

            if (!pipelineInitialized) {
                if (!g_pipeline.Initialize()) {
                    CFFLog::Error("Failed to initialize ForwardRenderPipeline!");
                    PostQuitMessage(-5);
                    break;
                }
                pipelineInitialized = true;
                CFFLog::Info("ForwardRenderPipeline initialized");
            }

            // Load default scene (deferred for DX12)
            if (!defaultSceneLoaded && !activeTest) {
                std::string scenePath = FFPath::GetAbsolutePath("scenes/simple_test_dx12.scene");
                CScene::Instance().LoadFromFile(scenePath);
                defaultSceneLoaded = true;
            }

            // 2. Get RHI CommandList
            RHI::ICommandList* cmdList = rhiCtx->GetCommandList();

            // ============================================
            // Phase 6.1: Test Offscreen RT creation + RHI clear
            // ============================================
            {
                // Get viewport size (same logic as DX11)
                ImVec2 vpSize = Panels::GetViewportLastSize();
                UINT vpW = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.x : rhiCtx->GetWidth();
                UINT vpH = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.y : rhiCtx->GetHeight();

                // Update editor camera
                CCamera& editorCamera = CScene::Instance().GetEditorCamera();
                float aspect = (vpH > 0) ? (float)vpW / (float)vpH : 1.0f;
                editorCamera.aspectRatio = aspect;
                CEditorContext::Instance().Update(dt, editorCamera);

                // Render 3D scene through pipeline
                g_pipeline.GetDebugLinePass().BeginFrame();
                CDebugRenderSystem::Instance().CollectAndRender(CScene::Instance(), g_pipeline.GetDebugLinePass());

                {
                    RHI::CScopedDebugEvent evtScene(cmdList, L"Forward Pipeline");
                    CRenderPipeline::RenderContext renderCtx{
                        editorCamera, CScene::Instance(), vpW, vpH, dt, FShowFlags::Editor()
                    };
                    g_pipeline.Render(renderCtx);
                }
            }

            // 3. Bind backbuffer for UI rendering
            {
                RHI::ITexture* backbuffer = rhiCtx->GetBackbuffer();
                RHI::ITexture* depthStencil = rhiCtx->GetDepthStencil();
                cmdList->SetRenderTargets(1, &backbuffer, depthStencil);
                cmdList->SetViewport(0, 0, (float)rhiCtx->GetWidth(), (float)rhiCtx->GetHeight());
                cmdList->SetScissorRect(0, 0, rhiCtx->GetWidth(), rhiCtx->GetHeight());

                const float clearColor[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
                cmdList->ClearRenderTarget(backbuffer, clearColor);
                if (depthStencil) {
                    cmdList->ClearDepthStencil(depthStencil, true, 1.0f, true, 0);
                }
            }

            // 4. ImGui NewFrame
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGuizmo::BeginFrame();

            // 5. ImGui Panels
            {
                bool dockOpen = true;
                Panels::DrawDockspace(&dockOpen, CScene::Instance(), &g_pipeline);
                Panels::DrawHierarchy(CScene::Instance());
                Panels::DrawInspector(CScene::Instance());

                // DX12 Viewport: Need to allocate ImGui descriptor for offscreen texture
                void* viewportSrv = nullptr;
                unsigned int vpW = g_pipeline.GetOffscreenWidth();
                unsigned int vpH = g_pipeline.GetOffscreenHeight();

                if (vpW > 0 && vpH > 0) {
                    RHI::ITexture* ldrTexture = g_pipeline.GetOffscreenTextureRHI();
                    if (ldrTexture) {
                        ID3D12Resource* d3dResource = static_cast<ID3D12Resource*>(ldrTexture->GetNativeHandle());

                        // Allocate slot only once, then update in place
                        if (g_dx12ViewportSlot == 0) {
                            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = dx12Ctx.AllocateImGuiTextureDescriptor(
                                d3dResource, DXGI_FORMAT_R8G8B8A8_UNORM);
                            g_dx12ViewportGpuHandle = gpuHandle.ptr;
                            // Calculate slot from handle (slot 1 is first after font texture)
                            uint64_t basePtr = dx12Ctx.GetImGuiSrvGpuHandle().ptr;
                            g_dx12ViewportSlot = static_cast<uint32_t>((gpuHandle.ptr - basePtr) / dx12Ctx.GetSrvDescriptorSize());
                            g_dx12ViewportLastWidth = vpW;
                            g_dx12ViewportLastHeight = vpH;
                        }
                        // Update descriptor if texture changed (resize recreates texture)
                        else if (vpW != g_dx12ViewportLastWidth || vpH != g_dx12ViewportLastHeight) {
                            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = dx12Ctx.UpdateImGuiTextureDescriptor(
                                g_dx12ViewportSlot, d3dResource, DXGI_FORMAT_R8G8B8A8_UNORM);
                            g_dx12ViewportGpuHandle = gpuHandle.ptr;
                            g_dx12ViewportLastWidth = vpW;
                            g_dx12ViewportLastHeight = vpH;
                        }
                    }
                    viewportSrv = (void*)g_dx12ViewportGpuHandle;
                }

                Panels::DrawViewport(CScene::Instance(), CScene::Instance().GetEditorCamera(),
                   viewportSrv, vpW, vpH, &g_pipeline);
                Panels::DrawIrradianceDebug();
                Panels::DrawHDRExportWindow();
                Panels::DrawSceneLightSettings(&g_pipeline);
                Panels::DrawMaterialEditor();
            }

            // 6. ImGui render
            {
                RHI::CScopedDebugEvent evtImGui(cmdList, L"ImGui Pass");
                ImGui::Render();

                // Set descriptor heap for ImGui (required before RenderDrawData)
                ID3D12GraphicsCommandList* d3dCmdList = static_cast<ID3D12GraphicsCommandList*>(rhiCtx->GetNativeContext());
                ID3D12DescriptorHeap* heaps[] = { dx12Ctx.GetImGuiSrvHeap() };
                d3dCmdList->SetDescriptorHeaps(1, heaps);

                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), d3dCmdList);
            }

            // 7. EndFrame and Present
            rhiCtx->EndFrame();
            rhiCtx->Present(true);
        }
        // ============================================
        // DX11 Full Loop (unchanged)
        // ============================================
        else {
            // 5.1 RHI BeginFrame
            rhiCtx->BeginFrame();

            // 5.2 ImGui 帧开始
            ImGui_ImplWin32_NewFrame();
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            ImGuizmo::BeginFrame();

            // 5.3 渲染 3D 场景
            ImVec2 vpSize = Panels::GetViewportLastSize();
            UINT vpW = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.x : rhiCtx->GetWidth();
            UINT vpH = (vpSize.x > 1.f && vpSize.y > 1.f) ? (UINT)vpSize.y : rhiCtx->GetHeight();

            CCamera& editorCamera = CScene::Instance().GetEditorCamera();
            float aspect = (vpH > 0) ? (float)vpW / (float)vpH : 1.0f;
            editorCamera.aspectRatio = aspect;
            CEditorContext::Instance().Update(dt, editorCamera);

            g_pipeline.GetDebugLinePass().BeginFrame();
            CDebugRenderSystem::Instance().CollectAndRender(CScene::Instance(), g_pipeline.GetDebugLinePass());

            {RHI::CScopedDebugEvent evtScene(rhiCtx->GetCommandList(), L"forward pipeline");
            CRenderPipeline::RenderContext renderCtx{
                editorCamera, CScene::Instance(), vpW, vpH, dt, FShowFlags::Editor()
            };
            g_pipeline.Render(renderCtx);
            }

            // Bind backbuffer for UI rendering
            ID3D11DeviceContext* d3dCtx = static_cast<ID3D11DeviceContext*>(rhiCtx->GetNativeContext());
            ID3D11RenderTargetView* rtv = static_cast<ID3D11RenderTargetView*>(RHI::GetNativeRTV(rhiCtx->GetBackbuffer()));
            ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(RHI::GetNativeDSV(rhiCtx->GetDepthStencil()));
            d3dCtx->OMSetRenderTargets(1, &rtv, dsv);

            D3D11_VIEWPORT vp{};
            vp.Width = (float)rhiCtx->GetWidth();
            vp.Height = (float)rhiCtx->GetHeight();
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            d3dCtx->RSSetViewports(1, &vp);

            const float clearCol[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
            d3dCtx->ClearRenderTargetView(rtv, clearCol);

            // ImGui panels
            {
                bool dockOpen = true;
                Panels::DrawDockspace(&dockOpen, CScene::Instance(), &g_pipeline);
                Panels::DrawHierarchy(CScene::Instance());
                Panels::DrawInspector(CScene::Instance());
                Panels::DrawViewport(CScene::Instance(), CScene::Instance().GetEditorCamera(),
                   g_pipeline.GetOffscreenSRV(), g_pipeline.GetOffscreenWidth(),
                   g_pipeline.GetOffscreenHeight(), &g_pipeline);
                Panels::DrawIrradianceDebug();
                Panels::DrawHDRExportWindow();
                Panels::DrawSceneLightSettings(&g_pipeline);
                Panels::DrawMaterialEditor();
            }

            // 5.5 提交 ImGui
            {RHI::CScopedDebugEvent evtScene(rhiCtx->GetCommandList(), L"imgui pass");
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }

            // 5.6 EndFrame and Present
            rhiCtx->EndFrame();
            rhiCtx->Present(true);
        }
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
        if (g_renderConfig.backend == RHI::EBackend::DX12) {
            ImGui_ImplDX12_Shutdown();
        } else {
            ImGui_ImplDX11_Shutdown();
        }
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    // Shutdown singleton managers before RHI (they hold GPU resources)
    CFFLog::Info("Shutting down TextureManager...");
    CTextureManager::Instance().Shutdown();

    if (dx11Initialized) {
        CFFLog::Info("Shutting down RHI...");
        RHI::CRHIManager::Instance().Shutdown();
    }

    CFFLog::Info("Shutdown complete.");
    return exitCode;
}