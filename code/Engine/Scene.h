#pragma once
#include <cstddef>
#include <string>
#include "World.h"

struct Scene {
    World world;
    int selected = -1; // index into world list
    GameObject* GetSelected(){
        if (selected>=0 && (std::size_t)selected < world.Count()) return world.Get((std::size_t)selected);
        return nullptr;
    }
};
