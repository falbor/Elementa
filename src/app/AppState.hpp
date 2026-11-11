#pragma once

#include "sim/SimWorld.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace sim::app {

inline constexpr int kMinBrushRadius = 0;
inline constexpr int kMaxBrushRadius = 35;

struct UiRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const noexcept { return std::max(0.0f, right - left); }
    float height() const noexcept { return std::max(0.0f, bottom - top); }
    bool contains(float x, float y) const noexcept { return x >= left && x < right && y >= top && y < bottom; }
};

enum class BannerTone : uint8_t { Info = 0, Success, Error };

struct StatusBannerState {
    std::string text;
    BannerTone tone = BannerTone::Info;
    std::chrono::steady_clock::time_point expiresAt{};

    bool isActive(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const noexcept {
        return !text.empty() && now < expiresAt;
    }

    void show(std::string message, BannerTone bannerTone, int durationMs = 4000) {
        text = std::move(message);
        tone = bannerTone;
        expiresAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    }

    void clear() noexcept {
        text.clear();
        expiresAt = {};
    }
};

struct GraphicsSettings {
    bool shadersEnabled = true;
    bool shadowsEnabled = true;
    bool refractionEnabled = true;
    bool bloomEnabled = true;
    float ambientLight = 0.34f;   // [0.05 .. 1.00]
    float sunIntensity = 0.65f;   // [0.00 .. 1.50]
    float bloomIntensity = 0.65f; // [0.00 .. 1.50]
};

struct AppState {
    ViewMode viewMode = ViewMode::Physical;
    ToolMode toolMode = ToolMode::Material;
    MaterialId paintMat = MaterialId::Sand;
    PresetId currentPreset = PresetId::Laboratory;
    int brushRadius = 5;
    int stepsPerFrame = 1;
    bool paused = false;

    GraphicsSettings gfx{};
    bool glShadersAvailable = false;

    UiRect canvasDrawRect{};
    int hoverSimX = -1;
    int hoverSimY = -1;
    bool leftDown = false;
    bool rightDown = false;
    int prevStrokeSimX = -1;
    int prevStrokeSimY = -1;

    bool lineDrawing = false;
    bool lineIsRight = false;
    int lineStartSimX = -1;
    int lineStartSimY = -1;
    int lineEndSimX = -1;
    int lineEndSimY = -1;

    double fps = 60.0;
    float uiScale = 1.0f;
    float pixelDensity = 1.0f;
    bool showSidebarGraphics = true;
    bool showSidebarInfo = true;
    bool showSidebarHints = true;
    StatusBannerState banner{};
};

struct GuiFrameOutput {
    UiRect canvasDrawRect{};
    bool canvasHovered = false;
    bool sidebarScrollable = false;

    bool triggerPlayPause = false;
    bool triggerStepOnce = false;
    bool triggerResetPreset = false;
    bool triggerClear = false;
    bool triggerExportSnapshot = false;
    bool triggerCycleGravity = false;
    bool triggerToggleBoundary = false;
    bool triggerToggleSpeed = false;

    bool hasViewChange = false;
    ViewMode newView = ViewMode::Physical;

    bool hasPresetChange = false;
    PresetId newPreset = PresetId::Laboratory;

    bool hasToolChange = false;
    ToolMode newTool = ToolMode::Material;

    bool hasMaterialChange = false;
    MaterialId newMaterial = MaterialId::Sand;

    bool hasBrushChange = false;
    int newBrushRadius = 5;

    bool hasGraphicsChange = false;
    GraphicsSettings newGraphics{};

    bool hasSidebarGraphicsToggle = false;
    bool showSidebarGraphics = true;
    bool hasSidebarInfoToggle = false;
    bool showSidebarInfo = true;
    bool hasSidebarHintsToggle = false;
    bool showSidebarHints = true;
};

inline bool screenToSim(const UiRect &rect, int worldW, int worldH, float sx, float sy, int &outSimX,
                        int &outSimY) noexcept {
    const float cw = rect.width();
    const float ch = rect.height();
    if (cw <= 0.0f || ch <= 0.0f || worldW <= 0 || worldH <= 0 || !rect.contains(sx, sy)) return false;
    outSimX = std::clamp(static_cast<int>(((sx - rect.left) * static_cast<float>(worldW)) / cw), 0, worldW - 1);
    outSimY = std::clamp(static_cast<int>(((sy - rect.top) * static_cast<float>(worldH)) / ch), 0, worldH - 1);
    return true;
}

inline bool screenToSimClamped(const UiRect &rect, int worldW, int worldH, float sx, float sy, int &outSimX,
                               int &outSimY) noexcept {
    const float cw = rect.width();
    const float ch = rect.height();
    if (cw <= 0.0f || ch <= 0.0f || worldW <= 0 || worldH <= 0) return false;
    const float u = std::clamp((sx - rect.left) / cw, 0.0f, 0.999999f);
    const float v = std::clamp((sy - rect.top) / ch, 0.0f, 0.999999f);
    outSimX = std::clamp(static_cast<int>(u * static_cast<float>(worldW)), 0, worldW - 1);
    outSimY = std::clamp(static_cast<int>(v * static_cast<float>(worldH)), 0, worldH - 1);
    return true;
}

inline void simToScreenCenter(const UiRect &rect, int worldW, int worldH, int simX, int simY, float &outX,
                              float &outY) noexcept {
    const float cw = std::max(1.0f, rect.width());
    const float ch = std::max(1.0f, rect.height());
    const float ww = static_cast<float>(std::max(1, worldW));
    const float hh = static_cast<float>(std::max(1, worldH));
    outX = rect.left + (static_cast<float>(simX) + 0.5f) * (cw / ww);
    outY = rect.top + (static_cast<float>(simY) + 0.5f) * (ch / hh);
}

} // namespace sim::app
