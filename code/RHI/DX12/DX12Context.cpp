#include "DX12Context.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

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

    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Failed to create DXGI factory: %s", HRESULTToString(hr).c_str());
        return false;
    }

    // Create device
    if (!CreateDevice()) {
        CFFLog::Error("[DX12Context] Failed to create D3D12 device");
        return false;
    }

    // Check feature support
    CheckFeatureSupport();

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
    m_device.Reset();
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

        // Optional: Enable GPU-based validation (very slow but thorough)
        // ComPtr<ID3D12Debug1> debugController1;
        // if (SUCCEEDED(debugController.As(&debugController1))) {
        //     debugController1->SetEnableGPUBasedValidation(true);
        //     CFFLog::Info("[DX12Context] GPU-based validation enabled");
        // }
    } else {
        CFFLog::Warning("[DX12Context] Failed to enable debug layer");
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
        HRESULT hr = D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,  // Minimum feature level
            IID_PPV_ARGS(&m_device)
        );

        if (SUCCEEDED(hr)) {
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
    HRESULT hr = m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] Failed to get WARP adapter: %s", HRESULTToString(hr).c_str());
        return false;
    }

    hr = D3D12CreateDevice(
        warpAdapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_device)
    );

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

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
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
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),  // DX12: swap chain needs command queue, not device
        hwnd,
        &swapChainDesc,
        nullptr,  // No fullscreen desc
        nullptr,  // No restrict to output
        &swapChain1
    );

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

    HRESULT hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateDescriptorHeap (RTV) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    DX12_SET_DEBUG_NAME(m_rtvHeap, "BackbufferRTVHeap");
    return true;
}

bool CDX12Context::CreateImGuiSrvHeap() {
    // ImGui needs a shader-visible SRV heap for font textures
    // We allocate a small heap just for ImGui (1 descriptor is enough for basic usage)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;  // Just for ImGui font texture
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    srvHeapDesc.NodeMask = 0;

    HRESULT hr = m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_imguiSrvHeap));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12Context] CreateDescriptorHeap (ImGui SRV) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    DX12_SET_DEBUG_NAME(m_imguiSrvHeap, "ImGuiSrvHeap");
    CFFLog::Info("[DX12Context] ImGui SRV heap created");
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE CDX12Context::GetImGuiSrvCpuHandle() const {
    return m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE CDX12Context::GetImGuiSrvGpuHandle() const {
    return m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
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
        HRESULT hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])
        );

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
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
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
    // Record the fence value for the current frame
    m_frameFenceValues[m_frameIndex] = m_fenceValue;

    // Signal the fence
    SignalFence();

    // Update frame index
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait if the next frame's resources are still in use
    WaitForFenceValue(m_frameFenceValues[m_frameIndex]);
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

} // namespace DX12
} // namespace RHI
