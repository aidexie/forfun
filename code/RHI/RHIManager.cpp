#include "RHIManager.h"
#include "RHIFactory.h"

namespace RHI {

CRHIManager& CRHIManager::Instance() {
    static CRHIManager instance;
    return instance;
}

bool CRHIManager::Initialize(EBackend backend) {
    if (m_initialized) {
        return true;
    }

    m_renderContext.reset(CreateRenderContext(backend));
    if (!m_renderContext) {
        return false;
    }

    m_initialized = true;
    return true;
}

void CRHIManager::Shutdown() {
    m_renderContext.reset();
    m_initialized = false;
}

} // namespace RHI
