#pragma once

#include "AppState.hpp"
#include "sim/SimWorld.hpp"

#include <cstdint>
#include <memory>

namespace sim::app {

class GlShaderPipeline {
public:
    GlShaderPipeline();
    ~GlShaderPipeline();

    GlShaderPipeline(const GlShaderPipeline &) = delete;
    GlShaderPipeline &operator=(const GlShaderPipeline &) = delete;

    // Must be called once after an OpenGL 3.3 Core context is current.
    bool initialize();
    void shutdown();

    bool isReady() const noexcept;
    uint64_t outputTextureId() const noexcept;

    // Upload dirty simulation fields and execute the multi-pass GL3 shader pipeline.
    // Always finishes with GL_FRAMEBUFFER bound to 0 so ImGui renders cleanly afterward.
    bool render(const SimWorld &world, const GraphicsSettings &settings, int canvasPixelW, int canvasPixelH,
                float timeSec, bool dataDirty);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sim::app
