#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include "GameObject.h"  // Required for REGISTER_COMPONENT macro

// Forward declarations
class Component;

// ComponentRegistry: Global registry for automatic component factory registration
// Usage: Add REGISTER_COMPONENT(YourComponent) at the end of your component header file
class ComponentRegistry {
public:
    using FactoryFunc = std::function<Component*(GameObject*)>;

    static ComponentRegistry& Instance() {
        static ComponentRegistry instance;
        return instance;
    }

    // Register a component type with its factory function
    void Register(const std::string& typeName, FactoryFunc factory) {
        m_factories[typeName] = factory;
    }

    // Create a component instance by type name
    Component* Create(GameObject* go, const std::string& typeName) {
        auto it = m_factories.find(typeName);
        if (it != m_factories.end()) {
            return it->second(go);
        }
        return nullptr;
    }

private:
    ComponentRegistry() = default;
    std::unordered_map<std::string, FactoryFunc> m_factories;
};

// REGISTER_COMPONENT macro: Automatically registers a component type
// Place this at the end of your component header file:
//
// struct MyComponent : public Component { ... };
// REGISTER_COMPONENT(MyComponent)
//
#define REGISTER_COMPONENT(ComponentType) \
    namespace { \
        struct ComponentType##Registrar { \
            ComponentType##Registrar() { \
                ComponentRegistry::Instance().Register( \
                    #ComponentType, \
                    [](GameObject* go) -> Component* { \
                        return go->AddComponent<ComponentType>(); \
                    } \
                ); \
            } \
        }; \
        static ComponentType##Registrar g_##ComponentType##Registrar; \
    }
