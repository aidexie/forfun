#pragma once
#include <DirectXMath.h>
#include <string>
#include <vector>

// Property visitor interface for component reflection system
// Components expose their properties by implementing VisitProperties()
class PropertyVisitor {
public:
    virtual ~PropertyVisitor() = default;

    // Basic types
    virtual void VisitFloat(const char* name, float& value) = 0;
    virtual void VisitInt(const char* name, int& value) = 0;
    virtual void VisitBool(const char* name, bool& value) = 0;
    virtual void VisitString(const char* name, std::string& value) = 0;

    // Math types
    virtual void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) = 0;

    // Enum type (with options)
    virtual void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) = 0;

    // Read-only label (for display only, like component info)
    virtual void VisitLabel(const char* name, const char* value) {}
};
