
#include <windows.h>
#include "Renderer.h"
#ifndef RID_INPUT
#include <winuser.h>
#endif

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static Renderer gRenderer;
static bool gRMB = false;

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    const UINT width = 1280, height = 720;

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DX11_Bunny_BlinnPhong";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT rc{0,0,(LONG)width,(LONG)height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"DX11: Bunny Blinn-Phong + Texture + NormalMap",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right-rc.left, rc.bottom-rc.top,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    if (!gRenderer.Initialize(hwnd, width, height)) return 1;

    // Camera: -X looking +X (towards origin)
    gRenderer.ResetCameraLookAt({-6.0f,0.8f,0.0f}, {0,0,0});

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
    case WM_RBUTTONDOWN:
        gRMB = true; gRenderer.OnRButton(true); SetCapture(hwnd);
        break;
    case WM_RBUTTONUP:
        gRMB = false; gRenderer.OnRButton(false); ReleaseCapture();
        break;
    case WM_INPUT:
        if (gRMB) {
            UINT dwSize = 0;
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
            if (dwSize) {
                BYTE* lpb = new BYTE[dwSize];
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
                    RAWINPUT* raw = (RAWINPUT*)lpb;
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        gRenderer.OnMouseDelta((int)raw->data.mouse.lLastX, (int)raw->data.mouse.lLastY);
                    }
                }
                delete[] lpb;
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
