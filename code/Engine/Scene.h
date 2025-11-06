#pragma once
#include <vector>
#include <string>

struct Entity { int id; std::string name; };

struct Scene {
    std::vector<Entity> entities;
    int selected = -1;
    Entity* GetSelected() {
        if (selected>=0 && selected<(int)entities.size()) return &entities[selected];
        return nullptr;
    }
};
