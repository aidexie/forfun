// ============================================
// DX12Debug.cpp - Debug helper implementations
// ============================================

#include "DX12Common.h"
#include "DX12Context.h"
#include "../../Core/FFLog.h"
#include <vector>

#ifdef _DEBUG

namespace {
    // Cached InfoQueue pointer (initialized lazily)
    ID3D12InfoQueue* g_infoQueue = nullptr;

    ID3D12InfoQueue* GetInfoQueue() {
        // Try to get InfoQueue if we don't have one yet
        if (!g_infoQueue) {
            ID3D12Device* device = RHI::DX12::CDX12Context::Instance().GetDevice();
            if (device) {
                device->QueryInterface(IID_PPV_ARGS(&g_infoQueue));
            }
        }
        return g_infoQueue;
    }
}

namespace RHI {
namespace DX12 {

void DX12Debug_ClearMessages() {
    ID3D12InfoQueue* infoQueue = GetInfoQueue();
    if (infoQueue) {
        infoQueue->ClearStoredMessages();
    }
}

void DX12Debug_PrintMessages(const char* expr, const char* file, int line) {
    ID3D12InfoQueue* infoQueue = GetInfoQueue();

    // Extract just the filename from the full path
    const char* filename = file;
    const char* lastSlash = strrchr(file, '\\');
    if (lastSlash) filename = lastSlash + 1;
    const char* lastFwdSlash = strrchr(filename, '/');
    if (lastFwdSlash) filename = lastFwdSlash + 1;

    if (!infoQueue) {
        CFFLog::Error("[DX12] Error at %s:%d (InfoQueue unavailable)", filename, line);
        CFFLog::Error("[DX12]   Call: %s", expr);
        return;
    }

    UINT64 messageCount = infoQueue->GetNumStoredMessages();
    if (messageCount == 0) {
        CFFLog::Error("[DX12] Error at %s:%d (no debug messages)", filename, line);
        CFFLog::Error("[DX12]   Call: %s", expr);
        return;
    }

    CFFLog::Error("[DX12] Error at %s:%d", filename, line);
    CFFLog::Error("[DX12]   Call: %s", expr);

    for (UINT64 i = 0; i < messageCount; i++) {
        SIZE_T messageLength = 0;
        infoQueue->GetMessage(i, nullptr, &messageLength);
        if (messageLength > 0) {
            std::vector<char> messageData(messageLength);
            D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &messageLength))) {
                // Map severity to log level
                const char* severity = "INFO";
                switch (message->Severity) {
                    case D3D12_MESSAGE_SEVERITY_CORRUPTION: severity = "CORRUPTION"; break;
                    case D3D12_MESSAGE_SEVERITY_ERROR:      severity = "ERROR"; break;
                    case D3D12_MESSAGE_SEVERITY_WARNING:    severity = "WARNING"; break;
                    case D3D12_MESSAGE_SEVERITY_INFO:       severity = "INFO"; break;
                    case D3D12_MESSAGE_SEVERITY_MESSAGE:    severity = "MESSAGE"; break;
                }
                CFFLog::Error("[DX12]   [%s] %s", severity, message->pDescription);
            }
        }
    }

    infoQueue->ClearStoredMessages();
}

} // namespace DX12
} // namespace RHI

#endif // _DEBUG
