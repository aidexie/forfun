#pragma once
#include <string>

// Forward declaration to avoid exposing D3D11 types in header
struct ID3DUserDefinedAnnotation;

// RAII wrapper for D3D11 debug events (RenderDoc/PIX markers)
// Usage:
//   {
//       CScopedDebugEvent evt(context, L"Shadow Pass");
//       // ... rendering code ...
//   } // Automatically calls EndEvent
class CScopedDebugEvent {
public:
    // Constructor takes void* to avoid D3D11 header dependency
    // nativeContext: ID3D11DeviceContext* (DX11) or ID3D12GraphicsCommandList* (DX12, future)
    CScopedDebugEvent(void* nativeContext, const wchar_t* name);
    ~CScopedDebugEvent();

    // Delete copy/move to prevent misuse
    CScopedDebugEvent(const CScopedDebugEvent&) = delete;
    CScopedDebugEvent& operator=(const CScopedDebugEvent&) = delete;

private:
    ID3DUserDefinedAnnotation* m_annotation = nullptr;
};
