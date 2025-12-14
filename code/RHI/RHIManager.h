#pragma once
#include "IRenderContext.h"
#include "RHICommon.h"
#include <memory>

namespace RHI {

// Singleton manager for global RHI context
// All rendering passes share the same device/context
class CRHIManager {
public:
    static CRHIManager& Instance();

    // Initialize with backend selection and window parameters
    bool Initialize(EBackend backend, void* nativeWindowHandle, uint32_t width, uint32_t height);
    void Shutdown();

    // Get the global render context
    IRenderContext* GetRenderContext() { return m_renderContext.get(); }

    // Get the current backend type
    EBackend GetBackend() const { return m_backend; }

    bool IsInitialized() const { return m_initialized; }

private:
    CRHIManager() = default;
    ~CRHIManager() = default;
    CRHIManager(const CRHIManager&) = delete;
    CRHIManager& operator=(const CRHIManager&) = delete;

    std::unique_ptr<IRenderContext> m_renderContext;
    EBackend m_backend = EBackend::DX11;
    bool m_initialized = false;
};

} // namespace RHI
