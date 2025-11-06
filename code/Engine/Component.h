#pragma once
#include <typeindex>
class GameObject;
class PropertyVisitor;

class Component {
public:
    virtual ~Component() = default;
    void SetOwner(GameObject* o){ owner = o; }
    GameObject* GetOwner() const { return owner; }
    virtual const char* GetTypeName() const = 0;

    // Reflection: override this to expose properties to the editor
    virtual void VisitProperties(PropertyVisitor& visitor) {}

private:
    GameObject* owner = nullptr;
};
