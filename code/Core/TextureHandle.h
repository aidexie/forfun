#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIPointers.h"
#include <atomic>
#include <memory>
#include <string>

/**
 * CTextureHandle - Async-friendly texture wrapper
 *
 * Provides transparent access to textures that may still be loading.
 * Returns placeholder texture until the real texture is ready.
 *
 * Usage:
 *   TextureHandlePtr handle = TextureManager::Load("path/to/texture.png", true);
 *   RHI::ITexture* tex = handle->GetTexture();  // Returns placeholder or real
 *   if (handle->IsReady()) { ... }              // Check if fully loaded
 */
class CTextureHandle {
public:
    enum class EState {
        Pending,    // Queued for loading
        Loading,    // Currently loading (disk I/O)
        Uploading,  // GPU upload in progress
        Ready,      // Fully loaded and ready
        Failed      // Load failed, using fallback
    };

    CTextureHandle(RHI::TextureSharedPtr placeholder, const std::string& path, bool srgb)
        : m_placeholder(std::move(placeholder))
        , m_path(path)
        , m_srgb(srgb)
        , m_state(EState::Pending)
    {}

    // Get the current usable texture (placeholder if not ready, real if ready)
    RHI::ITexture* GetTexture() const {
        if (m_state == EState::Ready || m_state == EState::Failed) {
            return m_realTexture ? m_realTexture.get() : m_placeholder.get();
        }
        return m_placeholder.get();
    }

    // Get as shared_ptr for systems that need ownership
    RHI::TextureSharedPtr GetTextureShared() const {
        if (m_state == EState::Ready || m_state == EState::Failed) {
            return m_realTexture ? m_realTexture : m_placeholder;
        }
        return m_placeholder;
    }

    // Check if texture is fully loaded
    bool IsReady() const { return m_state == EState::Ready; }

    // Check if load failed
    bool IsFailed() const { return m_state == EState::Failed; }

    // Check if still loading
    bool IsLoading() const {
        return m_state == EState::Pending ||
               m_state == EState::Loading ||
               m_state == EState::Uploading;
    }

    // Get current state
    EState GetState() const { return m_state; }

    // Get path for debugging
    const std::string& GetPath() const { return m_path; }

    // Get sRGB flag
    bool IsSRGB() const { return m_srgb; }

private:
    friend class CTextureManager;

    // Called by TextureManager when load completes
    void SetReady(RHI::TextureSharedPtr texture) {
        m_realTexture = std::move(texture);
        m_state = EState::Ready;
    }

    // Called by TextureManager if load fails
    void SetFailed() {
        m_state = EState::Failed;
    }

    // Called by TextureManager during loading
    void SetState(EState state) {
        m_state = state;
    }

    RHI::TextureSharedPtr m_placeholder;
    RHI::TextureSharedPtr m_realTexture;
    std::string m_path;
    bool m_srgb;
    EState m_state;
};

using TextureHandlePtr = std::shared_ptr<CTextureHandle>;
