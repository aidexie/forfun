#pragma once
#include <string>
#include <memory>
#include <vector>
#include <type_traits>
#include "Component.h"

class CGameObject {
public:
    explicit CGameObject(std::string name): m_name(std::move(name)) {}
    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& n){ m_name = n; }

    template<class T, class...Args>
    T* AddComponent(Args&&...args){
        static_assert(std::is_base_of<CComponent,T>::value, "T must be CComponent");
        auto up = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = up.get();
        raw->SetOwner(this);
        m_components.emplace_back(std::move(up));
        return raw;
    }

    template<class T>
    T* GetComponent(){
        for (auto& c : m_components){
            if (auto p = dynamic_cast<T*>(c.get())) return p;
        }
        return nullptr;
    }

    template<class T>
    const T* GetComponent() const {
        for (const auto& c : m_components){
            if (auto p = dynamic_cast<const T*>(c.get())) return p;
        }
        return nullptr;
    }

    // Iterate over all components (for serialization/reflection)
    template<typename Func>
    void ForEachComponent(Func&& func) const {
        for (const auto& comp : m_components) {
            func(comp.get());
        }
    }

private:
    std::string m_name;
    std::vector<std::unique_ptr<CComponent>> m_components;
};


