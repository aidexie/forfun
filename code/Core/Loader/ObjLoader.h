
#pragma once
#include <string>
#include "Mesh.h"

bool LoadOBJ_PNT(const std::string& path, SMeshCPU_PNT& out, bool flipZ=true, bool flipWinding=true);
void RecenterAndScale(SMeshCPU_PNT& m, float targetDiag = 2.0f);
