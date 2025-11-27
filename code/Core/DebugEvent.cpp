#include "DebugEvent.h"

CScopedDebugEvent::CScopedDebugEvent(ID3D11DeviceContext* context, const wchar_t* name) {
    if (context) {
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
