#pragma once
#include <DirectXMath.h>
#include <string>
#include <vector>

// Property visitor interface for component reflection system
// Components expose their properties by implementing VisitProperties()
class CPropertyVisitor {
public:
    virtual ~CPropertyVisitor() = default;

    // Basic types
    virtual void VisitFloat(const char* name, float& value) = 0;
    virtual void VisitInt(const char* name, int& value) = 0;
    virtual void VisitBool(const char* name, bool& value) = 0;
    virtual void VisitString(const char* name, std::string& value) = 0;

    // Float slider with range [min, max]
    virtual void VisitFloatSlider(const char* name, float& value, float min, float max) {
        // Fallback to regular float if not implemented
        value = (value < min) ? min : (value > max) ? max : value; // Clamp
        VisitFloat(name, value);
    }

    // Math types
    virtual void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) = 0;

    // Float3 array (for SH coefficients, etc.)
    // Default implementation: visit each element as "name[i]"
    virtual void VisitFloat3Array(const char* name, DirectX::XMFLOAT3* values, int count) {
        for (int i = 0; i < count; i++) {
            std::string elemName = std::string(name) + "[" + std::to_string(i) + "]";
            VisitFloat3(elemName.c_str(), values[i]);
        }
    }

    // Float3 read-only (for display only, cannot be edited)
    virtual void VisitFloat3ReadOnly(const char* name, const DirectX::XMFLOAT3& value) {
        // Fallback: do nothing (read-only fields are optional for serialization)
    }

    // Float3 as angles (UI shows degrees, storage/serialization uses radians)
    // Default: treat as regular Float3 (radians) - for JSON serialization
    // ImGui visitor overrides to convert rad<->deg
    virtual void VisitFloat3AsAngles(const char* name, DirectX::XMFLOAT3& valueRadians) {
        VisitFloat3(name, valueRadians);  // Default: no conversion
    }

    // Enum type (with options)
    virtual void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) = 0;

    // File path with browse button (filter: "*.obj;*.gltf" or nullptr for all files)
    virtual void VisitFilePath(const char* name, std::string& value, const char* filter = nullptr) {
        // Fallback to string if not implemented
        VisitString(name, value);
    }

    // Read-only label (for display only, like component info)
    virtual void VisitLabel(const char* name, const char* value) {}
};

