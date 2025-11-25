#pragma once
#include <string>
#include <sstream>
#include <iomanip>

// CRenderStats: Performance metrics tracking for AI testing
// Singleton that collects rendering statistics for automated verification
class CRenderStats {
public:
    // Singleton access
    static CRenderStats& Instance() {
        static CRenderStats instance;
        return instance;
    }

    // Delete copy/move constructors
    CRenderStats(const CRenderStats&) = delete;
    CRenderStats& operator=(const CRenderStats&) = delete;
    CRenderStats(CRenderStats&&) = delete;
    CRenderStats& operator=(CRenderStats&&) = delete;

    // Frame timing
    void RecordFrameTime(float deltaTime) {
        m_lastFrameTime = deltaTime;
        m_frameCount++;
        m_totalTime += deltaTime;
    }

    // Draw call tracking
    void RecordDrawCall(int vertexCount = 0, int indexCount = 0) {
        m_drawCallCount++;
        m_totalVertices += vertexCount;
        m_totalIndices += indexCount;
    }

    // Shadow pass tracking
    void RecordShadowPass(int cascadeIndex, int drawCalls) {
        if (cascadeIndex >= 0 && cascadeIndex < 4) {
            m_shadowDrawCalls[cascadeIndex] = drawCalls;
        }
    }

    // Reset per-frame counters (call at frame start)
    void BeginFrame() {
        m_drawCallCount = 0;
        m_totalVertices = 0;
        m_totalIndices = 0;
        for (int i = 0; i < 4; ++i) {
            m_shadowDrawCalls[i] = 0;
        }
    }

    // Reset all statistics
    void Reset() {
        m_frameCount = 0;
        m_totalTime = 0.0f;
        m_lastFrameTime = 0.0f;
        BeginFrame();
    }

    // Generate report
    std::string GenerateReport() const {
        std::ostringstream oss;

        oss << "================================\n";
        oss << "[RENDER STATS REPORT]\n";
        oss << "================================\n\n";

        // Frame timing
        oss << "[Frame Timing]\n";
        oss << "  Frame Count: " << m_frameCount << "\n";
        oss << "  Last Frame Time: " << std::fixed << std::setprecision(2)
            << (m_lastFrameTime * 1000.0f) << " ms\n";

        if (m_lastFrameTime > 0.0f) {
            float fps = 1.0f / m_lastFrameTime;
            oss << "  Last FPS: " << std::fixed << std::setprecision(1) << fps << "\n";
        }

        if (m_frameCount > 0) {
            float avgFrameTime = m_totalTime / m_frameCount;
            float avgFPS = 1.0f / avgFrameTime;
            oss << "  Average Frame Time: " << std::fixed << std::setprecision(2)
                << (avgFrameTime * 1000.0f) << " ms\n";
            oss << "  Average FPS: " << std::fixed << std::setprecision(1) << avgFPS << "\n";
        }

        // Draw calls
        oss << "\n[Draw Calls]\n";
        oss << "  Main Pass Draw Calls: " << m_drawCallCount << "\n";
        oss << "  Total Vertices: " << m_totalVertices << "\n";
        oss << "  Total Indices: " << m_totalIndices << "\n";

        // Shadow stats
        oss << "\n[Shadow Pass]\n";
        int totalShadowDrawCalls = 0;
        for (int i = 0; i < 4; ++i) {
            if (m_shadowDrawCalls[i] > 0) {
                oss << "  Cascade " << i << " Draw Calls: " << m_shadowDrawCalls[i] << "\n";
                totalShadowDrawCalls += m_shadowDrawCalls[i];
            }
        }
        oss << "  Total Shadow Draw Calls: " << totalShadowDrawCalls << "\n";

        oss << "\n================================\n";

        return oss.str();
    }

    // Accessors for individual metrics (for assertions)
    int GetFrameCount() const { return m_frameCount; }
    float GetLastFrameTime() const { return m_lastFrameTime; }
    float GetAverageFrameTime() const { return m_frameCount > 0 ? m_totalTime / m_frameCount : 0.0f; }
    int GetDrawCallCount() const { return m_drawCallCount; }
    int GetTotalVertices() const { return m_totalVertices; }
    int GetTotalIndices() const { return m_totalIndices; }

private:
    CRenderStats() = default;
    ~CRenderStats() = default;

    // Frame timing
    int m_frameCount = 0;
    float m_totalTime = 0.0f;
    float m_lastFrameTime = 0.0f;

    // Per-frame draw stats
    int m_drawCallCount = 0;
    int m_totalVertices = 0;
    int m_totalIndices = 0;

    // Shadow stats
    int m_shadowDrawCalls[4] = {0, 0, 0, 0};
};
