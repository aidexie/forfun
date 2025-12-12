#include "RHIFactory.h"
#include "DX11/DX11RenderContext.h"
#include "DX12/DX12RenderContext.h"
#include "Core/FFLog.h"

namespace RHI {

IRenderContext* CreateRenderContext(EBackend backend) {
    switch (backend) {
        case EBackend::DX11:
            CFFLog::Info("[RHI] Creating DX11 backend");
            return new DX11::CDX11RenderContext();

        case EBackend::DX12:
            CFFLog::Info("[RHI] Creating DX12 backend");
            return new DX12::CDX12RenderContext();

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
