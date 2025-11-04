
#include <Windows.h>
#include <vector>
#include <chrono>
#include "Render.h"
#include "Update.h"
#include "Camera.h"
#include "Console.h"
#include <iostream>
#include <filesystem>
static Render  gRender;
static Update  gUpdate;
static Camera  gCamera;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

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

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int n)
{
    // 可选：设置工作目录到资产位置
    Core::Console::InitUTF8();
    ForceWorkDir();

    const UINT W=1280,H=720;
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=h; wc.lpszClassName=L"ForFunRefactor";
    wc.hCursor=LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT rc{0,0,(LONG)W,(LONG)H}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND win = CreateWindowW(wc.lpszClassName, L"Engine/Core Refactor (DX11)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right-rc.left, rc.bottom-rc.top,
        nullptr, nullptr, h, nullptr);
    ShowWindow(win, n);

    // Raw mouse input
    RAWINPUTDEVICE rid{0x01,0x02,0,win};
    RegisterRawInputDevices(&rid,1,sizeof(rid));

    if(!gRender.Initialize(win, W, H)) return 1;
    gCamera.SetLookAt({-6.f,0.8f,0.f}, {0,0,0});
    gRender.SetCamera(&gCamera);
    gUpdate.BindCamera(&gCamera);

    MSG msg{};
    auto t0 = std::chrono::high_resolution_clock::now();
    bool running=true;
    while(running){
        while(PeekMessage(&msg, nullptr, 0,0, PM_REMOVE)){
            if(msg.message==WM_QUIT){ running=false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if(!running) break;
        auto t1 = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(t1-t0).count();
        t0 = t1;

        gUpdate.Tick(dt);
        gRender.Frame(dt);
    }

    gRender.Shutdown();
    return 0;
}

LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch(m){
    case WM_KEYDOWN: gUpdate.OnKeyDown((uint32_t)wp); break;
    case WM_KEYUP:   gUpdate.OnKeyUp((uint32_t)wp); break;
    case WM_RBUTTONDOWN: gUpdate.OnRButton(true); gRender.OnRButton(true); SetCapture(w); break;
    case WM_RBUTTONUP:   gUpdate.OnRButton(false); gRender.OnRButton(false); ReleaseCapture(); break;
    case WM_INPUT:{
        UINT sz=0; GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        if(sz){
            std::vector<BYTE> buf(sz);
            if(GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER))==sz){
                RAWINPUT* ri=(RAWINPUT*)buf.data();
                if(ri->header.dwType==RIM_TYPEMOUSE){
                    gUpdate.OnMouseDelta((int)ri->data.mouse.lLastX, (int)ri->data.mouse.lLastY);
                    gRender.OnMouseDelta((int)ri->data.mouse.lLastX, (int)ri->data.mouse.lLastY);
                }
            }
        }
        return 0;}
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProc(w,m,wp,lp);
    }
    return 0;
}
