#pragma once
#include "IRenderContext.h"

// ============================================
// RHI Factory - Create backend implementation
// ============================================

namespace RHI {

// Create render context for specified backend
// Returns nullptr if backend is not supported or creation failed
IRenderContext* CreateRenderContext(EBackend backend);

// Helper: Get backend name string
const char* GetBackendName(EBackend backend);

} // namespace RHI
