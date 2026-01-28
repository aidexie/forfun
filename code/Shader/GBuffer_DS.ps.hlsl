// GBuffer_DS.ps.hlsl
// G-Buffer Pixel Shader (Descriptor Set Path, SM 5.1)
// Uses register spaces for 4-set descriptor model

//==============================================
// Set 0: PerFrame (space0) - Global resources
//==============================================
// (No PS resources needed from Set 0 for GBuffer)

//==============================================
// Set 1: PerPass (space1) - GBuffer pass data
//==============================================
// 2D Lightmap resources
Texture2D<float4> gLightmap2D : register(t12, space1);
StructuredBuffer<float4> gLightmapScaleOffsets : register(t13, space1);
SamplerState gLightmapSampler : register(s2, space1);

//==============================================
// Set 2: PerMaterial (space2) - Material data
//==============================================
cbuffer CB_Material : register(b0, space2) {
    float3 gMatAlbedo;
    float gMatMetallic;
    float3 gMatEmissive;
    float gMatRoughness;
    float gMatEmissiveStrength;
    int gHasMetallicRoughnessTexture;
    int gHasEmissiveMap;
    int gAlphaMode;  // 0=Opaque, 1=Mask, 2=Blend
    float gAlphaCutoff;
    float gMaterialID;
    float2 _padMat;
};

Texture2D gAlbedoMap : register(t0, space2);
Texture2D gNormalMap : register(t1, space2);
Texture2D gMetallicRoughnessMap : register(t2, space2);  // G=Roughness, B=Metallic (glTF 2.0)
Texture2D gEmissiveMap : register(t3, space2);
SamplerState gMaterialSampler : register(s0, space2);

// Material ID constants (encoded in RT3.a)
#define MATERIAL_STANDARD       0    // Default PBR
#define MATERIAL_SUBSURFACE     1    // Skin, wax, leaves
#define MATERIAL_CLOTH          2    // Fabric, velvet
#define MATERIAL_HAIR           3    // Anisotropic hair
#define MATERIAL_CLEAR_COAT     4    // Car paint, lacquer
#define MATERIAL_UNLIT          5    // Emissive only

struct PSIn {
    float4 posH : SV_Position;
    float3 posWS : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3x3 TBN : TEXCOORD2;
    float4 color : COLOR0;
    float2 uv2 : TEXCOORD5;
    float4 posCurr : TEXCOORD6;
    float4 posPrev : TEXCOORD7;
    nointerpolation int lightmapIndex : TEXCOORD8;
};

// G-Buffer output structure (5 render targets)
struct PSOut {
    float4 worldPosMetallic     : SV_Target0;  // RT0: WorldPosition.xyz + Metallic
    float4 normalRoughness      : SV_Target1;  // RT1: Normal.xyz + Roughness
    float4 albedoAO             : SV_Target2;  // RT2: Albedo.rgb + AO
    float4 emissiveMaterialID   : SV_Target3;  // RT3: Emissive.rgb + MaterialID
    float2 velocity             : SV_Target4;  // RT4: Velocity.xy
};

// Sample 2D Lightmap (inline version for DS path)
float3 SampleLightmap2D_DS(float2 uv2, int lightmapIndex)
{
    if (lightmapIndex < 0)
    {
        return float3(0, 0, 0);
    }

    // Fetch per-object scale/offset from structured buffer
    float4 scaleOffset = gLightmapScaleOffsets[lightmapIndex];

    // Transform UV2 with per-object scale/offset
    float2 atlasUV = uv2 * scaleOffset.xy + scaleOffset.zw;

    // Sample lightmap (HDR, linear space)
    float3 irradiance = gLightmap2D.Sample(gLightmapSampler, atlasUV).rgb;

    return irradiance;
}

PSOut main(PSIn i) {
    PSOut o;

    // ============================================
    // Sample Albedo (sRGB texture, auto-converted to linear)
    // ============================================
    float4 albedoSample = gAlbedoMap.Sample(gMaterialSampler, i.uv);
    float3 albedo = albedoSample.rgb * gMatAlbedo;
    float alpha = albedoSample.a;

    // ============================================
    // Alpha Test (Mask mode)
    // ============================================
    if (gAlphaMode == 1 && alpha < gAlphaCutoff) {
        discard;
    }

    // ============================================
    // Normal Mapping
    // ============================================
    float3 nTS = gNormalMap.Sample(gMaterialSampler, i.uv).xyz * 2.0 - 1.0;
    nTS.y = -nTS.y;  // Flip Y (glTF uses OpenGL format)
    nTS = normalize(nTS);
    float3 N = normalize(mul(nTS, i.TBN));

    // ============================================
    // Metallic / Roughness
    // ============================================
    float2 mrSample = gMetallicRoughnessMap.Sample(gMaterialSampler, i.uv).gb;
    float metallic = gHasMetallicRoughnessTexture ? mrSample.y : gMatMetallic;
    float roughness = gHasMetallicRoughnessTexture ? mrSample.x : gMatRoughness;

    // ============================================
    // Ambient Occlusion (from vertex color)
    // ============================================
    float ao = i.color.r;

    // ============================================
    // Emissive + 2D Lightmap (pre-baked to emissive channel)
    // ============================================
    float3 emissive = gMatEmissive * gMatEmissiveStrength;
    if (gHasEmissiveMap) {
        emissive = gEmissiveMap.Sample(gMaterialSampler, i.uv).rgb * gMatEmissiveStrength;
    }

    // Apply 2D Lightmap (if available) - adds GI to emissive
    if (i.lightmapIndex >= 0) {
        float3 lightmapGI = SampleLightmap2D_DS(i.uv2, i.lightmapIndex);
        emissive += lightmapGI * albedo;  // Modulate by albedo for diffuse GI
    }

    // ============================================
    // Velocity Calculation (for TAA / Motion Blur)
    // ============================================
    // Convert to NDC [-1, 1]
    float2 ndcCurr = i.posCurr.xy / i.posCurr.w;
    float2 ndcPrev = i.posPrev.xy / i.posPrev.w;

    // Convert to UV space [0, 1] and calculate velocity
    float2 uvCurr = ndcCurr * 0.5 + 0.5;
    float2 uvPrev = ndcPrev * 0.5 + 0.5;
    uvCurr.y = 1.0 - uvCurr.y;  // Flip Y for DirectX
    uvPrev.y = 1.0 - uvPrev.y;

    float2 velocity = uvCurr - uvPrev;

    // ============================================
    // Output to G-Buffer
    // ============================================
    o.worldPosMetallic = float4(i.posWS, metallic);
    o.normalRoughness = float4(N, roughness);
    o.albedoAO = float4(albedo, ao);
    o.emissiveMaterialID = float4(emissive, gMaterialID / 255.0);
    o.velocity = velocity;

    return o;
}
