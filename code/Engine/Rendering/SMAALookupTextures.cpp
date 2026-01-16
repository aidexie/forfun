#include "SMAALookupTextures.h"

// Official SMAA lookup textures from the reference implementation
// https://github.com/iryoku/smaa
// Copyright (C) 2013 Jorge Jimenez et al. (MIT License)
#include "SMAAAreaTex.h"
#include "SMAASearchTex.h"

namespace SMAALookupTextures {

const uint8_t* GetAreaTexData() {
    return areaTexBytes;
}

const uint8_t* GetSearchTexData() {
    return searchTexBytes;
}

} // namespace SMAALookupTextures
