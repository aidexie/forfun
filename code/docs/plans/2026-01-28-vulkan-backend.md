# Vulkan Backend Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add Vulkan 1.3 backend to validate RHI abstraction and prepare for cross-platform support.

**Architecture:** Vulkan backend implements existing RHI interfaces (IRenderContext, ICommandList, IDescriptorSet) using Vulkan 1.3 dynamic rendering, VMA for memory management, and DXC for HLSL to SPIR-V compilation. Test-driven development with TestVulkanTriangle as the first milestone.

**Tech Stack:** Vulkan 1.3, VMA (Vulkan Memory Allocator), DXC with -spirv flag, Win32 surface (abstract later)

---

## Dependencies

**Vulkan SDK**: Install from https://vulkan.lunarg.com/
**VMA**: Download to E:/forfun/thirdparty/VMA/ from https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator

---

## Phase 1: Foundation (Get test to compile)

### Task 1.1: Add Vulkan Backend Enum

**Files:**
- Modify: RHI/RHICommon.h

**Step 1: Add Vulkan to EBackend enum**

In RHI/RHICommon.h, find EBackend enum and add Vulkan:

enum class EBackend {
    DX11,
    DX12,
    Vulkan  // NEW
};

**Step 2: Update BackendToString helper**

inline const char* BackendToString(EBackend backend) {
    switch (backend) {
        case EBackend::DX11: return "DX11";
        case EBackend::DX12: return "DX12";
        case EBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

**Step 3: Update StringToBackend helper**

inline EBackend StringToBackend(const char* str) {
    if (strcmp(str, "DX11") == 0) return EBackend::DX11;
    if (strcmp(str, "DX12") == 0) return EBackend::DX12;
    if (strcmp(str, "Vulkan") == 0) return EBackend::Vulkan;
    return EBackend::DX12;  // Default
}

**Step 4: Commit**

git add RHI/RHICommon.h
git commit -m "feat(rhi): add Vulkan backend enum"


---

### Task 1.2: Create VulkanCommon.h

**Files:**
- Create: RHI/Vulkan/VulkanCommon.h

**Step 1: Create the file with includes and macros**

Create RHI/Vulkan/VulkanCommon.h with:
- Vulkan includes with VK_USE_PLATFORM_WIN32_KHR
- VMA configuration macros
- VK_CHECK error checking macro
- VulkanDebugCallback for validation layer messages

Key macro:

#define VK_CHECK(call) do {     VkResult result_ = (call);     if (result_ != VK_SUCCESS) {         CFFLog::Error("Vulkan error: %s", VkResultToString(result_));         assert(false);     } } while(0)

**Step 2: Commit**

git add RHI/Vulkan/VulkanCommon.h
git commit -m "feat(rhi): add VulkanCommon.h with error checking"

---

### Task 1.3: Update CMakeLists.txt

**Files:**
- Modify: CMakeLists.txt

**Step 1: Add Vulkan SDK and VMA**

find_package(Vulkan REQUIRED)
set(VMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../thirdparty/VMA/include")
target_include_directories(forfun PRIVATE ${Vulkan_INCLUDE_DIRS} ${VMA_INCLUDE_DIR})
target_link_libraries(forfun PRIVATE Vulkan::Vulkan)

**Step 2: Add Vulkan source files**

set(VULKAN_SOURCES
    RHI/Vulkan/VulkanContext.cpp
    RHI/Vulkan/VulkanRenderContext.cpp
    RHI/Vulkan/VulkanCommandList.cpp
    RHI/Vulkan/VulkanResources.cpp
    RHI/Vulkan/VulkanPipelineState.cpp
    RHI/Vulkan/VulkanDescriptorSet.cpp
)
target_sources(forfun PRIVATE ${VULKAN_SOURCES})

**Step 3: Commit**

git add CMakeLists.txt
git commit -m "build: add Vulkan SDK and VMA to CMakeLists.txt"

---

### Task 1.4: Create Test Shaders

**Files:**
- Create: Shader/TestVulkan.vs.hlsl
- Create: Shader/TestVulkan.ps.hlsl

**Step 1: Create vertex shader (hardcoded triangle, no vertex buffer)**

Vertex shader uses SV_VertexID with static arrays for positions and colors.

**Step 2: Create pixel shader (passthrough color)**

Simple passthrough from vertex color to output.

**Step 3: Commit**

git add Shader/TestVulkan.vs.hlsl Shader/TestVulkan.ps.hlsl
git commit -m "feat(shader): add TestVulkan triangle shaders"

---

### Task 1.5: Create Test Case Skeleton

**Files:**
- Create: Tests/TestVulkanTriangle.cpp

**Step 1: Create minimal test skeleton**

- Override GetRequiredBackend() to return EBackend::Vulkan
- Setup/Teardown stubs
- OnFrame renders for 5 frames then screenshots and exits

**Step 2: Commit**

git add Tests/TestVulkanTriangle.cpp
git commit -m "test: add TestVulkanTriangle skeleton"

---

## Phase 2: Core Initialization (Get window to open)

### Task 2.1: Create VulkanContext - Instance and Device

**Files:**
- Create: RHI/Vulkan/VulkanContext.h
- Create: RHI/Vulkan/VulkanContext.cpp

**Step 1: Define CVulkanContext class**

class CVulkanContext {
public:
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    
    VkDevice GetDevice() const;
    VkPhysicalDevice GetPhysicalDevice() const;
    VkQueue GetGraphicsQueue() const;
    uint32_t GetGraphicsQueueFamily() const;
    VmaAllocator GetAllocator() const;
    
private:
    VkInstance m_instance;
    VkDebugUtilsMessengerEXT m_debugMessenger;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    VkQueue m_graphicsQueue;
    uint32_t m_graphicsQueueFamily;
    VmaAllocator m_vmaAllocator;
};

**Step 2: Implement CreateInstance()**

- Enable VK_LAYER_KHRONOS_validation in debug builds
- Enable VK_EXT_debug_utils for debug messenger
- Enable VK_KHR_surface and VK_KHR_win32_surface

**Step 3: Implement SelectPhysicalDevice()**

- Enumerate physical devices
- Select discrete GPU with Vulkan 1.3 support
- Check for required features (dynamicRendering, synchronization2, descriptorIndexing)

**Step 4: Implement CreateLogicalDevice()**

- Enable Vulkan 1.3 features
- Create graphics queue
- Enable required extensions (VK_KHR_swapchain)

**Step 5: Implement CreateVmaAllocator()**

- Configure VMA with Vulkan 1.3 functions
- Enable buffer device address

**Step 6: Commit**

git add RHI/Vulkan/VulkanContext.h RHI/Vulkan/VulkanContext.cpp
git commit -m "feat(rhi): add VulkanContext with instance and device creation"

---

### Task 2.2: Add Surface and Swapchain

**Files:**
- Modify: RHI/Vulkan/VulkanContext.h
- Modify: RHI/Vulkan/VulkanContext.cpp

**Step 1: Add surface and swapchain members**

VkSurfaceKHR m_surface;
VkSwapchainKHR m_swapchain;
VkFormat m_swapchainFormat;
VkExtent2D m_swapchainExtent;
std::vector<VkImage> m_swapchainImages;
std::vector<VkImageView> m_swapchainImageViews;

**Step 2: Implement CreateSurface()**

- Use vkCreateWin32SurfaceKHR with HWND

**Step 3: Implement CreateSwapchain()**

- Query surface capabilities
- Select B8G8R8A8_SRGB format, FIFO present mode
- Create swapchain with triple buffering
- Get swapchain images and create image views

**Step 4: Commit**

git add RHI/Vulkan/VulkanContext.h RHI/Vulkan/VulkanContext.cpp
git commit -m "feat(rhi): add Vulkan surface and swapchain"

---

### Task 2.3: Add Frame Synchronization

**Files:**
- Modify: RHI/Vulkan/VulkanContext.h
- Modify: RHI/Vulkan/VulkanContext.cpp

**Step 1: Add per-frame resources**

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
struct FrameData {
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
    VkFence inFlightFence;
};
std::array<FrameData, MAX_FRAMES_IN_FLIGHT> m_frames;
uint32_t m_currentFrame = 0;
uint32_t m_imageIndex = 0;

**Step 2: Implement CreateFrameResources()**

- Create command pool per frame (RESET_COMMAND_BUFFER flag)
- Allocate command buffer per frame
- Create semaphores and fences

**Step 3: Implement BeginFrame()**

- Wait for in-flight fence
- Acquire next swapchain image
- Reset fence and command pool
- Begin command buffer

**Step 4: Implement EndFrame()**

- End command buffer

**Step 5: Implement Present()**

- Submit with semaphore synchronization
- Present swapchain image
- Advance frame index

**Step 6: Commit**

git add RHI/Vulkan/VulkanContext.h RHI/Vulkan/VulkanContext.cpp
git commit -m "feat(rhi): add Vulkan frame synchronization"


---

## Phase 3: Resource Creation (Get PSO to build)

### Task 3.1: Add SPIR-V Compilation Path

**Files:**
- Modify: RHI/ShaderCompiler.h
- Modify: RHI/ShaderCompiler.cpp

**Step 1: Add EShaderTarget enum**

enum class EShaderTarget {
    DXIL,   // DX12
    SPIRV   // Vulkan
};

**Step 2: Extend CompileShader function**

Add target parameter with default DXIL.

**Step 3: Implement SPIR-V path in CompileShader**

Key DXC flags for SPIR-V:
- -spirv
- -fspv-target-env=vulkan1.3
- -fvk-invert-y (flip Y for Vulkan clip space)
- -fvk-use-dx-layout (keep DX matrix layout)
- -fvk-auto-shift-bindings (map register spaces to descriptor sets)

**Step 4: Commit**

git add RHI/ShaderCompiler.h RHI/ShaderCompiler.cpp
git commit -m "feat(rhi): add SPIR-V compilation path to ShaderCompiler"

---

### Task 3.2: Create CVulkanShader

**Files:**
- Create: RHI/Vulkan/VulkanResources.h
- Create: RHI/Vulkan/VulkanResources.cpp

**Step 1: Define CVulkanShader class**

class CVulkanShader : public IShader {
public:
    CVulkanShader(CVulkanContext* ctx, const void* bytecode, size_t size,
                  EShaderStage stage, const char* debugName);
    ~CVulkanShader();
    
    VkShaderModule GetModule() const { return m_module; }
    EShaderStage GetStage() const { return m_stage; }
    
private:
    CVulkanContext* m_ctx;
    VkShaderModule m_module;
    EShaderStage m_stage;
};

**Step 2: Implement constructor**

- Create VkShaderModule from SPIR-V bytecode
- Set debug name via vkSetDebugUtilsObjectNameEXT

**Step 3: Commit**

git add RHI/Vulkan/VulkanResources.h RHI/Vulkan/VulkanResources.cpp
git commit -m "feat(rhi): add CVulkanShader"

---

### Task 3.3: Create CVulkanPipelineState

**Files:**
- Create: RHI/Vulkan/VulkanPipelineState.h
- Create: RHI/Vulkan/VulkanPipelineState.cpp

**Step 1: Define CVulkanPipelineState class**

class CVulkanPipelineState : public IPipelineState {
public:
    CVulkanPipelineState(CVulkanContext* ctx, const SPipelineStateDesc& desc);
    ~CVulkanPipelineState();
    
    VkPipeline GetPipeline() const;
    VkPipelineLayout GetLayout() const;
    const SetBindingInfo& GetSetBindingInfo(uint32_t setIndex) const;
    IDescriptorSetLayout* GetExpectedLayout(uint32_t setIndex) const;
    
private:
    VkPipeline m_pipeline;
    VkPipelineLayout m_layout;
    std::array<SetBindingInfo, 4> m_setBindings;
    std::array<IDescriptorSetLayout*, 4> m_expectedLayouts;
};

**Step 2: Implement constructor with Vulkan 1.3 dynamic rendering**

Key points:
- Use VkPipelineRenderingCreateInfo (no VkRenderPass)
- Dynamic viewport/scissor state
- Translate all state from SPipelineStateDesc

**Step 3: Commit**

git add RHI/Vulkan/VulkanPipelineState.h RHI/Vulkan/VulkanPipelineState.cpp
git commit -m "feat(rhi): add CVulkanPipelineState with dynamic rendering"

---

### Task 3.4: Create Pipeline Layout Cache

**Files:**
- Modify: RHI/Vulkan/VulkanPipelineState.h
- Modify: RHI/Vulkan/VulkanPipelineState.cpp

**Step 1: Define CVulkanPipelineLayoutCache**

class CVulkanPipelineLayoutCache {
public:
    static CVulkanPipelineLayoutCache& Instance();
    VkPipelineLayout GetOrCreate(CVulkanContext* ctx, IDescriptorSetLayout* const* layouts);
    void Clear(VkDevice device);
    
private:
    std::unordered_map<CacheKey, VkPipelineLayout, CacheKeyHash> m_cache;
    std::mutex m_mutex;
};

**Step 2: Implement GetOrCreate**

- Hash layout pointers as cache key
- Create VkPipelineLayout from VkDescriptorSetLayouts
- Cache and return

**Step 3: Commit**

git add RHI/Vulkan/VulkanPipelineState.h RHI/Vulkan/VulkanPipelineState.cpp
git commit -m "feat(rhi): add CVulkanPipelineLayoutCache"

---

## Phase 4: Command Recording (Get triangle to render)

### Task 4.1: Create CVulkanCommandList - Basic Structure

**Files:**
- Create: RHI/Vulkan/VulkanCommandList.h
- Create: RHI/Vulkan/VulkanCommandList.cpp

**Step 1: Define CVulkanCommandList class**

class CVulkanCommandList : public ICommandList {
public:
    CVulkanCommandList(CVulkanContext* ctx);
    ~CVulkanCommandList();
    
    void Begin() override;
    void End() override;
    void SetPipelineState(IPipelineState* pso) override;
    void SetViewport(...) override;
    void SetScissorRect(...) override;
    void Draw(...) override;
    // ... other methods
    
private:
    CVulkanContext* m_ctx;
    VkCommandBuffer m_commandBuffer;
    CVulkanPipelineState* m_currentPSO;
    bool m_insideRenderPass;
};

**Step 2: Implement Begin/End**

- Get command buffer from context
- Track recording state

**Step 3: Commit**

git add RHI/Vulkan/VulkanCommandList.h RHI/Vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): add CVulkanCommandList basic structure"

---

### Task 4.2: Implement Dynamic Rendering

**Files:**
- Modify: RHI/Vulkan/VulkanCommandList.cpp

**Step 1: Add RenderingState tracking**

struct RenderingState {
    std::vector<VkRenderingAttachmentInfo> colorAttachments;
    VkRenderingAttachmentInfo depthAttachment;
    bool hasDepth;
    VkExtent2D extent;
};

**Step 2: Implement SetRenderTargets**

- End previous rendering if active
- Build VkRenderingAttachmentInfo for each RTV
- Build depth attachment if DSV provided
- Default to swapchain if no targets

**Step 3: Implement BeginRendering/EndRendering helpers**

- Use vkCmdBeginRendering/vkCmdEndRendering (Vulkan 1.3)
- Lazy begin on first draw

**Step 4: Commit**

git add RHI/Vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): implement Vulkan dynamic rendering"

---

### Task 4.3: Implement Draw Commands

**Files:**
- Modify: RHI/Vulkan/VulkanCommandList.cpp

**Step 1: Implement SetPipelineState**

vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->GetPipeline());

**Step 2: Implement SetViewport**

VkViewport viewport = {x, y, width, height, minDepth, maxDepth};
vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);

**Step 3: Implement SetScissorRect**

VkRect2D scissor = {{left, top}, {right - left, bottom - top}};
vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);

**Step 4: Implement Draw**

BeginRendering();  // Lazy begin
vkCmdDraw(m_commandBuffer, vertexCount, 1, startVertex, 0);

**Step 5: Implement DrawIndexed, DrawInstanced, DrawIndexedInstanced**

Similar pattern with vkCmdDrawIndexed, etc.

**Step 6: Commit**

git add RHI/Vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): implement Vulkan draw commands"

---

### Task 4.4: Implement Barriers and Clear

**Files:**
- Modify: RHI/Vulkan/VulkanCommandList.cpp

**Step 1: Implement Barrier for textures**

- End rendering before barriers
- Use VkImageMemoryBarrier2 with synchronization2
- Translate EResourceState to Vulkan stages/access/layout

**Step 2: Implement Barrier for buffers**

- Use VkBufferMemoryBarrier2

**Step 3: Implement ClearRenderTarget**

- End rendering
- Use vkCmdClearColorImage

**Step 4: Implement ClearDepthStencil**

- Use vkCmdClearDepthStencilImage

**Step 5: Implement BeginEvent/EndEvent**

- Use vkCmdBeginDebugUtilsLabelEXT/vkCmdEndDebugUtilsLabelEXT

**Step 6: Commit**

git add RHI/Vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): implement Vulkan barriers and clear commands"


---

## Phase 5: Integration (Get test to pass)

### Task 5.1: Create CVulkanRenderContext

**Files:**
- Create: RHI/Vulkan/VulkanRenderContext.h
- Create: RHI/Vulkan/VulkanRenderContext.cpp

**Step 1: Define CVulkanRenderContext class**

Implement IRenderContext interface with:
- Initialize/Shutdown
- BeginFrame/EndFrame/Present
- Resource creation methods
- GetCommandList/GetDescriptorSetAllocator

**Step 2: Implement Initialize**

- Create CVulkanContext
- Create CVulkanCommandList
- Create CVulkanDescriptorSetAllocator (stub for now)

**Step 3: Implement frame methods**

- BeginFrame: context->BeginFrame(), allocator->BeginFrame(), cmdList->Begin()
- EndFrame: cmdList->End()
- Present: context->Present()

**Step 4: Implement CreateShader**

- Compile HLSL to SPIR-V using ShaderCompiler with EShaderTarget::SPIRV
- Create CVulkanShader

**Step 5: Commit**

```
git add RHI/Vulkan/VulkanRenderContext.h RHI/Vulkan/VulkanRenderContext.cpp
git commit -m "feat(rhi): add CVulkanRenderContext"
```

---

### Task 5.2: Update RHIFactory

**Files:**
- Modify: RHI/RHIFactory.cpp

**Step 1: Add Vulkan include**

```cpp
#include "Vulkan/VulkanRenderContext.h"
```

**Step 2: Add Vulkan case to CreateRenderContext**

```cpp
case EBackend::Vulkan: {
    auto ctx = std::make_unique<CVulkanRenderContext>();
    if (!ctx->Initialize(static_cast<HWND>(nativeWindowHandle), width, height)) {
        CFFLog::Error("Failed to initialize Vulkan backend");
        return nullptr;
    }
    return ctx;
}
```

**Step 3: Commit**

```
git add RHI/RHIFactory.cpp
git commit -m "feat(rhi): add Vulkan to RHIFactory"
```

---

### Task 5.3: Add Backend Override to Test Framework

**Files:**
- Modify: Testing/TestFramework.h
- Modify: Testing/TestFramework.cpp

**Step 1: Add GetRequiredBackend to ITestCase**

```cpp
virtual std::optional<EBackend> GetRequiredBackend() const { return std::nullopt; }
```

**Step 2: Update test runner to check for override**

```cpp
EBackend backend = LoadBackendFromConfig();
if (auto required = test->GetRequiredBackend()) {
    backend = *required;
    CFFLog::Info("Test requires backend: %s", BackendToString(backend));
}
```

**Step 3: Commit**

```
git add Testing/TestFramework.h Testing/TestFramework.cpp
git commit -m "feat(test): add backend override support to test framework"
```

---

### Task 5.4: Complete TestVulkanTriangle

**Files:**
- Modify: Tests/TestVulkanTriangle.cpp

**Step 1: Implement Setup**

- Compile TestVulkan.vs.hlsl and TestVulkan.ps.hlsl to SPIR-V
- Create PSO with minimal state

**Step 2: Implement OnFrame**

- Clear swapchain to black
- Set viewport and scissor
- Set PSO
- Draw(3, 0) for triangle
- Screenshot on frame 5

**Step 3: Run test**

```
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestVulkanTriangle
```

Expected: Window opens, RGB triangle renders, screenshot saved

**Step 4: Verify**

- Check E:/forfun/debug/TestVulkanTriangle/runtime.log for errors
- Check E:/forfun/debug/TestVulkanTriangle/screenshot_frame5.png for RGB triangle

**Step 5: Commit**

```
git add Tests/TestVulkanTriangle.cpp
git commit -m "test: complete TestVulkanTriangle implementation"
```

---

## Phase 6: Descriptor Sets (Full feature parity)

### Task 6.1: Create CVulkanDescriptorSetLayout

**Files:**
- Create: RHI/Vulkan/VulkanDescriptorSet.h
- Create: RHI/Vulkan/VulkanDescriptorSet.cpp

**Step 1: Define CVulkanDescriptorSetLayout**

Implement IDescriptorSetLayout with:
- GetBindingCount, GetBinding, GetDebugName
- GetLayout() returning VkDescriptorSetLayout
- GetPoolSizes() for allocation sizing

**Step 2: Implement constructor**

- Translate BindingLayoutItems to VkDescriptorSetLayoutBindings
- Create VkDescriptorSetLayout
- Compute pool sizes for allocation

**Step 3: Commit**

```
git add RHI/Vulkan/VulkanDescriptorSet.h RHI/Vulkan/VulkanDescriptorSet.cpp
git commit -m "feat(rhi): add CVulkanDescriptorSetLayout"
```

---

### Task 6.2: Create CVulkanDescriptorSet

**Files:**
- Modify: RHI/Vulkan/VulkanDescriptorSet.h
- Modify: RHI/Vulkan/VulkanDescriptorSet.cpp

**Step 1: Define CVulkanDescriptorSet**

Implement IDescriptorSet with:
- Bind(item) and Bind(items, count)
- GetLayout, IsComplete
- GetDescriptorSet() returning VkDescriptorSet

**Step 2: Implement Bind**

- Translate BindingSetItem to VkWriteDescriptorSet
- Handle Texture_SRV, Buffer_SRV, Sampler, VolatileCBV, etc.
- Batch writes and flush with vkUpdateDescriptorSets

**Step 3: Commit**

```
git add RHI/Vulkan/VulkanDescriptorSet.h RHI/Vulkan/VulkanDescriptorSet.cpp
git commit -m "feat(rhi): add CVulkanDescriptorSet"
```

---

### Task 6.3: Create CVulkanDescriptorSetAllocator

**Files:**
- Create: RHI/Vulkan/VulkanDescriptorSetAllocator.h
- Create: RHI/Vulkan/VulkanDescriptorSetAllocator.cpp

**Step 1: Define CVulkanDescriptorSetAllocator**

Implement IDescriptorSetAllocator with:
- CreateLayout
- AllocatePersistentSet / AllocateTransientSet
- FreePersistentSet
- BeginFrame

**Step 2: Implement pool management**

- Persistent pool: grows as needed, supports individual free
- Transient pools: per-frame (3 pools), reset on BeginFrame via vkResetDescriptorPool

**Step 3: Commit**

```
git add RHI/Vulkan/VulkanDescriptorSetAllocator.h RHI/Vulkan/VulkanDescriptorSetAllocator.cpp
git commit -m "feat(rhi): add CVulkanDescriptorSetAllocator"
```

---

### Task 6.4: Implement BindDescriptorSet in CommandList

**Files:**
- Modify: RHI/Vulkan/VulkanCommandList.cpp

**Step 1: Implement BindDescriptorSet**

```cpp
void CVulkanCommandList::BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) {
    assert(m_currentPSO && "Must set PSO before binding descriptor sets");
    
    const auto& bindingInfo = m_currentPSO->GetSetBindingInfo(setIndex);
    if (!bindingInfo.isUsed) return;
    
    assert(set->GetLayout() == m_currentPSO->GetExpectedLayout(setIndex));
    
    auto* vkSet = static_cast<CVulkanDescriptorSet*>(set);
    VkDescriptorSet descriptorSet = vkSet->GetDescriptorSet();
    
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_currentPSO->GetLayout(), setIndex, 1,
                            &descriptorSet, 0, nullptr);
}
```

**Step 2: Commit**

```
git add RHI/Vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): implement BindDescriptorSet in Vulkan command list"
```

---

### Task 6.5: Add Buffer and Texture Resources

**Files:**
- Modify: RHI/Vulkan/VulkanResources.h
- Modify: RHI/Vulkan/VulkanResources.cpp

**Step 1: Implement CVulkanBuffer**

- Use VMA for allocation
- Support Map/Unmap for CPU-accessible buffers
- Get buffer device address via vkGetBufferDeviceAddress

**Step 2: Implement CVulkanTexture**

- Use VMA for allocation
- Create default image view
- Support subresource views for mip/slice access

**Step 3: Implement CVulkanSampler**

- Translate SSamplerDesc to VkSamplerCreateInfo
- Create VkSampler

**Step 4: Commit**

```
git add RHI/Vulkan/VulkanResources.h RHI/Vulkan/VulkanResources.cpp
git commit -m "feat(rhi): add CVulkanBuffer, CVulkanTexture, CVulkanSampler"
```


---

## Phase 7: Validation and Cleanup

### Task 7.1: Add Debug Names to All Objects

**Files:**
- Modify: All RHI/Vulkan/*.cpp files

**Step 1: Add SetDebugName helper**

```cpp
inline void SetVulkanObjectName(VkDevice device, VkObjectType type, 
                                 uint64_t handle, const char* name) {
    if (!name) return;
    VkDebugUtilsObjectNameInfoEXT nameInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    nameInfo.objectType = type;
    nameInfo.objectHandle = handle;
    nameInfo.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
}
```

**Step 2: Apply to all resource types**

- Buffers: VK_OBJECT_TYPE_BUFFER
- Textures: VK_OBJECT_TYPE_IMAGE, VK_OBJECT_TYPE_IMAGE_VIEW
- Samplers: VK_OBJECT_TYPE_SAMPLER
- Pipelines: VK_OBJECT_TYPE_PIPELINE
- Descriptor sets: VK_OBJECT_TYPE_DESCRIPTOR_SET

**Step 3: Commit**

```
git add RHI/Vulkan/*.cpp
git commit -m "feat(rhi): add debug names to all Vulkan objects"
```

---

### Task 7.2: Implement Proper Shutdown

**Files:**
- Modify: RHI/Vulkan/VulkanContext.cpp
- Modify: RHI/Vulkan/VulkanRenderContext.cpp

**Step 1: Implement CVulkanContext::Shutdown**

Order matters - destroy in reverse creation order:
1. Wait for GPU idle: vkDeviceWaitIdle
2. Destroy frame resources (fences, semaphores, command pools)
3. Destroy swapchain image views
4. Destroy swapchain
5. Destroy surface
6. Destroy VMA allocator
7. Destroy device
8. Destroy debug messenger
9. Destroy instance

**Step 2: Implement CVulkanRenderContext::Shutdown**

1. Destroy descriptor allocator
2. Destroy command list
3. Destroy context

**Step 3: Verify no leaks**

Run test with validation layers - should report no leaks on exit.

**Step 4: Commit**

```
git add RHI/Vulkan/VulkanContext.cpp RHI/Vulkan/VulkanRenderContext.cpp
git commit -m "feat(rhi): implement proper Vulkan shutdown"
```

---

### Task 7.3: Create TestVulkanDescriptorSet

**Files:**
- Create: Tests/TestVulkanDescriptorSet.cpp

**Step 1: Create test case**

Test the 4-set binding model:
- Create PerFrame layout with textures and samplers
- Create PerMaterial layout with textures
- Allocate persistent and transient sets
- Bind resources and render

**Step 2: Implement test**

```cpp
class CTestVulkanDescriptorSet : public ITestCase {
    std::optional<RHI::EBackend> GetRequiredBackend() const override {
        return RHI::EBackend::Vulkan;
    }
    
    void Setup() override {
        // Create layouts matching DX12 4-set model
        // Create PSO with layouts
        // Allocate sets and bind resources
    }
    
    void OnFrame(uint32_t frameIndex) override {
        // Update volatile CBV
        // Bind sets
        // Draw
    }
};
```

**Step 3: Run and verify**

```
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestVulkanDescriptorSet
```

**Step 4: Commit**

```
git add Tests/TestVulkanDescriptorSet.cpp
git commit -m "test: add TestVulkanDescriptorSet"
```

---

### Task 7.4: Run Existing Tests with Vulkan

**Files:**
- Modify: render.json (temporarily)

**Step 1: Set Vulkan as default backend**

```json
{
  "renderBackend": "Vulkan"
}
```

**Step 2: Run test suite**

Run key tests that exercise the rendering pipeline:
- TestTriangle (basic rendering)
- TestDescriptorSet (descriptor binding)
- TestDeferredLighting (if migrated)

**Step 3: Fix any issues**

Address validation errors or rendering differences.

**Step 4: Restore DX12 as default**

```json
{
  "renderBackend": "DX12"
}
```

**Step 5: Commit any fixes**

```
git add -A
git commit -m "fix(rhi): address Vulkan compatibility issues"
```

---

## Summary

**Total Tasks:** 25 tasks across 7 phases

**Key Milestones:**
1. Phase 1 complete: Project compiles with Vulkan stubs
2. Phase 2 complete: Window opens with Vulkan
3. Phase 5 complete: TestVulkanTriangle passes (MVP)
4. Phase 6 complete: Full descriptor set support
5. Phase 7 complete: Production-ready Vulkan backend

**Success Criteria:**
- No validation layer errors
- TestVulkanTriangle renders RGB triangle
- TestVulkanDescriptorSet validates 4-set model
- Clean shutdown with no leaks
- Existing tests pass with Vulkan backend

**Files Created:**
- RHI/Vulkan/VulkanCommon.h
- RHI/Vulkan/VulkanContext.h/cpp
- RHI/Vulkan/VulkanRenderContext.h/cpp
- RHI/Vulkan/VulkanCommandList.h/cpp
- RHI/Vulkan/VulkanResources.h/cpp
- RHI/Vulkan/VulkanPipelineState.h/cpp
- RHI/Vulkan/VulkanDescriptorSet.h/cpp
- RHI/Vulkan/VulkanDescriptorSetAllocator.h/cpp
- Shader/TestVulkan.vs.hlsl
- Shader/TestVulkan.ps.hlsl
- Tests/TestVulkanTriangle.cpp
- Tests/TestVulkanDescriptorSet.cpp

**Files Modified:**
- RHI/RHICommon.h
- RHI/RHIFactory.cpp
- RHI/ShaderCompiler.h/cpp
- Testing/TestFramework.h/cpp
- CMakeLists.txt
