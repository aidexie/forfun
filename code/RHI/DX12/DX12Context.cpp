#include "DX12Context.h"
#include "DX12Common.h"
#include "DX12MemoryAllocator.h"
#include "../../Core/FFLog.h"
#include <algorithm>
#include <cstdlib>  // for std::getenv

namespace RHI {
namespace DX12 {

// ============================================
// Deferred Deletion Queue Implementation
// ============================================

void CDX12DeferredDeletionQueue::DeferredRelease(ID3D12Resource* resource, uint64_t fenceValue) {
    if (!resource) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingResources.push_back({resource, fenceValue});
}

void CDX12DeferredDeletionQueue::DeferredRelease(ID3D12DescriptorHeap* heap, uint64_t fenceValue) {
    if (!heap) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingHeaps.push_back({heap, fenceValue});
}

void CDX12DeferredDeletionQueue::ProcessCompleted(uint64_t completedFenceValue) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Remove resources whose fence has completed
    size_t resourcesBefore = m_pendingResources.size();
    m_pendingResources.erase(
       std::remove_if(m_pendingResources.begin(), m_pendingResources.end(),
           [completedFenceValue](const SPendingResource& p) {
               return p.fenceValue < completedFenceValue;
           }),
       m_pendingResources.end());

    // Remove heaps whose fence has completed
    size_t heapsBefore = m_pendingHeaps.size();
    m_pendingHeaps.erase(
       std::remove_if(m_pendingHeaps.begin(), m_pendingHeaps.end(),
           [completedFenceValue](const SPendingHeap& p) {
               return p.fenceValue < completedFenceValue;
           }),
       m_pendingHeaps.end());

    size_t released = (resourcesBefore - m_pendingResources.size()) + (heapsBefore - m_pendingHeaps.size());
    if (released > 0) {
       CFFLog::Info("[DX12] Deferred deletion: released %zu resources (fence %llu)", released, completedFenceValue);
    }
}

void CDX12DeferredDeletionQueue::ReleaseAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t total = m_pendingResources.size() + m_pendingHeaps.size();
    m_pendingResources.clear();
    m_pendingHeaps.clear();

    if (total > 0) {
        CFFLog::Info("[DX12] Deferred deletion: force released %zu resources at shutdown", total);
    }
}

// ============================================
// Singleton Instance
// ============================================

CDX12Context& CDX12Context::Instance() {
    static CDX12Context instance;
    return instance;
}

CDX12Context::~CDX12Context() {
    Shutdown();
}

// ============================================
// Initialization
// ============================================

bool CDX12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    if (m_initialized) {
        CFFLog::Warning("[DX12Context] Already initialized");
        return true;
    }

    CFFLog::Info("[DX12Context] Initializing DX12 backend...");

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    // Enable debug layer first (before device creation)
    EnableDebugLayer();

    // Create DXGI factory
    UINT factoryFlags = 0;
#ifdef _DEBUG
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    HRESULT hr = DX12_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Failed to create DXGI factory: %s", HRESULTToString(hr).c_str());
        return false;
    }

    // Create device
    if (!CreateDevice()) {
        CFFLog::Error("[DX12Context] Failed to create D3D12 device");
        return false;
    }

    // Note: Debug break on errors disabled to avoid conflicts with exception handling
    // D3D12 debug messages still appear in VS Output window automatically

    // Check feature support
    CheckFeatureSupport();

    // Initialize memory allocator (D3D12MA)
    if (!CDX12MemoryAllocator::Instance().Initialize(m_device.Get(), m_adapter.Get())) {
        CFFLog::Error("[DX12Context] Failed to initialize memory allocator");
        return false;
    }

    // Create command queue
    if (!CreateCommandQueue()) {
        CFFLog::Error("[DX12Context] Failed to create command queue");
        return false;
    }

    // Create swap chain
    if (!CreateSwapChain(hwnd)) {
        CFFLog::Error("[DX12Context] Failed to create swap chain");
        return false;
    }

    // Create RTV heap for backbuffers
    if (!CreateRTVHeap()) {
        CFFLog::Error("[DX12Context] Failed to create RTV heap");
        return false;
    }

    // Create SRV heap for ImGui
    if (!CreateImGuiSrvHeap()) {
        CFFLog::Error("[DX12Context] Failed to create ImGui SRV heap");
        return false;
    }

    // Create backbuffer RTVs
    CreateBackbufferRTVs();

    // Create command allocators (one per frame)
    if (!CreateCommandAllocators()) {
        CFFLog::Error("[DX12Context] Failed to create command allocators");
        return false;
    }

    // Create fence for synchronization
    if (!CreateFence()) {
        CFFLog::Error("[DX12Context] Failed to create fence");
        return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_initialized = true;

    CFFLog::Info("[DX12Context] Initialized successfully (%ux%u)", width, height);
    CFFLog::Info("[DX12Context] Raytracing support: %s", m_supportsRaytracing ? "Yes" : "No");
    CFFLog::Info("[DX12Context] Mesh shader support: %s", m_supportsMeshShaders ? "Yes" : "No");

    return true;
}

void CDX12Context::Shutdown() {
    if (!m_initialized) return;

    CFFLog::Info("[DX12Context] Shutting down...");

    // Wait for GPU to finish all work
    WaitForGPU();

    // Release all deferred deletions now that GPU is idle
    m_deletionQueue.ReleaseAll();

    // Close fence event
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // Release backbuffers
    ReleaseBackbuffers();

    // Release command allocators
    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
        m_commandAllocators[i].Reset();
    }

    // Release other resources
    m_fence.Reset();
    m_imguiSrvHeap.Reset();
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();

    // Shutdown memory allocator before device release
    CDX12MemoryAllocator::Instance().Shutdown();

    DX12Debug_Shutdown();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();

    m_initialized = false;
    CFFLog::Info("[DX12Context] Shutdown complete");
}

// ============================================
// Debug Layer
// ============================================

void CDX12Context::EnableDebugLayer() {
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        CFFLog::Info("[DX12Context] Debug layer enabled");

        // GPU-based validation: Enable by default, but can be disabled for Nsight capture
        // Set environment variable DISABLE_GPU_VALIDATION=1 to disable
        bool enableGpuValidation = true;
        if (const char* env = std::getenv("DISABLE_GPU_VALIDATION")) {
            enableGpuValidation = (std::string(env) != "1");
        }

        if (enableGpuValidation) {
            ComPtr<ID3D12Debug1> debugController1;
            if (SUCCEEDED(debugController.As(&debugController1))) {
                debugController1->SetEnableGPUBasedValidation(true);
                CFFLog::Info("[DX12Context] GPU-based validation enabled");
            }
        } else {
            CFFLog::Warning("[DX12Context] GPU-based validation DISABLED (for Nsight capture)");
        }
    } else {
        CFFLog::Warning("[DX12Context] Failed to enable debug layer");
    }

    // Enable DRED (Device Removed Extended Data) for better crash diagnostics
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        CFFLog::Info("[DX12Context] DRED enabled for crash diagnostics");
    }
#endif
}

void CDX12Context::FlushDebugMessages() {
#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        UINT64 numMessages = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < numMessages; ++i) {
            SIZE_T messageLength = 0;
            infoQueue->GetMessage(i, nullptr, &messageLength);
            if (messageLength > 0) {
                std::vector<char> messageData(messageLength);
                D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
                if (SUCCEEDED(infoQueue->GetMessage(i, message, &messageLength))) {
                    const char* severityStr = "INFO";
                    bool isError = false;
                    switch (message->Severity) {
                        case D3D12_MESSAGE_SEVERITY_CORRUPTION: severityStr = "CORRUPTION"; isError = true; break;
                        case D3D12_MESSAGE_SEVERITY_ERROR: severityStr = "ERROR"; isError = true; break;
                        case D3D12_MESSAGE_SEVERITY_WARNING: severityStr = "WARNING"; break;
                        case D3D12_MESSAGE_SEVERITY_INFO: severityStr = "INFO"; break;
                        case D3D12_MESSAGE_SEVERITY_MESSAGE: severityStr = "MESSAGE"; break;
                    }

                    // Output to both debugger and log file
                    char buffer[4096];
                    snprintf(buffer, sizeof(buffer), "[D3D12 %s] %s", severityStr, message->pDescription);
                    OutputDebugStringA(buffer);
                    OutputDebugStringA("\n");

                    // Also log via CFFLog so it appears in test output
                    if (isError) {
                        CFFLog::Error("%s", buffer);
                    } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                        CFFLog::Warning("%s", buffer);
                    } else {
                        CFFLog::Info("%s", buffer);
                    }
                }
            }
        }
        infoQueue->ClearStoredMessages();
    }
#endif
}

// ============================================
// Device Creation
// ============================================

bool CDX12Context::CreateDevice() {
    // Try to find a hardware adapter
    ComPtr<IDXGIAdapter1> adapter;

    for (UINT adapterIndex = 0;
         m_factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND;
         adapterIndex++) {

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software adapter
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        // Try to create device with this adapter
        HRESULT hr = DX12_CHECK(D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,  // Minimum feature level
            IID_PPV_ARGS(&m_device)
        ));

        if (SUCCEEDED(hr)) {
            // Store adapter for D3D12MA initialization
            m_adapter = adapter;

            // Log adapter info
            char adapterName[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
            CFFLog::Info("[DX12Context] Using adapter: %s", adapterName);
            CFFLog::Info("[DX12Context] Dedicated VRAM: %llu MB", desc.DedicatedVideoMemory / (1024 * 1024));

            DX12_SET_DEBUG_NAME(m_device, "MainDevice");
            return true;
        }
    }

    // Fallback to WARP (software) adapter
    CFFLog::Warning("[DX12Context] No hardware adapter found, falling back to WARP");

    ComPtr<IDXGIAdapter> warpAdapter;
    HRESULT hr = DX12_CHECK(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Failed to get WARP adapter: %s", HRESULTToString(hr).c_str());
        return false;
    }

    hr = DX12_CHECK(D3D12CreateDevice(
        warpAdapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_device)
    ));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Failed to create WARP device: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_device, "WARPDevice");
    return true;
}

void CDX12Context::CheckFeatureSupport() {
    // Check raytracing support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
        m_supportsRaytracing = (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
    }

    // Cache ID3D12Device5 if ray tracing is supported (avoids repeated QueryInterface)
    if (m_supportsRaytracing) {
        if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&m_device5)))) {
            CFFLog::Warning("[DX12Context] Failed to query ID3D12Device5, disabling ray tracing");
            m_supportsRaytracing = false;
        }
    }

    // Check mesh shader support
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
        m_supportsMeshShaders = (options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
    }
}

// ============================================
// Command Queue
// ============================================

bool CDX12Context::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = DX12_CHECK(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateCommandQueue failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_commandQueue, "MainCommandQueue");
    return true;
}

// ============================================
// Swap Chain
// ============================================

bool CDX12Context::CreateSwapChain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = NUM_FRAMES_IN_FLIGHT;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = DX12_CHECK(m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),  // DX12: swap chain needs command queue, not device
        hwnd,
        &swapChainDesc,
        nullptr,  // No fullscreen desc
        nullptr,  // No restrict to output
        &swapChain1
    ));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateSwapChainForHwnd failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    // Disable Alt+Enter fullscreen toggle
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Get IDXGISwapChain3 for GetCurrentBackBufferIndex()
    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Failed to get IDXGISwapChain3: %s", HRESULTToString(hr).c_str());
        return false;
    }

    return true;
}

// ============================================
// RTV Heap and Backbuffer RTVs
// ============================================

bool CDX12Context::CreateRTVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = NUM_FRAMES_IN_FLIGHT;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;

    HRESULT hr = DX12_CHECK(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateDescriptorHeap (RTV) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    DX12_SET_DEBUG_NAME(m_rtvHeap, "BackbufferRTVHeap");
    return true;
}

bool CDX12Context::CreateImGuiSrvHeap() {
    // ImGui needs a shader-visible SRV heap for font textures and viewport textures
    // Slot 0: ImGui font texture
    // Slot 1+: Viewport textures (allocated dynamically)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = IMGUI_SRV_HEAP_SIZE;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    srvHeapDesc.NodeMask = 0;

    HRESULT hr = DX12_CHECK(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_imguiSrvHeap)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateDescriptorHeap (ImGui SRV) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_imguiSrvNextSlot = 1;  // Slot 0 reserved for font texture

    DX12_SET_DEBUG_NAME(m_imguiSrvHeap, "ImGuiSrvHeap");
    CFFLog::Info("[DX12Context] ImGui SRV heap created (%u descriptors)", IMGUI_SRV_HEAP_SIZE);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Context::GetImGuiSrvCpuHandle() const {
    return m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12Context::GetImGuiSrvGpuHandle() const {
    return m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12Context::AllocateImGuiTextureDescriptor(ID3D12Resource* texture, DXGI_FORMAT format) {
    if (!texture || !m_imguiSrvHeap) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    if (m_imguiSrvNextSlot >= IMGUI_SRV_HEAP_SIZE) {
        CFFLog::Error("[DX12Context] ImGui SRV heap full (max %u)", IMGUI_SRV_HEAP_SIZE);
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    uint32_t slot = m_imguiSrvNextSlot++;
    CFFLog::Info("[DX12Context] Allocated ImGui texture descriptor at slot %u", slot);
    return UpdateImGuiTextureDescriptor(slot, texture, format);
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12Context::UpdateImGuiTextureDescriptor(uint32_t slot, ID3D12Resource* texture, DXGI_FORMAT format) {
    if (!texture || !m_imguiSrvHeap || slot >= IMGUI_SRV_HEAP_SIZE || slot == 0) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    // Get CPU handle at the specified slot
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += slot * m_srvDescriptorSize;

    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    m_device->CreateShaderResourceView(texture, &srvDesc, cpuHandle);

    // Get GPU handle for the same slot
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += slot * m_srvDescriptorSize;

    return gpuHandle;
}

void CDX12Context::CreateBackbufferRTVs() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i]));
        if (FAILED(hr)) {
            CFFLog::Error("[DX12Context] GetBuffer(%u) failed: %s", i, HRESULTToString(hr).c_str());
            continue;
        }

        m_device->CreateRenderTargetView(m_backbuffers[i].Get(), nullptr, rtvHandle);

        DX12_SET_DEBUG_NAME_INDEXED(m_backbuffers[i], "Backbuffer", i);

        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Context::GetCurrentBackbufferRTV() const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_frameIndex * m_rtvDescriptorSize;
    return handle;
}

void CDX12Context::ReleaseBackbuffers() {
    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
        m_backbuffers[i].Reset();
    }
}

// ============================================
// Command Allocators
// ============================================

bool CDX12Context::CreateCommandAllocators() {
    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
        HRESULT hr = DX12_CHECK(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])
        ));

        if (FAILED(hr)) {
            CFFLog::Error("[DX12Context] CreateCommandAllocator(%u) failed: %s", i, HRESULTToString(hr).c_str());
            return false;
        }

        DX12_SET_DEBUG_NAME_INDEXED(m_commandAllocators[i], "CommandAllocator", i);
    }
    return true;
}

// ============================================
// Fence Synchronization
// ============================================

bool CDX12Context::CreateFence() {
    HRESULT hr = DX12_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateFence failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        CFFLog::Error("[DX12Context] CreateEvent failed");
        return false;
    }

    // Initialize fence values
    m_fenceValue = 0;
    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
        m_frameFenceValues[i] = 0;
    }

    DX12_SET_DEBUG_NAME(m_fence, "FrameFence");
    return true;
}

uint64_t CDX12Context::SignalFence() {
    uint64_t fenceValue = ++m_fenceValue;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceValue);
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Signal failed: %s", HRESULTToString(hr).c_str());
    }
    return fenceValue;
}

void CDX12Context::WaitForFenceValue(uint64_t fenceValue) {
    if (m_fence->GetCompletedValue() < fenceValue) {
        HRESULT hr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        if (FAILED(hr)) {
            CFFLog::Error("[DX12Context] SetEventOnCompletion failed: %s", HRESULTToString(hr).c_str());
            return;
        }
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void CDX12Context::WaitForGPU() {
    // Signal and wait for all pending work
    uint64_t fenceValue = SignalFence();
    WaitForFenceValue(fenceValue);
}

void CDX12Context::MoveToNextFrame() {
    // Signal the fence and record the value for the current frame
    uint64_t fenceValue = SignalFence();
    m_frameFenceValues[m_frameIndex] = fenceValue;

    // Update frame index
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait if the next frame's resources are still in use
    WaitForFenceValue(m_frameFenceValues[m_frameIndex]);

    // Process deferred deletions now that we know this fence value is complete
    uint64_t completedValue = m_fence->GetCompletedValue();
    m_deletionQueue.ProcessCompleted(completedValue);
}

// ============================================
// Resize
// ============================================

void CDX12Context::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    CFFLog::Info("[DX12Context] Resizing to %ux%u", width, height);

    // Wait for all GPU work to complete
    WaitForGPU();

    // Release backbuffer references
    ReleaseBackbuffers();

    // Resize swap chain buffers
    HRESULT hr = m_swapChain->ResizeBuffers(
        NUM_FRAMES_IN_FLIGHT,
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
    );

    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] ResizeBuffers failed: %s", HRESULTToString(hr).c_str());
        return;
    }

    m_width = width;
    m_height = height;

    // Recreate backbuffer RTVs
    CreateBackbufferRTVs();

    // Reset frame index
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Reset fence values
    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
        m_frameFenceValues[i] = m_fenceValue;
    }
}

// ============================================
// Deferred Deletion Wrappers
// ============================================

void CDX12Context::DeferredRelease(ID3D12Resource* resource) {
    if (resource) {
        m_deletionQueue.DeferredRelease(resource, m_fenceValue);
    }
}

void CDX12Context::DeferredRelease(ID3D12DescriptorHeap* heap) {
    if (heap) {
        m_deletionQueue.DeferredRelease(heap, m_fenceValue);
    }
}

} // namespace DX12
} // namespace RHI
