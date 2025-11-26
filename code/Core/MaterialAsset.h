#pragma once

#include <string>
#include <DirectXMath.h>
#include "Engine/PropertyVisitor.h"

/**
 * Alpha Mode - Defines how alpha channel is handled in rendering
 */
enum class EAlphaMode : uint8_t {
    Opaque = 0,  // No transparency (default)
    Mask = 1,    // Binary transparency with cutoff threshold (Alpha Test)
    Blend = 2    // Continuous transparency (Alpha Blending)
};

/**
 * Material Asset - Shared resource representing a PBR material
 *
 * This is NOT a component. Materials are shared resources that can be
 * referenced by multiple MeshRenderers. They define the visual properties
 * and textures used for rendering.
 *
 * Material files (.ffasset) are JSON serialized and stored in the assets directory.
 */
class CMaterialAsset {
public:
    CMaterialAsset() = default;
    explicit CMaterialAsset(const std::string& name);

    // Asset metadata
    std::string name;

    // PBR properties
    DirectX::XMFLOAT3 albedo{1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;  // Constant AO (used when aoMap is empty)

    // Emissive properties
    DirectX::XMFLOAT3 emissive{0.0f, 0.0f, 0.0f};
    float emissiveStrength = 1.0f;
    // Transparency properties
    EAlphaMode alphaMode = EAlphaMode::Opaque;  // Alpha rendering mode
    float alphaCutoff = 0.5f;  // Cutoff threshold for Mask mode (0.0-1.0)


    // Texture paths (relative to assets directory)
    std::string albedoTexture;           // sRGB color texture
    std::string normalMap;               // Tangent-space normal map (Linear)
    std::string metallicRoughnessMap;    // Packed: G=Roughness, B=Metallic (Linear)
    std::string aoMap;                   // Ambient Occlusion (Linear)
    std::string emissiveMap;             // Emissive texture (sRGB)

    // Reflection system - expose properties for UI and serialization
    // CRITICAL: Use exact member variable names as the first parameter for JSON serialization compatibility!
    void VisitProperties(CPropertyVisitor& visitor) {
        visitor.VisitString("name", name);

        // PBR Properties - using exact variable names for JSON keys
        visitor.VisitFloat3("albedo", albedo);
        visitor.VisitFloatSlider("metallic", metallic, 0.0f, 1.0f);
        visitor.VisitFloatSlider("roughness", roughness, 0.0f, 1.0f);
        visitor.VisitFloatSlider("ao", ao, 0.0f, 1.0f);

        // Emissive Properties
        visitor.VisitFloat3("emissive", emissive);
        visitor.VisitFloat("emissiveStrength", emissiveStrength);
        // Transparency Properties
        // CRITICAL: VisitEnum expects int&, but alphaMode is uint8_t enum
        // Using temporary int to avoid undefined behavior from reinterpret_cast size mismatch
        int alphaModeInt = static_cast<int>(alphaMode);
        visitor.VisitEnum("alphaMode", alphaModeInt, {"Opaque", "Mask", "Blend"});
        alphaMode = static_cast<EAlphaMode>(alphaModeInt);
        visitor.VisitFloatSlider("alphaCutoff", alphaCutoff, 0.0f, 1.0f);


        // Texture Paths - using exact variable names for JSON keys
        visitor.VisitFilePath("albedoTexture", albedoTexture, "Image Files\0*.jpg;*.png;*.tga;*.bmp\0All Files\0*.*\0");
        visitor.VisitFilePath("normalMap", normalMap, "Image Files\0*.jpg;*.png;*.tga;*.bmp\0All Files\0*.*\0");
        visitor.VisitFilePath("metallicRoughnessMap", metallicRoughnessMap, "Image Files\0*.jpg;*.png;*.tga;*.bmp\0All Files\0*.*\0");
        visitor.VisitFilePath("aoMap", aoMap, "Image Files\0*.jpg;*.png;*.tga;*.bmp\0All Files\0*.*\0");
        visitor.VisitFilePath("emissiveMap", emissiveMap, "Image Files\0*.jpg;*.png;*.tga;*.bmp\0All Files\0*.*\0");
    }

    // Serialization
    bool LoadFromFile(const std::string& filepath);
    bool SaveToFile(const std::string& filepath) const;

    // JSON serialization (now uses reflection)
    std::string ToJson() const;
    bool FromJson(const std::string& json);
};
