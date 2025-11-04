
#include <windows.h>
#include "Renderer.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static Renderer gRenderer;

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    const UINT width = 1280, height = 720;

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DX11PrimitivesClass";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT rc{0,0,(LONG)width,(LONG)height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"DX11 Primitives (Cube/Cuboid/Cylinder/Sphere)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right-rc.left, rc.bottom-rc.top,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);

    if (!gRenderer.Initialize(hwnd, width, height)) return 1;

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;
        gRenderer.Render();
    }
    gRenderer.Shutdown();
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (gRenderer.IsInitialized()) {
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            if (w > 0 && h > 0) gRenderer.Resize(w, h);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
