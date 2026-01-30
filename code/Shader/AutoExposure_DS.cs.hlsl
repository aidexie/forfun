// ============================================
// AutoExposure_DS.cs.hlsl - Auto Exposure (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
//
// Entry Points:
//   CSBuildHistogram - Build luminance histogram from HDR buffer
//   CSAdaptExposure  - Calculate average luminance and adapt exposure
//
// Reference: "Automatic Exposure" - Krzysztof Narkowicz
//            https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/
// ============================================

// ============================================
// Constants
// ============================================
#define HISTOGRAM_BINS 256
#define HISTOGRAM_THREAD_GROUP_SIZE 16
#define EPSILON 0.0001

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_AutoExposure : register(b0, space1) {
    float2 gScreenSize;
    float2 gRcpScreenSize;
    float gMinLogLuminance;
    float gMaxLogLuminance;
    float gCenterWeight;
    float gDeltaTime;
    float gMinExposure;
    float gMaxExposure;
    float gAdaptSpeedUp;
    float gAdaptSpeedDown;
    float gExposureCompensation;
    float _pad0;
    float _pad1;
    float _pad2;
};

// Texture SRVs (t0-t7, space1)
Texture2D<float4> gHDRInput : register(t0, space1);

// Structured Buffer SRVs - use t1 for histogram read in adaptation pass
StructuredBuffer<uint> gHistogramSRV : register(t1, space1);

// UAVs (u0-u3, space1)
RWStructuredBuffer<uint> gHistogram : register(u0, space1);
RWStructuredBuffer<float> gExposure : register(u1, space1);  // [0] = current, [1] = target, [2] = maxHistogramValue

// Samplers (s0-s3, space1) - not used but declared for layout compatibility
SamplerState gPointSampler : register(s0, space1);
SamplerState gLinearSampler : register(s1, space1);

// Shared memory for histogram accumulation
groupshared uint gs_Histogram[HISTOGRAM_BINS];

// ============================================
// Helper Functions
// ============================================

// Convert RGB to luminance (Rec. 709)
float RGBToLuminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Map luminance to histogram bin index
uint LuminanceToBin(float luminance) {
    if (luminance < EPSILON) {
        return 0;
    }

    float logLum = log2(luminance);
    float normalizedLog = saturate((logLum - gMinLogLuminance) / (gMaxLogLuminance - gMinLogLuminance));
    return uint(normalizedLog * (HISTOGRAM_BINS - 1));
}

// Map histogram bin to luminance
float BinToLuminance(uint bin) {
    float normalizedLog = (float(bin) + 0.5) / float(HISTOGRAM_BINS);
    float logLum = lerp(gMinLogLuminance, gMaxLogLuminance, normalizedLog);
    return exp2(logLum);
}

// Calculate center weight (Gaussian falloff from screen center)
float GetCenterWeight(float2 uv) {
    float2 offset = (uv - 0.5) * 2.0;  // [-1, 1]
    float distSq = dot(offset, offset);
    float sigma = 0.5;  // Controls falloff rate
    float gaussianWeight = exp(-distSq / (2.0 * sigma * sigma));
    return lerp(1.0, gaussianWeight, gCenterWeight);
}

// ============================================
// CSBuildHistogram - Build Luminance Histogram
// ============================================
// Dispatched with (width/16, height/16, 1) groups
// Each thread processes one pixel
// Uses shared memory for local accumulation, then atomic add to global
// ============================================
[numthreads(HISTOGRAM_THREAD_GROUP_SIZE, HISTOGRAM_THREAD_GROUP_SIZE, 1)]
void CSBuildHistogram(
    uint3 dispatchThreadID : SV_DispatchThreadID,
    uint3 groupThreadID : SV_GroupThreadID,
    uint groupIndex : SV_GroupIndex)
{
    // Clear shared memory histogram
    if (groupIndex < HISTOGRAM_BINS) {
        gs_Histogram[groupIndex] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Process pixel (all threads execute, but only valid ones contribute)
    uint2 pixelCoord = dispatchThreadID.xy;
    bool validPixel = (pixelCoord.x < uint(gScreenSize.x)) && (pixelCoord.y < uint(gScreenSize.y));

    if (validPixel) {
        // Sample HDR color
        float3 hdrColor = gHDRInput[pixelCoord].rgb;

        // Calculate luminance
        float luminance = RGBToLuminance(hdrColor);

        // Get histogram bin
        uint binIndex = LuminanceToBin(luminance);

        // Calculate center weight
        float2 uv = (float2(pixelCoord) + 0.5) * gRcpScreenSize;
        float weight = GetCenterWeight(uv);

        // Scale weight to integer (preserve precision)
        uint weightedCount = uint(weight * 1000.0);

        // Accumulate to shared memory
        InterlockedAdd(gs_Histogram[binIndex], weightedCount);
    }

    // Sync before writing to global (all threads must hit this)
    GroupMemoryBarrierWithGroupSync();

    // Write shared memory to global histogram
    if (groupIndex < HISTOGRAM_BINS) {
        InterlockedAdd(gHistogram[groupIndex], gs_Histogram[groupIndex]);
    }
}

// ============================================
// Shared memory for parallel reduction
// ============================================
groupshared float gs_PartialSums[HISTOGRAM_BINS];
groupshared float gs_PartialWeights[HISTOGRAM_BINS];
groupshared uint gs_MaxBinValue[HISTOGRAM_BINS];

// ============================================
// CSAdaptExposure - Calculate and Adapt Exposure
// ============================================
// Dispatched with (1, 1, 1) - single thread group
// 256 threads perform parallel reduction on histogram
// Thread 0 computes final exposure and applies adaptation
// ============================================
[numthreads(HISTOGRAM_BINS, 1, 1)]
void CSAdaptExposure(
    uint3 dispatchThreadID : SV_DispatchThreadID,
    uint groupIndex : SV_GroupIndex)
{
    // Each thread loads one histogram bin
    uint binValue = gHistogramSRV[groupIndex];
    float weight = float(binValue) / 1000.0;  // Undo weight scaling

    // Calculate weighted luminance contribution
    float luminance = BinToLuminance(groupIndex);
    float logLum = log2(max(luminance, EPSILON));

    // Store in shared memory for reduction
    gs_PartialSums[groupIndex] = weight * logLum;
    gs_PartialWeights[groupIndex] = weight;
    gs_MaxBinValue[groupIndex] = binValue;

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction (sum for avg, max for histogram debug)
    for (uint stride = HISTOGRAM_BINS / 2; stride > 0; stride >>= 1) {
        if (groupIndex < stride) {
            gs_PartialSums[groupIndex] += gs_PartialSums[groupIndex + stride];
            gs_PartialWeights[groupIndex] += gs_PartialWeights[groupIndex + stride];
            gs_MaxBinValue[groupIndex] = max(gs_MaxBinValue[groupIndex], gs_MaxBinValue[groupIndex + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Thread 0 computes final exposure
    if (groupIndex == 0) {
        float totalWeight = max(gs_PartialWeights[0], 1.0);
        float avgLogLum = gs_PartialSums[0] / totalWeight;
        float avgLuminance = exp2(avgLogLum);

        // Target exposure: bring average luminance to middle gray (0.18)
        // exposure = targetLuminance / sceneLuminance
        float targetLuminance = 0.18;
        float targetExposure = targetLuminance / max(avgLuminance, EPSILON);

        // Apply exposure compensation (in EV stops)
        targetExposure *= exp2(gExposureCompensation);

        // Clamp to valid range
        targetExposure = clamp(targetExposure, gMinExposure, gMaxExposure);

        // Read previous exposure
        float currentExposure = gExposure[0];

        // First frame initialization
        if (currentExposure <= 0.0 || isnan(currentExposure) || isinf(currentExposure)) {
            currentExposure = targetExposure;
        }

        // Asymmetric adaptation speed
        // Dark -> Bright: faster (eyes adjust quickly to bright light)
        // Bright -> Dark: slower (eyes take longer to adjust to darkness)
        float adaptSpeed = (targetExposure > currentExposure) ? gAdaptSpeedUp : gAdaptSpeedDown;

        // Exponential smoothing
        float alpha = 1.0 - exp(-gDeltaTime / max(adaptSpeed, 0.001));
        float newExposure = lerp(currentExposure, targetExposure, alpha);

        // Clamp final exposure
        newExposure = clamp(newExposure, gMinExposure, gMaxExposure);

        // Write results
        gExposure[0] = newExposure;              // Current (smoothed) exposure
        gExposure[1] = targetExposure;           // Target exposure (for debug)
        gExposure[2] = float(gs_MaxBinValue[0]); // Max histogram bin value (for debug visualization)
    }
}
