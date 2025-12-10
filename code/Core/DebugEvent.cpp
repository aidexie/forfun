#include "DebugEvent.h"
#include <d3d11_1.h>  // D3D11 implementation details stay in .cpp

CScopedDebugEvent::CScopedDebugEvent(void* nativeContext, const wchar_t* name) {
    if (nativeContext) {
        // Cast to D3D11 context (DX11 implementation)
        ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(nativeContext);

        // Query for ID3DUserDefinedAnnotation interface
        HRESULT hr = context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation),
                                             (void**)&m_annotation);
        if (SUCCEEDED(hr) && m_annotation) {
            m_annotation->BeginEvent(name);
        }
    }
}

CScopedDebugEvent::~CScopedDebugEvent() {
    if (m_annotation) {
        m_annotation->EndEvent();
        m_annotation->Release();
    }
}
