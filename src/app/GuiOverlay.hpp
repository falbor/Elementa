#pragma once

#include "AppState.hpp"
#include "sim/SimWorld.hpp"

#include <cstdint>
#include <memory>

namespace sim::app {

class GuiOverlay {
public:
    GuiOverlay();
    ~GuiOverlay();

    GuiOverlay(const GuiOverlay &) = delete;
    GuiOverlay &operator=(const GuiOverlay &) = delete;

    void initialize(float uiScale, float pixelDensity = 1.0f);
    void applyScale(float uiScale, float pixelDensity = 1.0f);
    GuiFrameOutput renderFrame(const SimWorld &world, const AppState &state, uint64_t simTextureId,
                               bool simTextureBottomLeftOrigin = false);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sim::app
