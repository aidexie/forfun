#pragma once
#include "RHIResources.h"
#include <memory>

namespace RHI {

// Custom deleters for RHI resources
struct RHIDeleter {
    void operator()(IBuffer* ptr) { delete ptr; }
    void operator()(ITexture* ptr) { delete ptr; }
    void operator()(ISampler* ptr) { delete ptr; }
    void operator()(IShader* ptr) { delete ptr; }
    void operator()(IPipelineState* ptr) { delete ptr; }
};

// Smart pointer types for RHI resources (unique ownership)
using BufferPtr = std::unique_ptr<IBuffer, RHIDeleter>;
using TexturePtr = std::unique_ptr<ITexture, RHIDeleter>;
using SamplerPtr = std::unique_ptr<ISampler, RHIDeleter>;
using ShaderPtr = std::unique_ptr<IShader, RHIDeleter>;
using PipelineStatePtr = std::unique_ptr<IPipelineState, RHIDeleter>;

// Shared pointer types for RHI resources (shared ownership)
// Use when multiple systems need to hold references to the same resource
using TextureSharedPtr = std::shared_ptr<ITexture>;

} // namespace RHI
