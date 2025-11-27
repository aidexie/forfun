#pragma once
#include <d3d11_1.h>  // ID3DUserDefinedAnnotation requires d3d11_1.h
#include <string>

// RAII wrapper for D3D11 debug events (RenderDoc/PIX markers)
// Usage:
//   {
//       CScopedDebugEvent evt(context, L"Shadow Pass");
//       // ... rendering code ...
//   } // Automatically calls EndEvent
class CScopedDebugEvent {
public:
    CScopedDebugEvent(ID3D11DeviceContext* context, const wchar_t* name);
    ~CScopedDebugEvent();

    // Delete copy/move to prevent misuse
    CScopedDebugEvent(const CScopedDebugEvent&) = delete;
    CScopedDebugEvent& operator=(const CScopedDebugEvent&) = delete;

private:
    ID3DUserDefinedAnnotation* m_annotation = nullptr;
};
