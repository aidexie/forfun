#include "RHIManager.h"
#include "RHIFactory.h"

namespace RHI {

CRHIManager& CRHIManager::Instance() {
    static CRHIManager instance;
    return instance;
}

bool CRHIManager::Initialize(EBackend backend, void* nativeWindowHandle, uint32_t width, uint32_t height) {
    if (m_initialized) {
        return true;
    }

    m_renderContext.reset(CreateRenderContext(backend));
    if (!m_renderContext) {
        return false;
    }

    if (!m_renderContext->Initialize(nativeWindowHandle, width, height)) {
        m_renderContext.reset();
        return false;
    }

    m_backend = backend;
    m_initialized = true;
    return true;
}

void CRHIManager::Shutdown() {
    if (m_renderContext) {
        m_renderContext->Shutdown();
    }
    m_renderContext.reset();
    m_initialized = false;
}

} // namespace RHI
