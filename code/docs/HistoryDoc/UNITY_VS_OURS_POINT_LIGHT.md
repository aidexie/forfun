# Unity vs Our Point Light Implementation

## 问题现象

**对比图分析**：
- **Unity** (`E:\forfun\test\unity_point_light.png`):
  - ✨ 明亮的高光（接近白色）
  - ✨ 明显的光晕/辉光效果
  - ✨ 地面反射有渐变拉伸

- **Ours** (`E:\forfun\test\my_point_light.png`):
  - ❌ 高光相对暗淡
  - ❌ 没有光晕效果
  - ❌ 整体显得"平淡"

**测试条件**：
- Material: `metallic = 1.0`, `roughness = 0.25`
- 绿色点光源

---

## 根本原因

### **Bug #1：Specular 计算错误**（已修复）

**错误代码**（Common.hlsl:115）：
```hlsl
float3 specular = (D ) / max(4.0 * NdotV * NdotL, 0.001);  // 缺少 G * F
```

**正确代码**：
```hlsl
float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
```

**影响**：高光强度只有理论值的一小部分（缺少 Geometry 和 Fresnel 贡献）。

---

### **差异 #2：能量守恒策略不同**

#### **我们的原始实现**（LDR）
```hlsl
specular = min(specular, float3(1.0, 1.0, 1.0));  // 限制到 [0, 1]
```

**问题**：
- 高光被强制限制在 1.0
- 即使 `light.intensity = 100`，高光也不会更亮
- 无法表现"过曝"效果

#### **Unity 的实现**（HDR）

**Unity URP 源码**（`Lighting.hlsl:DirectBRDFSpecular`）：
```hlsl
half specularTerm = roughness2 / ((d * d) * max(0.1h, LoH2) * normalizationTerm);

#if defined (SHADER_API_MOBILE) || defined (SHADER_API_SWITCH)
    specularTerm = clamp(specularTerm, 0.0, 100.0);  // 移动平台限制到 100
#endif
// 桌面平台：不限制！
```

**关键点**：
1. 桌面：specular 可以远超 1.0（如 10, 50, 甚至 1000）
2. 移动：限制到 100.0（而不是 1.0）
3. 最终通过 **Tone Mapping** 压缩到 [0, 1]

---

### **差异 #3：Bloom 后处理**

Unity 图中的"光晕"效果来自 **Bloom（辉光）后处理**：

```
┌─────────────────────────────────────┐
│ 1. Extract Bright Pixels            │
│    if (luminance(color) > threshold) │
│        brightColor = color           │
├─────────────────────────────────────┤
│ 2. Gaussian Blur (Multi-pass)       │
│    downscale → blur → upscale        │
├─────────────────────────────────────┤
│ 3. Additive Blend                   │
│    finalColor = originalColor +      │
│                 bloomColor * intensity│
└─────────────────────────────────────┘
```

**Unity 的 Bloom 设置**：
- **Threshold**: 0.9-1.0（提取高光）
- **Intensity**: 0.5-1.0（混合强度）
- **Blur Passes**: 4-6（模糊次数）

**我们的实现**：
- ❌ 没有 Bloom 后处理
- 结果：高光边缘锐利，没有辉光扩散

---

## Unity URP 完整工作流

```hlsl
// ============================================
// 1. Lighting Pass (Forward Rendering)
// ============================================
// MainLightRealtimeShadow.hlsl + Lighting.hlsl
half3 DirectBDRF(BRDFData brdfData, half3 normalWS,
                 half3 lightDirectionWS, half3 viewDirectionWS)
{
    // GGX + Geometry + Fresnel
    half3 radiance = lightColor * lightAttenuation;
    half3 brdf = (specular + diffuse);  // specular 可以 >> 1.0
    return brdf * radiance * NdotL;
}

// ============================================
// 2. HDR Framebuffer
// ============================================
// 渲染到 R16G16B16A16_FLOAT (HDR格式)
// Color 可以存储 [0, 65504] 范围

// ============================================
// 3. Post-Processing Stack
// ============================================
// a. Bloom
BloomPyramidPS() {
    // Extract bright pixels (threshold = 1.0)
    float brightness = max(color.r, max(color.g, color.b));
    if (brightness > _Threshold) {
        return color * (brightness - _Threshold) / brightness;
    }
}

// b. Tone Mapping
ToneMappingPS() {
    // ACES, Neutral, or Reinhard
    color = ACESFitted(color);  // 压缩到 [0, 1]
}

// c. Color Grading (optional)

// d. FXAA / TAA (optional)

// ============================================
// 4. Final Output to LDR
// ============================================
// Gamma correction + dithering
```

---

## 数学对比

### **Cook-Torrance BRDF（我们都用这个）**

$$
f_r = \frac{D(h) \cdot G(l, v, h) \cdot F(v, h)}{4 (n \cdot l)(n \cdot v)}
$$

- **D**: GGX Normal Distribution
- **G**: Smith's Geometry (Schlick-GGX)
- **F**: Fresnel (Schlick approximation)

**我们的实现**：✅ 数学上完全正确

**Unity 的实现**：✅ 也是正确的（有优化）

**差异不在 BRDF 本身！**

---

### **能量守恒对比**

| 项目 | 我们（原始） | Unity URP | 物理正确性 |
|------|------------|-----------|-----------|
| Specular 上限 | 1.0 | 100.0 (移动) / ∞ (桌面) | Unity 更合理 |
| Tone Mapping | ❌ 无 | ✅ ACES/Neutral | 必需（HDR→LDR）|
| Bloom | ❌ 无 | ✅ 有 | 视觉效果，非物理 |
| HDR Buffer | ❌ R8G8B8A8 | ✅ R16G16B16A16_FLOAT | Unity 正确 |

---

## 优化方案

### **Phase 1：修复 Bug + 移除过度限制**（已实现）

```hlsl
// 修复 specular 计算
float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

// 移除 clamp(specular, 0, 1) - 改为 Unity 风格
#if defined(LDR_MODE)
    specular = min(specular, 100.0);  // 移动平台
#endif
// 桌面：不限制，依赖 tone mapping
```

**效果**：
- ✅ 高光亮度恢复正常
- ✅ 允许"过曝"效果
- ⚠️ 仍然没有光晕（需要 Phase 2）

---

### **Phase 2：实现 HDR + Tone Mapping**

#### **2.1 切换到 HDR Framebuffer**

```cpp
// MainPass.cpp: CreateRenderTargets()
D3D11_TEXTURE2D_DESC rtDesc = {};
rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR
// 原来：DXGI_FORMAT_R8G8B8A8_UNORM (LDR)
```

#### **2.2 添加 Tone Mapping Pass**

```hlsl
// ToneMapping.ps.hlsl
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 main(PSIn i) : SV_Target {
    float3 hdrColor = gHDRTexture.Sample(gSampler, i.uv).rgb;
    float3 ldrColor = ACESFilm(hdrColor);  // HDR → LDR
    return float4(ldrColor, 1.0);
}
```

---

### **Phase 3：实现 Bloom 后处理**

#### **算法流程**

```cpp
class CBloomPass {
public:
    void Execute(ID3D11DeviceContext* ctx,
                 ID3D11ShaderResourceView* input,
                 ID3D11RenderTargetView* output) {
        // 1. Extract bright pixels (threshold)
        ExtractBrightPass(ctx, input, m_brightRT);

        // 2. Downscale + Blur (Gaussian pyramid)
        for (int i = 0; i < m_blurPasses; i++) {
            Downsample(ctx, m_brightRT, m_blurRTs[i]);
            GaussianBlur(ctx, m_blurRTs[i]);
        }

        // 3. Upscale + Combine
        for (int i = m_blurPasses - 1; i >= 0; i--) {
            Upsample(ctx, m_blurRTs[i], m_blurRTs[i-1]);
        }

        // 4. Additive blend
        AdditiveBlend(ctx, input, m_blurRTs[0], output);
    }
};
```

#### **Shader 示例**

```hlsl
// BloomExtract.ps.hlsl
float4 main(PSIn i) : SV_Target {
    float3 color = gSceneTexture.Sample(gSampler, i.uv).rgb;
    float brightness = max(color.r, max(color.g, color.b));

    float contribution = max(0, brightness - gThreshold);
    contribution /= max(brightness, 0.00001);

    return float4(color * contribution, 1.0);
}

// BloomBlur.ps.hlsl (Gaussian 13-tap)
float4 main(PSIn i) : SV_Target {
    float3 color = float3(0, 0, 0);
    float2 texelSize = 1.0 / gTextureSize;

    // 13-tap Gaussian kernel
    float weights[7] = { 0.0044, 0.0540, 0.2420, 0.3992, 0.2420, 0.0540, 0.0044 };

    for (int x = -3; x <= 3; x++) {
        float2 uv = i.uv + float2(x, 0) * texelSize * gDirection;
        color += gBlurTexture.Sample(gSampler, uv).rgb * weights[x + 3];
    }

    return float4(color, 1.0);
}

// BloomCombine.ps.hlsl
float4 main(PSIn i) : SV_Target {
    float3 scene = gSceneTexture.Sample(gSampler, i.uv).rgb;
    float3 bloom = gBloomTexture.Sample(gSampler, i.uv).rgb;

    return float4(scene + bloom * gBloomIntensity, 1.0);
}
```

---

## 实现优先级

### ✅ **已完成**
1. 修复 specular 计算 bug（`D * G * F`）
2. 移除过度的 specular clamp（改为 Unity 风格）

### 🔄 **短期目标**（接近 Unity）
3. 切换到 HDR framebuffer (R16G16B16A16_FLOAT)
4. 实现 ACES Tone Mapping

### 🎯 **中期目标**（完全匹配 Unity）
5. 实现 Bloom 后处理
6. 添加 Bloom 参数调节（Threshold, Intensity, Blur Passes）

### 🚀 **长期目标**（超越 Unity）
7. 实现完整 Post-Processing Stack
   - Color Grading (LUT)
   - Chromatic Aberration
   - Vignette
   - Film Grain
8. TAA (Temporal Anti-Aliasing)
9. Auto Exposure

---

## 性能考虑

### **HDR + Tone Mapping**
- **额外开销**: ~0.5ms (1080p)
- **内存**: 2x framebuffer size (R16 vs R8)

### **Bloom**
- **额外开销**: ~2-5ms (取决于 blur passes)
- **内存**: Mipmap chain (~1.33x original size)

### **优化**
- Compute Shader 实现 Bloom（更快）
- 使用 Half-precision (R16G16B16A16_FLOAT → R11G11B10_FLOAT)
- Separable Gaussian blur（2 pass 替代 full kernel）

---

## Unity URP 源码参考

```
Packages/com.unity.render-pipelines.universal/
├── ShaderLibrary/
│   ├── Lighting.hlsl              (BRDF functions)
│   ├── BRDF.hlsl                  (GGX, Geometry, Fresnel)
│   └── RealtimeLights.hlsl        (Point light attenuation)
├── Shaders/
│   ├── PostProcessing/
│   │   ├── Bloom.shader           (Bloom implementation)
│   │   └── Tonemapping.hlsl       (ACES, Neutral, Reinhard)
└── Runtime/
    ├── Passes/
    │   └── PostProcessPass.cs     (Post-processing orchestration)
```

---

## 总结

| 特性 | 我们（修复前） | 我们（修复后） | Unity URP |
|------|---------------|--------------|-----------|
| Cook-Torrance BRDF | ❌ Bug | ✅ 正确 | ✅ 正确 |
| Specular 限制 | 1.0 | 100.0 / ∞ | 100.0 / ∞ |
| HDR Rendering | ❌ | ⏳ TODO | ✅ |
| Tone Mapping | ❌ | ⏳ TODO | ✅ ACES |
| Bloom | ❌ | ⏳ TODO | ✅ |
| 视觉效果 | 平淡 | 更亮 | 漂亮 |

**结论**：
1. ✅ BRDF 数学正确（修复 bug 后）
2. 🔄 需要 HDR + Tone Mapping（接近 Unity）
3. 🎨 需要 Bloom（完全匹配 Unity）

---

**Last Updated**: 2025-11-27
