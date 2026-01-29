// RHI/PerFrameSlots.h
#pragma once
#include <cstdint>

namespace PerFrameSlots {

//==============================================
// Constant Buffers (b-registers, space0)
// Range: b0-b7 (8 slots)
//==============================================
namespace CB {
    constexpr uint32_t PerFrame        = 0;  // Camera, Time, global settings
    constexpr uint32_t Clustered       = 1;  // Cluster grid params
    constexpr uint32_t Volumetric      = 2;  // Lightmap params
    constexpr uint32_t ReflectionProbe = 3;  // Probe selection data
    constexpr uint32_t LightProbe      = 4;  // Light probe params (count, blend settings)
    // b5-b7: Reserved for future
}

//==============================================
// Textures (t-registers, space0)
// Range: t0-t31 (32 slots)
//==============================================
namespace Tex {
    // Global resources (t0-t3)
    constexpr uint32_t ShadowMapArray   = 0;
    constexpr uint32_t BrdfLUT          = 1;
    constexpr uint32_t IrradianceArray  = 2;
    constexpr uint32_t PrefilteredArray = 3;

    // ClusteredLighting (t4-t7)
    // t4 = g_clusterData (ClusterData structs), t5 = g_compactLightList (uint indices)
    constexpr uint32_t Clustered_LightGrid      = 4;  // ClusterData array
    constexpr uint32_t Clustered_LightIndexList = 5;  // Compact light index list
    constexpr uint32_t Clustered_LightData      = 6;
    constexpr uint32_t Clustered_Reserved       = 7;

    // VolumetricLightmap (t8-t12)
    constexpr uint32_t Volumetric_SH_R     = 8;
    constexpr uint32_t Volumetric_SH_G     = 9;
    constexpr uint32_t Volumetric_SH_B     = 10;
    constexpr uint32_t Volumetric_Octree   = 11;
    constexpr uint32_t Volumetric_Reserved = 12;

    // ReflectionProbes (t13-t14)
    constexpr uint32_t ReflectionProbe_Array    = 13;
    constexpr uint32_t ReflectionProbe_Indices  = 14;

    // LightProbes (t15-t16)
    constexpr uint32_t LightProbe_Buffer   = 15;  // StructuredBuffer<LightProbeData>
    constexpr uint32_t LightProbe_Reserved = 16;

    // t17-t31: Reserved for future (DDGI, RTGI, etc.)
}

//==============================================
// Samplers (s-registers, space0)
// Range: s0-s7 (8 slots)
//==============================================
namespace Samp {
    constexpr uint32_t LinearClamp = 0;
    constexpr uint32_t LinearWrap  = 1;
    constexpr uint32_t PointClamp  = 2;
    constexpr uint32_t ShadowCmp   = 3;
    constexpr uint32_t Aniso       = 4;
    // s5-s7: Reserved for future
}

} // namespace PerFrameSlots
