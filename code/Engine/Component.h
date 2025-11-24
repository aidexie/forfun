#pragma once
#include <typeindex>
class CGameObject;
class CPropertyVisitor;

class CComponent {
public:
    virtual ~CComponent() = default;
    void SetOwner(CGameObject* o){ owner = o; }
    CGameObject* GetOwner() const { return owner; }
    virtual const char* GetTypeName() const = 0;

    // Reflection: override this to expose properties to the editor
    virtual void VisitProperties(CPropertyVisitor& visitor) {}

private:
    CGameObject* owner = nullptr;
};

