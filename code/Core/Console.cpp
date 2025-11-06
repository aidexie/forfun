#include "Console.h"

#include <windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <iostream>

namespace {

bool g_consoleReady = false;

void EnableVTSupport(HANDLE hOut) {
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode)) {
        // 开启虚拟终端处理（支持彩色/emoji 等）
        SetConsoleMode(hOut, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

// 把 C 运行库的 stdin/stdout/stderr 绑定到控制台
void BindCRTToConsole() {
    FILE* fp = nullptr;
    // 绑定窄/宽输出流，保证 printf / std::cout / std::wcout 都能用
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$",  "r", stdin);

    // 让 iostream 重新同步底层句柄
    std::ios::sync_with_stdio(true);

    // 对于宽字符，使用 UTF-16 宽模式写入控制台，避免乱码
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    _setmode(_fileno(stdin),  _O_U16TEXT);
}

} // anonymous

namespace Core::Console {

void InitUTF8() {
    // 1) 附加或分配控制台
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }

    // 2) 绑定 CRT 标准流到控制台设备
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    // 3) 窄字节输出走 UTF-8（给 std::cout）
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    // 4) 关键！把宽字节通道切到 UTF-16 直写（给 std::wcout）
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);

    // 5) 可选：VT 序列支持
    DWORD mode = 0; HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(hOut, &mode)) {
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

void Shutdown() {
    if (!g_consoleReady) return;
    // 可选：把输出重定向回 NUL，防止 VS 结束时异常
    FILE* fp = nullptr;
    freopen_s(&fp, "NUL", "w", stdout);
    freopen_s(&fp, "NUL", "w", stderr);
    freopen_s(&fp, "NUL", "r", stdin);

    FreeConsole();
    g_consoleReady = false;
}

void PrintUTF8(const std::string& s) {
    // 直接写窄字符串：控制台代码页已是 UTF-8
    DWORD written = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        WriteConsoleA(h, s.c_str(), (DWORD)s.size(), &written, nullptr);
    }
}

void PrintW(const std::wstring& s) {
    DWORD written = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &written, nullptr);
    }
}

} // namespace Core::Console
