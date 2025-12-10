#include "RHIFactory.h"
#include "DX11/DX11RenderContext.h"
// #include "DX12/DX12RenderContext.h"  // TODO: Implement in Phase 3
#include "Core/FFLog.h"

namespace RHI {

IRenderContext* CreateRenderContext(EBackend backend) {
    switch (backend) {
        case EBackend::DX11:
            CFFLog::Info("[RHI] Creating DX11 backend");
            return new DX11::CDX11RenderContext();

        case EBackend::DX12:
            CFFLog::Error("[RHI] DX12 backend not yet implemented (coming in Phase 3)");
            CFFLog::Info("[RHI] Falling back to DX11");
            return new DX11::CDX11RenderContext();

        default:
            CFFLog::Error("[RHI] Unknown backend: %d", static_cast<int>(backend));
            return nullptr;
    }
}

const char* GetBackendName(EBackend backend) {
    switch (backend) {
        case EBackend::DX11: return "DirectX 11";
        case EBackend::DX12: return "DirectX 12";
        default: return "Unknown";
    }
}

} // namespace RHI
