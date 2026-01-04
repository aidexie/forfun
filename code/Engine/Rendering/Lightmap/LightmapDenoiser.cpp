#include "LightmapDenoiser.h"
#include "Core/FFLog.h"
#include <OpenImageDenoise/oidn.hpp>

CLightmapDenoiser::CLightmapDenoiser() = default;

CLightmapDenoiser::~CLightmapDenoiser() {
    Shutdown();
}

bool CLightmapDenoiser::Initialize() {
    if (m_isReady) {
        return true;
    }

    try {
        // Create OIDN device (CPU)
        oidn::DeviceRef* device = new oidn::DeviceRef(oidn::newDevice());
        if (!*device) {
            m_lastError = "Failed to create OIDN device";
            CFFLog::Error("[LightmapDenoiser] %s", m_lastError);
            delete device;
            return false;
        }

        // Commit device configuration
        (*device).commit();

        // Check for device errors
        const char* errorMessage = nullptr;
        if ((*device).getError(errorMessage) != oidn::Error::None) {
            m_lastError = errorMessage ? errorMessage : "Unknown OIDN device error";
            CFFLog::Error("[LightmapDenoiser] Device error: %s", m_lastError);
            delete device;
            return false;
        }

        m_device = device;
        m_isReady = true;

        CFFLog::Info("[LightmapDenoiser] Initialized successfully (OIDN %d.%d.%d)",
                     OIDN_VERSION_MAJOR, OIDN_VERSION_MINOR, OIDN_VERSION_PATCH);
        return true;
    }
    catch (const std::exception& e) {
        m_lastError = e.what();
        CFFLog::Error("[LightmapDenoiser] Exception during initialization: %s", m_lastError);
        return false;
    }
}

void CLightmapDenoiser::Shutdown() {
    if (m_filter) {
        delete static_cast<oidn::FilterRef*>(m_filter);
        m_filter = nullptr;
    }

    if (m_device) {
        delete static_cast<oidn::DeviceRef*>(m_device);
        m_device = nullptr;
    }

    m_isReady = false;
    CFFLog::Info("[LightmapDenoiser] Shutdown complete");
}

bool CLightmapDenoiser::Denoise(
    float* colorBuffer,
    int width,
    int height,
    float* normalBuffer,
    float* albedoBuffer)
{
    if (!m_isReady || !m_device) {
        m_lastError = "Denoiser not initialized";
        CFFLog::Error("[LightmapDenoiser] %s", m_lastError);
        return false;
    }

    if (!colorBuffer || width <= 0 || height <= 0) {
        m_lastError = "Invalid input parameters";
        CFFLog::Error("[LightmapDenoiser] %s", m_lastError);
        return false;
    }

    try {
        oidn::DeviceRef& device = *static_cast<oidn::DeviceRef*>(m_device);

        // Create RTLightmap filter (optimized for lightmaps)
        oidn::FilterRef filter = device.newFilter("RTLightmap");

        // Set input/output buffers
        // OIDN expects float3 (RGB) buffers with row-major layout
        filter.setImage("color", colorBuffer, oidn::Format::Float3, width, height);
        filter.setImage("output", colorBuffer, oidn::Format::Float3, width, height);  // In-place

        // Optional auxiliary buffers for better quality
        if (normalBuffer) {
            filter.setImage("normal", normalBuffer, oidn::Format::Float3, width, height);
        }
        if (albedoBuffer) {
            filter.setImage("albedo", albedoBuffer, oidn::Format::Float3, width, height);
        }

        // HDR input (lightmaps are HDR)
        filter.set("hdr", true);

        // Commit filter configuration
        filter.commit();

        // Check for filter errors
        const char* errorMessage = nullptr;
        if (device.getError(errorMessage) != oidn::Error::None) {
            m_lastError = errorMessage ? errorMessage : "Unknown OIDN filter error";
            CFFLog::Error("[LightmapDenoiser] Filter setup error: %s", m_lastError);
            return false;
        }

        CFFLog::Info("[LightmapDenoiser] Denoising %dx%d lightmap...", width, height);

        // Execute denoising
        filter.execute();

        // Check for execution errors
        if (device.getError(errorMessage) != oidn::Error::None) {
            m_lastError = errorMessage ? errorMessage : "Unknown OIDN execution error";
            CFFLog::Error("[LightmapDenoiser] Execution error: %s", m_lastError);
            return false;
        }

        CFFLog::Info("[LightmapDenoiser] Denoising complete");
        return true;
    }
    catch (const std::exception& e) {
        m_lastError = e.what();
        CFFLog::Error("[LightmapDenoiser] Exception during denoising: %s", m_lastError);
        return false;
    }
}
