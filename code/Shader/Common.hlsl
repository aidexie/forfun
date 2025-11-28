// Common.hlsl
// Shared shader functions and constants

#ifndef COMMON_HLSL
#define COMMON_HLSL

// ============================================
// Constants
// ============================================
static const float PI = 3.14159265359;

// ============================================
// PBR BRDF Functions
// ============================================

// Normal Distribution Function (GGX/Trowbridge-Reitz)
float DistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0000001);  // Prevent divide by zero
}

// Geometry Function (Schlick-GGX for direct lighting)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;  // Direct lighting k
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method for Geometry obstruction
float GeometrySmith(float NdotV, float NdotL, float roughness) {
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Fresnel-Schlick approximation
float3 FresnelSchlick(float VdotH, float3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
}

// Fresnel-Schlick with roughness (for IBL)
float3 FresnelSchlickRoughness(float NdotV, float3 F0, float roughness) {
    float oneMinusRoughness = 1.0 - roughness;
    return F0 + (max(float3(oneMinusRoughness, oneMinusRoughness, oneMinusRoughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
}

// ============================================
// Distance Attenuation (UE4 Style)
// ============================================
// Reference: UE4 DeferredLightingCommon.ush
// Combines physical inverse-square falloff with smooth windowing
float GetDistanceAttenuation(float3 unnormalizedLightVector, float invRadius) {
    float distSqr = dot(unnormalizedLightVector, unnormalizedLightVector);
    float attenuation = 1.0 / (distSqr + 1.0);  // Inverse-square + 1 prevents infinity
    float factor = distSqr * invRadius * invRadius;
    float smoothFactor = saturate(1.0 - factor * factor);
    smoothFactor = smoothFactor * smoothFactor;
    return attenuation * smoothFactor;
}

// ============================================
// Point Light Calculation (Shared)
// ============================================
// Used by both forward and clustered rendering
struct PointLightInput {
    float3 position;
    float range;
    float3 color;
    float intensity;
};

float3 CalculatePointLightPBR(
    PointLightInput light,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Light vector (unnormalized for distance attenuation)
    float3 unnormalizedL = light.position - worldPos;
    float distance = length(unnormalizedL);
    float3 L = unnormalizedL / distance;

    // Early exit: backface (30% performance gain)
    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return float3(0, 0, 0);

    // Distance attenuation (UE4 style)
    float invRadius = 1.0 / light.range;
    float attenuation = GetDistanceAttenuation(unnormalizedL, invRadius);

    // Early exit: out of range
    if (attenuation < 0.001) return float3(0, 0, 0);

    // BRDF calculations
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));

    // Cook-Torrance BRDF terms
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);

    // Fresnel
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = FresnelSchlick(VdotH, F0);

    // Specular BRDF
    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Energy conservation: Clamp to reasonable max (Unity-style)
    // Note: Unity uses HDR + tone mapping, so allows specular > 1.0
    // For LDR, we clamp to 100.0 (mobile) or don't clamp (desktop HDR)
    #if defined(LDR_MODE)
        specular = min(specular, float3(100.0, 100.0, 100.0));  // Unity mobile
    #endif
    // For HDR, no clamp - rely on tone mapping in post-processing

    // Diffuse BRDF (Lambertian)
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // Final radiance
    float3 radiance = light.color * light.intensity * attenuation;
    return (diffuse + specular) * radiance * NdotL;
}

// ============================================
// Spot Light BRDF (Point Light + Cone Attenuation)
// ============================================

struct SpotLightInput {
    float3 position;
    float range;
    float3 color;
    float intensity;
    float3 direction;     // World space direction (normalized)
    float innerConeAngle; // cos(innerAngle)
    float outerConeAngle; // cos(outerAngle)
};

float3 CalculateSpotLightPBR(
    SpotLightInput light,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Light vector (unnormalized for distance attenuation)
    float3 unnormalizedL = light.position - worldPos;
    float distance = length(unnormalizedL);
    float3 L = unnormalizedL / distance;

    // Early exit: backface (30% performance gain)
    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return float3(0, 0, 0);

    // Distance attenuation (same as point light)
    float invRadius = 1.0 / light.range;
    float attenuation = GetDistanceAttenuation(unnormalizedL, invRadius);

    // Cone attenuation (spot light specific)
    float3 lightDir = -L;  // Direction from light to surface
    float cosTheta = dot(lightDir, light.direction);
    float spotFactor = smoothstep(light.outerConeAngle, light.innerConeAngle, cosTheta);

    // Combined attenuation
    attenuation *= spotFactor;

    // Early exit: out of range or out of cone
    if (attenuation < 0.001) return float3(0, 0, 0);

    // BRDF calculations (same as point light)
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));

    // Cook-Torrance BRDF terms
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);

    // Fresnel
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = FresnelSchlick(VdotH, F0);

    // Specular BRDF
    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Energy conservation (same as point light)
    #if defined(LDR_MODE)
        specular = min(specular, float3(100.0, 100.0, 100.0));
    #endif

    // Diffuse BRDF (Lambertian)
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // Final radiance
    float3 radiance = light.color * light.intensity * attenuation;
    return (diffuse + specular) * radiance * NdotL;
}

#endif // COMMON_HLSL
