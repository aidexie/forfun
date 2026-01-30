// Grid_DS.ps.hlsl
// Pixel Shader for infinite grid rendering (SM 5.1 - Descriptor Set path)
// Uses GPU depth test (SV_DEPTH output)

// PerPass constant buffer (space1)
cbuffer CBPerFrame : register(b0, space1) {
    matrix gViewProj;          // View * Projection matrix
    matrix gInvViewProj;       // Inverse of View * Projection
    float3 gCameraPos;         // Camera world position
    float  gGridFadeStart;     // Distance where grid starts fading (e.g., 50m)
    float  gGridFadeEnd;       // Distance where grid fully fades (e.g., 100m)
    float3 gPadding;
};

struct PSInput {
    float4 positionCS : SV_POSITION;
    float2 clipPos    : TEXCOORD0;
};

struct PSOutput {
    float4 color : SV_TARGET;
    float depth  : SV_DEPTH;  // GPU depth test
};

// Calculate grid line intensity for a given scale
float GridLine(float2 coord, float scale) {
    float2 grid = abs(frac(coord / scale - 0.5) - 0.5) / fwidth(coord / scale);
    float lineIntensity = min(grid.x, grid.y);
    return 1.0 - min(lineIntensity, 1.0);
}

PSOutput main(PSInput input) {
    PSOutput output;

    // Reconstruct view ray direction (from camera through this pixel)
    // Use far plane (depth = 1.0) to get ray direction
    float4 farWorld = mul(float4(input.clipPos, 1.0, 1.0), gInvViewProj);
    farWorld /= farWorld.w;
    float3 viewDir = normalize(farWorld.xyz - gCameraPos);

    // Grid is on XZ plane at Y=0
    // Calculate intersection of view ray with Y=0 plane

    // If looking up (away from grid), discard
    if (viewDir.y > 0.0) {
        discard;
    }

    // Ray-plane intersection: find t where (cameraPos + t * viewDir).y = 0
    // Avoid division by zero
    if (abs(viewDir.y) < 0.0001) {
        discard;
    }

    float t = -gCameraPos.y / viewDir.y;

    // If intersection is behind camera, discard
    if (t < 0.0) {
        discard;
    }

    float3 gridPos = gCameraPos + viewDir * t;

    // Transform grid position to clip space for depth output
    float4 gridClipPos = mul(float4(gridPos, 1.0), gViewProj);
    gridClipPos /= gridClipPos.w;
    output.depth = gridClipPos.z;  // GPU will depth test against scene

    // Distance fade (using gGridFadeStart/End parameters, default 50-100m)
    float distanceToCamera = length(gridPos - gCameraPos);
    float fadeFactor = 1.0 - smoothstep(gGridFadeStart, gGridFadeEnd, distanceToCamera);
    if (fadeFactor < 0.01) {
        discard;
    }

    // Generate grid pattern (dual-scale: 1m fine + 10m coarse)
    // Using original intensity: 0.3 and 0.6
    float2 coord = gridPos.xz;
    float fineGrid = GridLine(coord, 1.0);
    float coarseGrid = GridLine(coord, 10.0);
    float grid = max(fineGrid * 0.3, coarseGrid * 0.6);

    // View angle fade (reduce grid at shallow angles to prevent moire)
    // Using original parameters: smoothstep(0.0, 0.2)
    float viewAngleFade = abs(viewDir.y);
    viewAngleFade = smoothstep(0.0, 0.2, viewAngleFade);
    grid *= viewAngleFade;

    // Distance fade
    grid *= fadeFactor;

    // Unity-style grid color (neutral gray, slightly cool tone)
    // Using original color
    float3 gridColor = float3(0.47, 0.47, 0.50);  // Similar to Unity's grid
    output.color = float4(gridColor, grid);

    return output;
}
