# GBufferPass Descriptor Set Migration

**Date**: 2026-01-28
**Status**: Plan
**Related**: `2026-01-26-descriptor-set-ownership-design.md`, `2026-01-26-deferred-lighting-descriptor-set-migration.md`

---

## Overview

Migrate `CGBufferPass` from legacy per-slot binding (`SetShaderResource`, `SetConstantBufferData`) to the 4-set descriptor model. This completes the core deferred pipeline migration (GBuffer → DeferredLighting).

**Sets Used:**
- **Set 0 (PerFrame, space0)**: From RenderPipeline - samplers, global resources
- **Set 1 (PerPass, space1)**: Owned by GBufferPass - CB_GBufferFrame, Lightmap
- **Set 2 (PerMaterial, space2)**: Owned by CMaterialAsset - textures, CB_Material
- **Set 3 (PerDraw, space3)**: Owned by GBufferPass - CB_PerDraw (VolatileCBV)

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Legacy support | None | DX12-only, clean migration |
| PerDraw binding | VolatileCBV | Push constants too small (128B limit) |
| Material ownership | CMaterialAsset owns Set 2 | Natural ownership, dirty tracking |
| Per-object data | CB_PerDraw with World, WorldPrev, lightmapIndex | All per-object data in one CB |

---

## Constant Buffer Structures

### CB_GBufferFrame (Set 1, b0 space1)

```cpp
struct alignas(16) CB_GBufferFrame {
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX viewProjPrev;
    XMFLOAT3 camPosWS;
    float _pad0;
};
```

### CB_Material (Set 2, b0 space2)

```cpp
struct alignas(16) CB_Material {
    XMFLOAT3 albedo;
    float metallic;                    // 16 bytes

    XMFLOAT3 emissive;
    float roughness;                   // 16 bytes

    float emissiveStrength;
    int hasMetallicRoughnessTexture;
    int hasEmissiveMap;
    int alphaMode;                     // 16 bytes

    float alphaCutoff;
    float materialID;
    float _pad[2];                     // 16 bytes

    // Total: 64 bytes
};
```

### CB_PerDraw (Set 3, b0 space3)

```cpp
struct alignas(16) CB_PerDraw {
    XMFLOAT4X4 World;           // 64 bytes
    XMFLOAT4X4 WorldPrev;       // 64 bytes
    int lightmapIndex;
    int objectID;
    float _pad[2];              // 16 bytes
    // Total: 144 bytes
};
```

---

## Set Layouts

### Set 1: PerPass (GBuffer-specific)

```cpp
BindingLayoutDesc("GBuffer_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_GBufferFrame)))
    .AddItem(BindingLayoutItem::Texture_SRV(12))           // Lightmap atlas
    .AddItem(BindingLayoutItem::StructuredBuffer_SRV(13))  // Lightmap infos
    .AddItem(BindingLayoutItem::Sampler(2))                // Lightmap sampler
```

### Set 2: PerMaterial (Shared PBR layout)

```cpp
BindingLayoutDesc("PBRMaterial")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_Material)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Albedo
    .AddItem(BindingLayoutItem::Texture_SRV(1))   // Normal
    .AddItem(BindingLayoutItem::Texture_SRV(2))   // MetallicRoughness
    .AddItem(BindingLayoutItem::Texture_SRV(3))   // Emissive
    .AddItem(BindingLayoutItem::Sampler(0))       // Material sampler
```

### Set 3: PerDraw

```cpp
BindingLayoutDesc("PerDraw")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_PerDraw)))
```

---

## Implementation Plan

### Phase 1: Infrastructure Updates

#### 1.1 Update PerDrawSlots.h

Change from push constants to VolatileCBV:

```cpp
// RHI/PerDrawSlots.h
namespace PerDrawSlots {

namespace CB {
    constexpr uint32_t PerDraw = 0;
}

struct CB_PerDraw {
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 WorldPrev;
    int lightmapIndex;
    int objectID;
    float _pad[2];
};

} // namespace PerDrawSlots
```

#### 1.2 Update PerDrawLayout

```cpp
// Engine/Rendering/PerDrawLayout.h
inline IDescriptorSetLayout* CreatePerDrawLayout(IRenderContext* ctx) {
    return ctx->CreateDescriptorSetLayout(
        BindingLayoutDesc("PerDraw")
            .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(PerDrawSlots::CB_PerDraw)))
    );
}
```

#### 1.3 Add GBuffer PerPass Layout to PassLayouts.h

```cpp
// Engine/Rendering/PassLayouts.h
inline IDescriptorSetLayout* CreateGBufferPassLayout(IRenderContext* ctx) {
    return ctx->CreateDescriptorSetLayout(
        BindingLayoutDesc("GBuffer_PerPass")
            .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_GBufferFrame)))
            .AddItem(BindingLayoutItem::Texture_SRV(12))
            .AddItem(BindingLayoutItem::StructuredBuffer_SRV(13))
            .AddItem(BindingLayoutItem::Sampler(2))
    );
}
```

---

### Phase 2: Material System Integration

#### 2.1 Create CB_Material struct

**File**: `Engine/Material/MaterialConstants.h`

```cpp
struct alignas(16) CB_Material {
    DirectX::XMFLOAT3 albedo;
    float metallic;

    DirectX::XMFLOAT3 emissive;
    float roughness;

    float emissiveStrength;
    int hasMetallicRoughnessTexture;
    int hasEmissiveMap;
    int alphaMode;

    float alphaCutoff;
    float materialID;
    float _pad[2];
};
```

#### 2.2 Add Descriptor Set to CMaterialAsset

**File**: `Core/MaterialManager.h`

```cpp
class CMaterialAsset {
public:
    // Existing fields...

    // New: Descriptor set for DS path
    IDescriptorSet* GetDescriptorSet() const { return m_descriptorSet; }
    void InitializeDescriptorSet(IRenderContext* ctx, IDescriptorSetLayout* layout);
    void UpdateDescriptorSet();
    void BindToCommandList(ICommandList* cmdList);

private:
    IDescriptorSet* m_descriptorSet = nullptr;
    bool m_descriptorSetDirty = true;
};
```

#### 2.3 Implement Material Descriptor Set

**File**: `Core/MaterialManager.cpp`

```cpp
void CMaterialAsset::InitializeDescriptorSet(IRenderContext* ctx, IDescriptorSetLayout* layout) {
    if (!m_descriptorSet) {
        m_descriptorSet = ctx->AllocateDescriptorSet(layout);
    }
    m_descriptorSetDirty = true;
}

void CMaterialAsset::UpdateDescriptorSet() {
    if (!m_descriptorSet || !m_descriptorSetDirty) return;

    auto& texMgr = CTextureManager::Instance();

    m_descriptorSet->Bind({
        BindingSetItem::Texture_SRV(0, GetAlbedoTexture()),
        BindingSetItem::Texture_SRV(1, GetNormalTexture()),
        BindingSetItem::Texture_SRV(2, GetMetallicRoughnessTexture()),
        BindingSetItem::Texture_SRV(3, GetEmissiveTexture()),
        BindingSetItem::Sampler(0, texMgr.GetDefaultSampler()),
    });

    m_descriptorSetDirty = false;
}

void CMaterialAsset::BindToCommandList(ICommandList* cmdList) {
    UpdateDescriptorSet();

    CB_Material cb;
    cb.albedo = albedo;
    cb.metallic = metallic;
    cb.emissive = emissive;
    cb.roughness = roughness;
    cb.emissiveStrength = emissiveStrength;
    cb.hasMetallicRoughnessTexture = !metallicRoughnessMap.empty() ? 1 : 0;
    cb.hasEmissiveMap = !emissiveMap.empty() ? 1 : 0;
    cb.alphaMode = static_cast<int>(alphaMode);
    cb.alphaCutoff = alphaCutoff;
    cb.materialID = static_cast<float>(materialType);

    m_descriptorSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(cb)));
    cmdList->BindDescriptorSet(2, m_descriptorSet);
}
```

---

### Phase 3: Shader Creation

#### 3.1 Create GBuffer_DS.vs.hlsl

**File**: `Shader/GBuffer_DS.vs.hlsl`

```hlsl
// SM 5.1 vertex shader with register spaces

//==============================================
// Set 0: PerFrame (space0)
//==============================================
SamplerState gLinearWrap : register(s1, space0);

//==============================================
// Set 1: PerPass (space1)
//==============================================
cbuffer CB_GBufferFrame : register(b0, space1) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProjPrev;
    float3 gCamPosWS;
    float _pad0;
};

//==============================================
// Set 3: PerDraw (space3)
//==============================================
cbuffer CB_PerDraw : register(b0, space3) {
    float4x4 gWorld;
    float4x4 gWorldPrev;
    int gLightmapIndex;
    int gObjectID;
    float2 _padDraw;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
    float4 color    : COLOR;
    float2 uv2      : TEXCOORD1;
};

struct VSOutput {
    float4 posH       : SV_Position;
    float3 posW       : TEXCOORD0;
    float3 normalW    : TEXCOORD1;
    float2 uv         : TEXCOORD2;
    float4 tangentW   : TEXCOORD3;
    float4 color      : TEXCOORD4;
    float2 uv2        : TEXCOORD5;
    float4 posCurr    : TEXCOORD6;
    float4 posPrev    : TEXCOORD7;
    nointerpolation int lightmapIndex : TEXCOORD8;
};

VSOutput main(VSInput input) {
    VSOutput o;

    float4 posW = mul(float4(input.position, 1.0), gWorld);
    o.posW = posW.xyz;
    o.posH = mul(mul(posW, gView), gProj);

    o.normalW = normalize(mul(input.normal, (float3x3)gWorld));
    o.tangentW = float4(normalize(mul(input.tangent.xyz, (float3x3)gWorld)), input.tangent.w);

    o.uv = input.uv;
    o.uv2 = input.uv2;
    o.color = input.color;
    o.lightmapIndex = gLightmapIndex;

    // Velocity
    o.posCurr = o.posH;
    float4 posPrevW = mul(float4(input.position, 1.0), gWorldPrev);
    o.posPrev = mul(posPrevW, gViewProjPrev);

    return o;
}
```

#### 3.2 Create GBuffer_DS.ps.hlsl

**File**: `Shader/GBuffer_DS.ps.hlsl`

```hlsl
// SM 5.1 pixel shader with register spaces

//==============================================
// Set 0: PerFrame (space0)
//==============================================
SamplerState gLinearWrap : register(s1, space0);

//==============================================
// Set 1: PerPass (space1)
//==============================================
Texture2D gLightmapAtlas : register(t12, space1);
StructuredBuffer<LightmapInfo> gLightmapInfos : register(t13, space1);
SamplerState gLightmapSampler : register(s2, space1);

//==============================================
// Set 2: PerMaterial (space2)
//==============================================
cbuffer CB_Material : register(b0, space2) {
    float3 gAlbedo;
    float gMetallic;
    float3 gEmissive;
    float gRoughness;
    float gEmissiveStrength;
    int gHasMetallicRoughnessTexture;
    int gHasEmissiveMap;
    int gAlphaMode;
    float gAlphaCutoff;
    float gMaterialID;
    float2 _padMat;
};

Texture2D gAlbedoTex            : register(t0, space2);
Texture2D gNormalTex            : register(t1, space2);
Texture2D gMetallicRoughnessTex : register(t2, space2);
Texture2D gEmissiveTex          : register(t3, space2);
SamplerState gMaterialSampler   : register(s0, space2);

struct PSInput {
    float4 posH       : SV_Position;
    float3 posW       : TEXCOORD0;
    float3 normalW    : TEXCOORD1;
    float2 uv         : TEXCOORD2;
    float4 tangentW   : TEXCOORD3;
    float4 color      : TEXCOORD4;
    float2 uv2        : TEXCOORD5;
    float4 posCurr    : TEXCOORD6;
    float4 posPrev    : TEXCOORD7;
    nointerpolation int lightmapIndex : TEXCOORD8;
};

struct PSOutput {
    float4 WorldPosMetallic    : SV_Target0;
    float4 NormalRoughness     : SV_Target1;
    float4 AlbedoAO            : SV_Target2;
    float4 EmissiveMaterialID  : SV_Target3;
    float2 Velocity            : SV_Target4;
};

PSOutput main(PSInput input) {
    PSOutput o;

    // Sample textures
    float4 albedoSample = gAlbedoTex.Sample(gMaterialSampler, input.uv);
    float3 albedo = albedoSample.rgb * gAlbedo;
    float alpha = albedoSample.a;

    // Alpha test
    if (gAlphaMode == 1 && alpha < gAlphaCutoff) {
        discard;
    }

    // Normal mapping
    float3 N = normalize(input.normalW);
    float3 T = normalize(input.tangentW.xyz);
    float3 B = cross(N, T) * input.tangentW.w;
    float3x3 TBN = float3x3(T, B, N);

    float3 normalSample = gNormalTex.Sample(gMaterialSampler, input.uv).xyz * 2.0 - 1.0;
    float3 normal = normalize(mul(normalSample, TBN));

    // Metallic/Roughness
    float metallic = gMetallic;
    float roughness = gRoughness;
    if (gHasMetallicRoughnessTexture) {
        float2 mr = gMetallicRoughnessTex.Sample(gMaterialSampler, input.uv).bg;
        metallic = mr.x;
        roughness = mr.y;
    }

    // Emissive
    float3 emissive = gEmissive * gEmissiveStrength;
    if (gHasEmissiveMap) {
        emissive *= gEmissiveTex.Sample(gMaterialSampler, input.uv).rgb;
    }

    // AO from lightmap (if available)
    float ao = 1.0;
    if (input.lightmapIndex >= 0) {
        // Sample lightmap...
    }

    // Velocity
    float2 currPos = input.posCurr.xy / input.posCurr.w;
    float2 prevPos = input.posPrev.xy / input.posPrev.w;
    float2 velocity = (currPos - prevPos) * 0.5;

    // Output
    o.WorldPosMetallic = float4(input.posW, metallic);
    o.NormalRoughness = float4(normal * 0.5 + 0.5, roughness);
    o.AlbedoAO = float4(albedo, ao);
    o.EmissiveMaterialID = float4(emissive, gMaterialID / 255.0);
    o.Velocity = velocity;

    return o;
}
```

---

### Phase 4: GBufferPass Migration

#### 4.1 Update GBufferPass.h

```cpp
class CGBufferPass {
public:
    bool Initialize();
    void Shutdown();

    // New signature with perFrameSet
    void Render(
        const CCamera& camera,
        CScene& scene,
        CGBuffer& gbuffer,
        const DirectX::XMMATRIX& viewProjPrev,
        uint32_t width, uint32_t height,
        IDescriptorSet* perFrameSet);

    bool IsDescriptorSetModeAvailable() const { return m_perPassSet != nullptr; }

private:
    // Existing
    std::unique_ptr<IShader> m_vs;
    std::unique_ptr<IShader> m_ps;
    std::unique_ptr<IPipelineState> m_pso;
    std::unique_ptr<ISampler> m_sampler;

    // New: Descriptor set resources
    IDescriptorSetLayout* m_perPassLayout = nullptr;
    IDescriptorSetLayout* m_perMaterialLayout = nullptr;
    IDescriptorSetLayout* m_perDrawLayout = nullptr;
    IDescriptorSet* m_perPassSet = nullptr;
    IDescriptorSet* m_perDrawSet = nullptr;

    // SM 5.1 shaders
    std::unique_ptr<IShader> m_vs_DS;
    std::unique_ptr<IShader> m_ps_DS;
    std::unique_ptr<IPipelineState> m_pso_DS;
};
```

#### 4.2 Update GBufferPass.cpp Initialize()

Add descriptor set initialization after existing code:

```cpp
bool CGBufferPass::Initialize() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // ... existing shader compilation and PSO creation ...

    // === Descriptor Set Path (DX12 only) ===
    if (ctx->SupportsDescriptorSets()) {
        // Create layouts
        m_perPassLayout = PassLayouts::CreateGBufferPassLayout(ctx);
        m_perMaterialLayout = MaterialLayout::CreatePBRMaterialLayout(ctx);
        m_perDrawLayout = PerDrawLayout::CreatePerDrawLayout(ctx);

        // Allocate sets
        m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
        m_perDrawSet = ctx->AllocateDescriptorSet(m_perDrawLayout);

        // Compile SM 5.1 shaders
        std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

        std::string vsSource = LoadShaderSource(shaderDir + "GBuffer_DS.vs.hlsl");
        SCompiledShader vsCompiled = CompileShaderFromSource(
            vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);

        std::string psSource = LoadShaderSource(shaderDir + "GBuffer_DS.ps.hlsl");
        SCompiledShader psCompiled = CompileShaderFromSource(
            psSource.c_str(), "main", "ps_5_1", &includeHandler, debugShaders);

        // Create shaders and PSO with descriptor set layouts
        // psoDesc.descriptorSetLayouts[0] = ctx->GetPerFrameLayout();
        // psoDesc.descriptorSetLayouts[1] = m_perPassLayout;
        // psoDesc.descriptorSetLayouts[2] = m_perMaterialLayout;
        // psoDesc.descriptorSetLayouts[3] = m_perDrawLayout;

        CFFLog::Info("GBufferPass: Descriptor set path initialized");
    }

    return true;
}
```

#### 4.3 Update GBufferPass.cpp Render()

```cpp
void CGBufferPass::Render(
    const CCamera& camera,
    CScene& scene,
    CGBuffer& gbuffer,
    const DirectX::XMMATRIX& viewProjPrev,
    uint32_t width, uint32_t height,
    IDescriptorSet* perFrameSet)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    RHI::CScopedDebugEvent evt(cmdList, L"G-Buffer Pass (DS)");

    // Set render targets, viewport, clear...

    // Set PSO
    cmdList->SetPipelineState(m_pso_DS.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Bind Set 0 (PerFrame)
    cmdList->BindDescriptorSet(0, perFrameSet);

    // Bind Set 1 (PerPass)
    CB_GBufferFrame frameData;
    frameData.view = XMMatrixTranspose(camera.GetViewMatrix());
    frameData.proj = XMMatrixTranspose(camera.GetJitteredProjectionMatrix(width, height));
    frameData.viewProjPrev = XMMatrixTranspose(viewProjPrev);
    frameData.camPosWS = camera.position;

    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(0, &frameData, sizeof(frameData)),
        BindingSetItem::Texture_SRV(12, scene.GetLightmap2D().GetAtlas()),
        BindingSetItem::StructuredBuffer_SRV(13, scene.GetLightmap2D().GetInfoBuffer()),
        BindingSetItem::Sampler(2, m_sampler.get()),
    });
    cmdList->BindDescriptorSet(1, m_perPassSet);

    // Render objects
    CMaterialAsset* lastMaterial = nullptr;

    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) continue;

        CMaterialAsset* material = meshRenderer->GetMaterial();

        // Bind Set 2 (PerMaterial) - on material change
        if (material != lastMaterial) {
            material->BindToCommandList(cmdList);
            lastMaterial = material;
        }

        // Bind Set 3 (PerDraw)
        PerDrawSlots::CB_PerDraw perDraw;
        perDraw.World = XMMatrixTranspose(transform->WorldMatrix());
        perDraw.WorldPrev = XMMatrixTranspose(transform->WorldMatrix());
        perDraw.lightmapIndex = meshRenderer->lightmapInfosIndex;
        perDraw.objectID = obj->GetID();

        m_perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
        cmdList->BindDescriptorSet(3, m_perDrawSet);

        // Draw meshes
        for (auto& gpuMesh : meshRenderer->meshes) {
            cmdList->SetVertexBuffer(0, gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
            cmdList->SetIndexBuffer(gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
            cmdList->DrawIndexed(gpuMesh->indexCount, 0, 0);
        }
    }
}
```

---

### Phase 5: Integration

#### 5.1 Update DeferredRenderPipeline.cpp

```cpp
void CDeferredRenderPipeline::Render(const RenderContext& ctx) {
    // ...

    // G-Buffer Pass
    {
        if (m_perFrameSet && m_gbufferPass.IsDescriptorSetModeAvailable()) {
            m_gbufferPass.Render(ctx.camera, ctx.scene, m_gbuffer,
                                 m_viewProjPrev, ctx.width, ctx.height,
                                 m_perFrameSet);
        } else {
            // Legacy path (removed - DX12 only)
        }
    }

    // ...
}
```

#### 5.2 Initialize Material Descriptor Sets

In `CMaterialManager::Load()`:

```cpp
CMaterialAsset* CMaterialManager::Load(const std::string& path) {
    // ... existing loading code ...

    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx && ctx->SupportsDescriptorSets()) {
        material->InitializeDescriptorSet(ctx, GetPBRMaterialLayout());
    }

    return material;
}
```

---

### Phase 6: Testing

#### 6.1 Create Test Case

**File**: `Tests/TestGBufferDS.cpp`

```cpp
class CTestGBufferDS : public ITestCase {
public:
    const char* GetName() const override { return "TestGBufferDS"; }

    void OnFrame(int frameIndex) override {
        if (frameIndex == 20) {
            TakeScreenshot("gbuffer_ds");
        }
        if (frameIndex == 30) {
            RequestExit();
        }
    }
};
REGISTER_TEST(CTestGBufferDS)
```

#### 6.2 Run Tests

```bash
cmake --build build --target forfun
timeout 30 build/Debug/forfun.exe --test TestGBufferDS
```

---

## File Changes Summary

| File | Action | Description |
|------|--------|-------------|
| `RHI/PerDrawSlots.h` | Modify | Change to VolatileCBV, add CB_PerDraw |
| `Engine/Rendering/PassLayouts.h` | Modify | Add CreateGBufferPassLayout() |
| `Engine/Rendering/PerDrawLayout.h` | Modify | Update to VolatileCBV |
| `Engine/Material/MaterialConstants.h` | Create | CB_Material struct |
| `Core/MaterialManager.h` | Modify | Add descriptor set methods |
| `Core/MaterialManager.cpp` | Modify | Implement descriptor set methods |
| `Shader/GBuffer_DS.vs.hlsl` | Create | SM 5.1 vertex shader |
| `Shader/GBuffer_DS.ps.hlsl` | Create | SM 5.1 pixel shader |
| `Engine/Rendering/Deferred/GBufferPass.h` | Modify | Add DS members, update signature |
| `Engine/Rendering/Deferred/GBufferPass.cpp` | Modify | Add DS initialization and render path |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Modify | Pass perFrameSet to GBufferPass |
| `Tests/TestGBufferDS.cpp` | Create | Test case |

---

## Implementation Order

1. `RHI/PerDrawSlots.h` - Update struct
2. `Engine/Material/MaterialConstants.h` - Create CB_Material
3. `Core/MaterialManager.h/.cpp` - Add descriptor set support
4. `Shader/GBuffer_DS.vs.hlsl` - Create vertex shader
5. `Shader/GBuffer_DS.ps.hlsl` - Create pixel shader
6. `Engine/Rendering/Deferred/GBufferPass.h/.cpp` - Add DS path
7. `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` - Integration
8. `Tests/TestGBufferDS.cpp` - Test case

---

## Success Criteria

- [ ] GBufferPass uses BindDescriptorSet() for all bindings
- [ ] CMaterialAsset owns and manages its descriptor set
- [ ] CB_PerDraw contains World, WorldPrev, lightmapIndex
- [ ] SM 5.1 shaders compile and render correctly
- [ ] TestGBufferDS passes
- [ ] No visual regression in editor

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Material descriptor set not initialized | Assert in BindToCommandList() |
| Lightmap binding mismatch | Verify slot numbers match PerPassSlots.h |
| Performance regression | Profile before/after, material sorting should help |
| Shader compilation fails | Keep legacy shaders until validated |

---

**Last Updated**: 2026-01-28
