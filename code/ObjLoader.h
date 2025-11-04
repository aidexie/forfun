
#pragma once
#include <string>
#include "Mesh.h"

bool LoadOBJ_PNT(const std::string& path, MeshCPU_PNT& out, bool flipZ=true, bool flipWinding=true);
void RecenterAndScale(MeshCPU_PNT& m, float targetDiag = 2.0f);
