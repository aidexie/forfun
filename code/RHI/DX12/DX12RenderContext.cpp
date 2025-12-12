#include "DX12RenderContext.h"
#include "DX12Context.h"
#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "DX12UploadManager.h"
#include "DX12PipelineState.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// Constructor / Destructor
// ============================================

CDX12RenderContext::CDX12RenderContext() {
}

CDX12RenderContext::~CDX12RenderContext() {
    Shutdown();
}

// ============================================
// Lifecycle
// ============================================

bool CDX12RenderContext::Initialize(void* nativeWindowHandle, uint32_t width, uint32_t height) {
    HWND hwnd = static_cast<HWND>(nativeWindowHandle);

    // Initialize DX12 context (device, swapchain, etc.)
    if (!CDX12Context::Instance().Initialize(hwnd, width, height)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize DX12Context");
        return false;
    }

    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Initialize descriptor heap manager
    if (!CDX12DescriptorHeapManager::Instance().Initialize(device)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize descriptor heap manager");
        return false;
    }

    // Initialize upload manager
    if (!CDX12UploadManager::Instance().Initialize(device)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize upload manager");
        return false;
    }

    // Initialize PSO cache
    if (!CDX12PSOCache::Instance().Initialize(device)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize PSO cache");
        return false;
    }

    // Create root signatures
    if (!CreateRootSignatures()) {
        CFFLog::Error("[DX12RenderContext] Failed to create root signatures");
        return false;
    }

    // Create command list
    m_commandList = std::make_unique<CDX12CommandList>(this);
    if (!m_commandList->Initialize()) {
        CFFLog::Error("[DX12RenderContext] Failed to create command list");
        return false;
    }

    // Create depth stencil buffer
    CreateDepthStencilBuffer();

    CFFLog::Info("[DX12RenderContext] Initialized successfully");
    return true;
}

void CDX12RenderContext::Shutdown() {
    // Wait for GPU to finish
    CDX12Context::Instance().WaitForGPU();

    // Release resources
    m_depthStencilBuffer.reset();
    m_backbufferWrapper.reset();
    m_commandList.reset();

    m_graphicsRootSignature.Reset();
    m_computeRootSignature.Reset();

    // Shutdown managers
    CDX12PSOCache::Instance().Shutdown();
    CDX12UploadManager::Instance().Shutdown();
    CDX12DescriptorHeapManager::Instance().Shutdown();
    CDX12Context::Instance().Shutdown();

    CFFLog::Info("[DX12RenderContext] Shutdown complete");
}

void CDX12RenderContext::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    // Wait for GPU
    CDX12Context::Instance().WaitForGPU();

    // Release depth stencil
    ReleaseDepthStencilBuffer();
    m_backbufferWrapper.reset();

    // Resize swapchain
    CDX12Context::Instance().OnResize(width, height);

    // Recreate depth stencil
    CreateDepthStencilBuffer();
}

// ============================================
// Frame Control
// ============================================

void CDX12RenderContext::BeginFrame() {
    if (m_frameInProgress) {
        CFFLog::Warning("[DX12RenderContext] BeginFrame called while frame in progress");
        return;
    }

    // Reset command list with current frame's allocator
    m_commandList->Reset(CDX12Context::Instance().GetCurrentCommandAllocator());
    m_frameInProgress = true;
}

void CDX12RenderContext::EndFrame() {
    if (!m_frameInProgress) {
        CFFLog::Warning("[DX12RenderContext] EndFrame called without BeginFrame");
        return;
    }

    // Transition backbuffer to present state
    ID3D12Resource* backbuffer = CDX12Context::Instance().GetCurrentBackbuffer();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->GetNativeCommandList()->ResourceBarrier(1, &barrier);

    // Close command list
    m_commandList->Close();

    // Execute command list
    ID3D12CommandList* cmdLists[] = { m_commandList->GetNativeCommandList() };
    CDX12Context::Instance().GetCommandQueue()->ExecuteCommandLists(1, cmdLists);

    // Signal fence for upload manager
    uint64_t fenceValue = CDX12Context::Instance().SignalFence();
    CDX12UploadManager::Instance().FinishUploads(fenceValue);

    m_frameInProgress = false;
}

void CDX12RenderContext::Present(bool vsync) {
    auto& context = CDX12Context::Instance();

    UINT syncInterval = vsync ? 1 : 0;
    UINT presentFlags = 0;

    HRESULT hr = context.GetSwapChain()->Present(syncInterval, presentFlags);
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] Present failed: %s", HRESULTToString(hr).c_str());
    }

    // Move to next frame
    context.MoveToNextFrame();

    // Process completed uploads
    uint64_t completedValue = context.GetCurrentFenceValue();
    CDX12UploadManager::Instance().ProcessCompletedUploads(completedValue);
}

// ============================================
// Command List Access
// ============================================

ICommandList* CDX12RenderContext::GetCommandList() {
    return m_commandList.get();
}

// ============================================
// Resource Creation
// ============================================

IBuffer* CDX12RenderContext::CreateBuffer(const BufferDesc& desc, const void* initialData) {
    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Determine heap type
    D3D12_HEAP_TYPE heapType = GetHeapType(desc.cpuAccess, desc.usage);

    // Buffer size must be aligned for constant buffers
    uint32_t alignedSize = desc.size;
    if (desc.usage & EBufferUsage::Constant) {
        alignedSize = AlignUp(desc.size, CONSTANT_BUFFER_ALIGNMENT);
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = alignedSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Resource flags
    if (desc.usage & EBufferUsage::UnorderedAccess) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_STATES initialState = GetInitialResourceState(heapType, desc.usage);

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource)
    );

    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateBuffer failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        resource->SetName(wname);
    }

    // Create buffer wrapper
    CDX12Buffer* buffer = new CDX12Buffer(resource.Get(), desc, device);
    resource.Detach();  // Transfer ownership

    // Upload initial data if provided
    if (initialData && heapType == D3D12_HEAP_TYPE_UPLOAD) {
        void* mapped = buffer->Map();
        if (mapped) {
            memcpy(mapped, initialData, desc.size);
            buffer->Unmap();
        }
    } else if (initialData && heapType == D3D12_HEAP_TYPE_DEFAULT) {
        // Would need upload buffer and copy command
        CFFLog::Warning("[DX12RenderContext] Initial data for default heap buffer not implemented");
    }

    return buffer;
}

ITexture* CDX12RenderContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    if (initialData) {
        SubresourceData subresource;
        subresource.pData = initialData;
        subresource.rowPitch = desc.width * GetBytesPerPixel(desc.format);
        subresource.slicePitch = subresource.rowPitch * desc.height;
        return CreateTextureInternal(desc, &subresource, 1);
    }
    return CreateTextureInternal(desc, nullptr, 0);
}

ITexture* CDX12RenderContext::CreateTextureWithData(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources) {
    return CreateTextureInternal(desc, subresources, numSubresources);
}

ITexture* CDX12RenderContext::CreateTextureInternal(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources) {
    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Determine resource dimension
    D3D12_RESOURCE_DIMENSION dimension;
    uint32_t arraySize = desc.arraySize;

    switch (desc.dimension) {
        case ETextureDimension::Tex2D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        case ETextureDimension::Tex3D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            break;
        case ETextureDimension::TexCube:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            arraySize = 6;
            break;
        case ETextureDimension::Tex2DArray:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        case ETextureDimension::TexCubeArray:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            arraySize = desc.arraySize * 6;
            break;
        default:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = (desc.usage & ETextureUsage::Staging) ?
                     ((desc.cpuAccess == ECPUAccess::Read) ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD) :
                     D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = dimension;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? desc.depth : static_cast<UINT16>(arraySize);
    resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resourceDesc.Format = ToDXGIFormat(desc.format);
    resourceDesc.SampleDesc.Count = desc.sampleCount;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = GetResourceFlags(desc.usage);

    D3D12_RESOURCE_STATES initialState = GetInitialResourceState(heapProps.Type, desc.usage);

    // Clear value for render targets / depth stencil
    D3D12_CLEAR_VALUE* pClearValue = nullptr;
    D3D12_CLEAR_VALUE clearValue = {};
    if (desc.usage & ETextureUsage::RenderTarget) {
        clearValue.Format = (desc.rtvFormat != ETextureFormat::Unknown) ? ToDXGIFormat(desc.rtvFormat) : resourceDesc.Format;
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;
        pClearValue = &clearValue;
    } else if (desc.usage & ETextureUsage::DepthStencil) {
        clearValue.Format = (desc.dsvFormat != ETextureFormat::Unknown) ? ToDXGIFormat(desc.dsvFormat) : resourceDesc.Format;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;
        pClearValue = &clearValue;
    }

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        pClearValue,
        IID_PPV_ARGS(&resource)
    );

    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateTexture failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        resource->SetName(wname);
    }

    // Create texture wrapper
    CDX12Texture* texture = new CDX12Texture(resource.Get(), desc, device);
    resource.Detach();

    // Upload initial data if provided
    if (subresources && numSubresources > 0 && heapProps.Type == D3D12_HEAP_TYPE_DEFAULT) {
        // TODO: Implement texture upload using upload manager
        CFFLog::Warning("[DX12RenderContext] Texture initial data upload not fully implemented");
    }

    return texture;
}

ISampler* CDX12RenderContext::CreateSampler(const SamplerDesc& desc) {
    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    SDescriptorHandle handle = heapMgr.AllocateSampler();
    if (!handle.IsValid()) {
        CFFLog::Error("[DX12RenderContext] Failed to allocate sampler descriptor");
        return nullptr;
    }

    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    D3D12_SAMPLER_DESC samplerDesc = {};

    // Filter conversion
    switch (desc.filter) {
        case EFilter::MinMagMipPoint:       samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; break;
        case EFilter::MinMagMipLinear:      samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; break;
        case EFilter::Anisotropic:          samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC; break;
        case EFilter::ComparisonMinMagMipLinear: samplerDesc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR; break;
        case EFilter::ComparisonAnisotropic: samplerDesc.Filter = D3D12_FILTER_COMPARISON_ANISOTROPIC; break;
        default:                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; break;
    }

    // Address mode conversion
    auto convertAddressMode = [](ETextureAddressMode mode) -> D3D12_TEXTURE_ADDRESS_MODE {
        switch (mode) {
            case ETextureAddressMode::Wrap:   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case ETextureAddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case ETextureAddressMode::Clamp:  return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case ETextureAddressMode::Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default:                          return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    };

    samplerDesc.AddressU = convertAddressMode(desc.addressU);
    samplerDesc.AddressV = convertAddressMode(desc.addressV);
    samplerDesc.AddressW = convertAddressMode(desc.addressW);
    samplerDesc.MipLODBias = desc.mipLODBias;
    samplerDesc.MaxAnisotropy = desc.maxAnisotropy;

    // Comparison func conversion
    auto convertCompFunc = [](EComparisonFunc func) -> D3D12_COMPARISON_FUNC {
        switch (func) {
            case EComparisonFunc::Never:        return D3D12_COMPARISON_FUNC_NEVER;
            case EComparisonFunc::Less:         return D3D12_COMPARISON_FUNC_LESS;
            case EComparisonFunc::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
            case EComparisonFunc::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case EComparisonFunc::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
            case EComparisonFunc::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case EComparisonFunc::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case EComparisonFunc::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
            default:                            return D3D12_COMPARISON_FUNC_NEVER;
        }
    };

    samplerDesc.ComparisonFunc = convertCompFunc(desc.comparisonFunc);
    memcpy(samplerDesc.BorderColor, desc.borderColor, sizeof(float) * 4);
    samplerDesc.MinLOD = desc.minLOD;
    samplerDesc.MaxLOD = desc.maxLOD;

    device->CreateSampler(&samplerDesc, handle.cpuHandle);

    return new CDX12Sampler(handle);
}

IShader* CDX12RenderContext::CreateShader(const ShaderDesc& desc) {
    return new CDX12Shader(desc.type, desc.bytecode, desc.bytecodeSize);
}

IPipelineState* CDX12RenderContext::CreatePipelineState(const PipelineStateDesc& desc) {
    CDX12PSOBuilder builder;

    // Set root signature
    builder.SetRootSignature(m_graphicsRootSignature.Get());

    // Set shaders
    if (desc.vertexShader) {
        CDX12Shader* vs = static_cast<CDX12Shader*>(desc.vertexShader);
        builder.SetVertexShader(vs->GetBytecode());
    }
    if (desc.pixelShader) {
        CDX12Shader* ps = static_cast<CDX12Shader*>(desc.pixelShader);
        builder.SetPixelShader(ps->GetBytecode());
    }
    if (desc.geometryShader) {
        CDX12Shader* gs = static_cast<CDX12Shader*>(desc.geometryShader);
        builder.SetGeometryShader(gs->GetBytecode());
    }
    if (desc.hullShader) {
        CDX12Shader* hs = static_cast<CDX12Shader*>(desc.hullShader);
        builder.SetHullShader(hs->GetBytecode());
    }
    if (desc.domainShader) {
        CDX12Shader* ds = static_cast<CDX12Shader*>(desc.domainShader);
        builder.SetDomainShader(ds->GetBytecode());
    }

    // Set input layout
    if (!desc.inputLayout.empty()) {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        for (const auto& elem : desc.inputLayout) {
            D3D12_INPUT_ELEMENT_DESC d3dElem = {};
            d3dElem.SemanticName = ToD3D12SemanticName(elem.semantic);
            d3dElem.SemanticIndex = elem.semanticIndex;
            d3dElem.Format = ToD3D12VertexFormat(elem.format);
            d3dElem.InputSlot = elem.inputSlot;
            d3dElem.AlignedByteOffset = elem.offset;
            d3dElem.InputSlotClass = elem.instanceData ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            d3dElem.InstanceDataStepRate = elem.instanceData ? 1 : 0;
            elements.push_back(d3dElem);
        }
        builder.SetInputLayout(elements);
    }

    // Set rasterizer state
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = ToD3D12FillMode(desc.rasterizer.fillMode);
    rasterizerDesc.CullMode = ToD3D12CullMode(desc.rasterizer.cullMode);
    rasterizerDesc.FrontCounterClockwise = desc.rasterizer.frontCounterClockwise;
    rasterizerDesc.DepthBias = desc.rasterizer.depthBias;
    rasterizerDesc.DepthBiasClamp = desc.rasterizer.depthBiasClamp;
    rasterizerDesc.SlopeScaledDepthBias = desc.rasterizer.slopeScaledDepthBias;
    rasterizerDesc.DepthClipEnable = desc.rasterizer.depthClipEnable;
    rasterizerDesc.MultisampleEnable = desc.rasterizer.multisampleEnable;
    rasterizerDesc.AntialiasedLineEnable = desc.rasterizer.antialiasedLineEnable;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    builder.SetRasterizerState(rasterizerDesc);

    // Set depth stencil state
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = desc.depthStencil.depthEnable;
    depthStencilDesc.DepthWriteMask = desc.depthStencil.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc = ToD3D12ComparisonFunc(desc.depthStencil.depthFunc);
    depthStencilDesc.StencilEnable = desc.depthStencil.stencilEnable;
    depthStencilDesc.StencilReadMask = desc.depthStencil.stencilReadMask;
    depthStencilDesc.StencilWriteMask = desc.depthStencil.stencilWriteMask;
    builder.SetDepthStencilState(depthStencilDesc);

    // Set blend state
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = desc.blend.blendEnable;
    blendDesc.RenderTarget[0].SrcBlend = ToD3D12BlendFactor(desc.blend.srcBlend);
    blendDesc.RenderTarget[0].DestBlend = ToD3D12BlendFactor(desc.blend.dstBlend);
    blendDesc.RenderTarget[0].BlendOp = ToD3D12BlendOp(desc.blend.blendOp);
    blendDesc.RenderTarget[0].SrcBlendAlpha = ToD3D12BlendFactor(desc.blend.srcBlendAlpha);
    blendDesc.RenderTarget[0].DestBlendAlpha = ToD3D12BlendFactor(desc.blend.dstBlendAlpha);
    blendDesc.RenderTarget[0].BlendOpAlpha = ToD3D12BlendOp(desc.blend.blendOpAlpha);
    blendDesc.RenderTarget[0].RenderTargetWriteMask = desc.blend.renderTargetWriteMask;
    builder.SetBlendState(blendDesc);

    // Set render target formats
    std::vector<DXGI_FORMAT> rtFormats;
    for (auto fmt : desc.renderTargetFormats) {
        rtFormats.push_back(ToDXGIFormat(fmt));
    }
    if (rtFormats.empty()) {
        rtFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);  // Default
    }
    builder.SetRenderTargetFormats(rtFormats);

    // Set depth stencil format
    if (desc.depthStencilFormat != ETextureFormat::Unknown) {
        builder.SetDepthStencilFormat(ToDXGIFormat(desc.depthStencilFormat));
    } else {
        builder.SetDepthStencilFormat(DXGI_FORMAT_D24_UNORM_S8_UINT);
    }

    // Set topology type
    builder.SetPrimitiveTopologyType(ToD3D12TopologyType(desc.primitiveTopology));

    // Build PSO
    ID3D12PipelineState* pso = builder.Build(CDX12Context::Instance().GetDevice());
    if (!pso) {
        CFFLog::Error("[DX12RenderContext] Failed to create graphics PSO");
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        pso->SetName(wname);
    }

    return new CDX12PipelineState(pso, m_graphicsRootSignature.Get(), false);
}

IPipelineState* CDX12RenderContext::CreateComputePipelineState(const ComputePipelineDesc& desc) {
    if (!desc.computeShader) {
        CFFLog::Error("[DX12RenderContext] CreateComputePipelineState requires compute shader");
        return nullptr;
    }

    CDX12Shader* cs = static_cast<CDX12Shader*>(desc.computeShader);

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_computeRootSignature.Get();
    psoDesc.CS = cs->GetBytecode();

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = CDX12Context::Instance().GetDevice()->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&pso));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateComputePipelineState failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        pso->SetName(wname);
    }

    return new CDX12PipelineState(pso.Detach(), m_computeRootSignature.Get(), true);
}

ITexture* CDX12RenderContext::WrapNativeTexture(void* nativeTexture, void* nativeSRV, uint32_t width, uint32_t height, ETextureFormat format) {
    CFFLog::Warning("[DX12RenderContext] WrapNativeTexture not implemented");
    return nullptr;
}

ITexture* CDX12RenderContext::WrapExternalTexture(void* nativeTexture, const TextureDesc& desc) {
    CFFLog::Warning("[DX12RenderContext] WrapExternalTexture not implemented");
    return nullptr;
}

// ============================================
// Backbuffer Access
// ============================================

ITexture* CDX12RenderContext::GetBackbuffer() {
    // For DX12, we return a wrapper around the current backbuffer
    // This is simplified - would need proper per-frame backbuffer handling
    return m_backbufferWrapper.get();
}

ITexture* CDX12RenderContext::GetDepthStencil() {
    return m_depthStencilBuffer.get();
}

// ============================================
// Query
// ============================================

uint32_t CDX12RenderContext::GetWidth() const {
    return CDX12Context::Instance().GetWidth();
}

uint32_t CDX12RenderContext::GetHeight() const {
    return CDX12Context::Instance().GetHeight();
}

bool CDX12RenderContext::SupportsRaytracing() const {
    return CDX12Context::Instance().SupportsRaytracing();
}

bool CDX12RenderContext::SupportsMeshShaders() const {
    return CDX12Context::Instance().SupportsMeshShaders();
}

// ============================================
// Advanced
// ============================================

void* CDX12RenderContext::GetNativeDevice() {
    return CDX12Context::Instance().GetDevice();
}

void* CDX12RenderContext::GetNativeContext() {
    return m_commandList ? m_commandList->GetNativeCommandList() : nullptr;
}

ID3D12Device* CDX12RenderContext::GetDevice() const {
    return CDX12Context::Instance().GetDevice();
}

ID3D12CommandQueue* CDX12RenderContext::GetCommandQueue() const {
    return CDX12Context::Instance().GetCommandQueue();
}

// ============================================
// Root Signature Creation
// ============================================

bool CDX12RenderContext::CreateRootSignatures() {
    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Graphics Root Signature
    // Parameter 0: Root CBV b0 (PerFrame)
    // Parameter 1: Root CBV b1 (PerObject)
    // Parameter 2: Root CBV b2 (Material)
    // Parameter 3: SRV Descriptor Table t0-t15
    // Parameter 4: UAV Descriptor Table u0-u7
    // Parameter 5: Sampler Descriptor Table s0-s7

    D3D12_ROOT_PARAMETER rootParams[6] = {};

    // CBV parameters
    for (int i = 0; i < 3; ++i) {
        rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[i].Descriptor.ShaderRegister = i;
        rootParams[i].Descriptor.RegisterSpace = 0;
        rootParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // SRV table
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 16;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // UAV table
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 8;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Sampler table
    D3D12_DESCRIPTOR_RANGE samplerRange = {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = 8;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.RegisterSpace = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges = &samplerRange;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 6;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            CFFLog::Error("[DX12RenderContext] Root signature serialization failed: %s", (char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_graphicsRootSignature));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateRootSignature (graphics) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_graphicsRootSignature, "GraphicsRootSignature");

    // Compute Root Signature (similar but simpler)
    D3D12_ROOT_PARAMETER computeParams[4] = {};

    // CBV parameter
    computeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeParams[0].Descriptor.ShaderRegister = 0;
    computeParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // SRV table
    computeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[1].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    computeParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // UAV table
    computeParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[2].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    computeParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Sampler table
    computeParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[3].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[3].DescriptorTable.pDescriptorRanges = &samplerRange;
    computeParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeRootSigDesc = {};
    computeRootSigDesc.NumParameters = 4;
    computeRootSigDesc.pParameters = computeParams;
    computeRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    hr = D3D12SerializeRootSignature(&computeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            CFFLog::Error("[DX12RenderContext] Compute root signature serialization failed: %s", (char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateRootSignature (compute) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_computeRootSignature, "ComputeRootSignature");

    CFFLog::Info("[DX12RenderContext] Root signatures created");
    return true;
}

// ============================================
// Depth Stencil Management
// ============================================

void CDX12RenderContext::CreateDepthStencilBuffer() {
    uint32_t width = CDX12Context::Instance().GetWidth();
    uint32_t height = CDX12Context::Instance().GetHeight();

    TextureDesc desc = TextureDesc::DepthStencil(width, height);
    desc.debugName = "MainDepthStencil";

    m_depthStencilBuffer.reset(static_cast<CDX12Texture*>(CreateTexture(desc, nullptr)));
}

void CDX12RenderContext::ReleaseDepthStencilBuffer() {
    m_depthStencilBuffer.reset();
}

} // namespace DX12
} // namespace RHI
