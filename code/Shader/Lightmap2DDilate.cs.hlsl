// ============================================
// 2D Lightmap Dilation Compute Shader
// ============================================
// Expands valid texels into invalid (empty) regions to prevent
// dark seams when bilinear filtering samples near UV chart edges.
//
// Algorithm:
// - For each invalid texel (alpha == 0), search for nearest valid neighbor
// - Copy color from nearest valid texel within search radius
// - Valid texels are unchanged
//
// Run multiple passes for larger dilation radius.

// ============================================
// Constant Buffer
// ============================================

cbuffer CB_DilateParams : register(b0) {
    uint g_AtlasWidth;
    uint g_AtlasHeight;
    uint g_SearchRadius;    // Max distance to search for valid texel
    uint g_Padding;
};

// ============================================
// Resource Bindings
// ============================================

// Input: Current lightmap state
Texture2D<float4> g_Input : register(t0);

// Output: Dilated lightmap
RWTexture2D<float4> g_Output : register(u0);

// ============================================
// Main Compute Shader
// ============================================

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint x = dispatchThreadID.x;
    uint y = dispatchThreadID.y;

    // Bounds check
    if (x >= g_AtlasWidth || y >= g_AtlasHeight) {
        return;
    }

    // Load current texel
    float4 current = g_Input[uint2(x, y)];

    // If texel is valid (alpha > 0), keep it unchanged
    if (current.a > 0.0f) {
        g_Output[uint2(x, y)] = current;
        return;
    }

    // Invalid texel - search for nearest valid neighbor
    float3 bestColor = float3(0, 0, 0);
    float bestDistSq = 1e10f;
    bool found = false;

    // Search in a square region around the texel
    int radius = (int)g_SearchRadius;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            // Skip center (already checked)
            if (dx == 0 && dy == 0) continue;

            int nx = (int)x + dx;
            int ny = (int)y + dy;

            // Bounds check
            if (nx < 0 || nx >= (int)g_AtlasWidth || ny < 0 || ny >= (int)g_AtlasHeight) {
                continue;
            }

            float4 neighbor = g_Input[uint2(nx, ny)];

            // Check if neighbor is valid
            if (neighbor.a > 0.0f) {
                float distSq = (float)(dx * dx + dy * dy);
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestColor = neighbor.rgb;
                    found = true;
                }
            }
        }
    }

    // Write result
    if (found) {
        // Found a valid neighbor - use its color, mark as dilated (alpha = 0.5)
        g_Output[uint2(x, y)] = float4(bestColor, 0.5f);
    } else {
        // No valid neighbor found - keep as invalid
        g_Output[uint2(x, y)] = float4(0, 0, 0, 0);
    }
}
