#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include "GameObject.h"  // Required for REGISTER_COMPONENT macro

// Forward declarations
class CComponent;

// CComponentRegistry: Global registry for automatic CComponent factory registration
// Usage: Add REGISTER_COMPONENT(YourComponent) at the end of your CComponent header file
class CComponentRegistry {
public:
    using FactoryFunc = std::function<CComponent*(CGameObject*)>;

    static CComponentRegistry& Instance() {
        static CComponentRegistry instance;
        return instance;
    }

    // Register a CComponent type with its factory function
    void Register(const std::string& typeName, FactoryFunc factory) {
        m_factories[typeName] = factory;
    }

    // Create a CComponent instance by type name
    CComponent* Create(CGameObject* go, const std::string& typeName) {
        auto it = m_factories.find(typeName);
        if (it != m_factories.end()) {
            return it->second(go);
        }
        return nullptr;
    }

private:
    CComponentRegistry() = default;
    std::unordered_map<std::string, FactoryFunc> m_factories;
};

// REGISTER_COMPONENT macro: Automatically registers a CComponent type
// Place this at the end of your CComponent header file:
//
// struct SMyComponent : public CComponent {
//     const char* GetTypeName() const override { return "MyComponent"; }
// };
// REGISTER_COMPONENT(SMyComponent)
//
// The macro uses GetTypeName() to determine the registration key, so the
// serialization name can differ from the C++ type name (e.g., STransform vs "Transform")
//
#define REGISTER_COMPONENT(ComponentType) \
    namespace { \
        struct ComponentType##Registrar { \
            ComponentType##Registrar() { \
                ComponentType temp; \
                CComponentRegistry::Instance().Register( \
                    temp.GetTypeName(), \
                    [](CGameObject* go) -> CComponent* { \
                        return go->AddComponent<ComponentType>(); \
                    } \
                ); \
            } \
        }; \
        static ComponentType##Registrar g_##ComponentType##Registrar; \
    }


