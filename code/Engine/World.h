#pragma once
#include <memory>
#include <vector>
#include <string>
#include "GameObject.h"

class World {
public:
    GameObject* Create(const std::string& name){
        m_objects.emplace_back(std::make_unique<GameObject>(name));
        return m_objects.back().get();
    }
    void Destroy(std::size_t index){
        if (index < m_objects.size()){
            m_objects.erase(m_objects.begin()+index);
        }
    }
    std::size_t Count() const { return m_objects.size(); }
    GameObject* Get(std::size_t i){ return (i<m_objects.size())? m_objects[i].get() : nullptr; }
    const std::vector<std::unique_ptr<GameObject>>& Objects() const { return m_objects; }
private:
    std::vector<std::unique_ptr<GameObject>> m_objects;
};
