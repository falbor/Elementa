#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "sim/SimWorld.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace sim::app {

struct UiButton {
    RECT rect{};
    std::wstring label;
    int group = 0;
    int id = 0;
    COLORREF accent = RGB(59, 130, 246);
    bool hasSwatch = false;
    COLORREF swatchColor = RGB(255, 255, 255);
    bool active = false;
};

class Win32App {
public:
    explicit Win32App(SimWorld& world,
                      ViewMode initialView = ViewMode::Physical,
                      PresetId initialPreset = PresetId::Laboratory);
    ~Win32App();

    Win32App(const Win32App&) = delete;
    Win32App& operator=(const Win32App&) = delete;

    bool create(int windowWidth = 1480, int windowHeight = 900);
    int run();

private:
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void recreateFonts();
    void rebuildLayout();
    void ensureBackbuffer(int width, int height);
    int uiPx(int basePx) const noexcept;
    void renderFrame();
    void drawToolbar(HDC dc);
    void drawSidebar(HDC dc);
    void drawCanvas(HDC dc);
    void drawLinePreview(HDC dc) const;
    void drawStatusBar(HDC dc);
    void drawButton(HDC dc, const UiButton& btn, bool hovered) const;

    bool screenToSim(int sx, int sy, int& outSimX, int& outSimY) const noexcept;
    bool screenToSimClamped(int sx, int sy, int& outSimX, int& outSimY) const noexcept;
    void simToScreenCenter(int simX, int simY, int& outScreenX, int& outScreenY) const noexcept;

    void handleMouseAction(int sx, int sy, bool isLeft, bool isRight, bool isMiddle, bool isDrag);
    void handleButtonClick(const UiButton& btn);
    void handleKeyDown(WPARAM vk, bool isRepeat = false);
    void exportSnapshot();
    void showStatusBanner(const std::wstring& msg,
                          COLORREF fill,
                          COLORREF border,
                          COLORREF text,
                          int durationMs = 4000);
    std::wstring activeToolSummary() const;
    std::wstring activeToolDetails() const;

    SimWorld& m_world;
    HWND m_hwnd = nullptr;

    // Backbuffer GDI resources
    HDC m_backDC = nullptr;
    HBITMAP m_backBmp = nullptr;
    HBITMAP m_oldBmp = nullptr;
    int m_clientW = 0;
    int m_clientH = 0;
    float m_uiScale = 1.0f;

    HFONT m_fontUI = nullptr;
    HFONT m_fontBold = nullptr;
    HFONT m_fontMono = nullptr;
    HFONT m_fontSmall = nullptr;

    // Layout regions
    int m_toolbarHeightPx = 56;
    RECT m_toolbarRect{};
    RECT m_sidebarRect{};
    RECT m_canvasBounds{};
    RECT m_canvasDrawRect{};
    RECT m_statusRect{};

    RECT m_brushHeaderRect{};
    RECT m_brushValueRect{};
    RECT m_toolsHeaderRect{};
    RECT m_materialsHeaderRect{};
    RECT m_sidebarInfoRect{};
    RECT m_sidebarHintsRect{};
    bool m_showSidebarInfo = false;
    bool m_showSidebarHints = false;

    std::vector<UiButton> m_buttons;

    // Interactive state
    ViewMode m_viewMode = ViewMode::Physical;
    ToolMode m_toolMode = ToolMode::Material;
    MaterialId m_paintMat = MaterialId::Sand;
    PresetId m_currentPreset = PresetId::Laboratory;
    int m_brushRadius = 5;
    int m_stepsPerFrame = 1;
    bool m_paused = false;

    int m_mouseScreenX = -1;
    int m_mouseScreenY = -1;
    int m_hoverSimX = -1;
    int m_hoverSimY = -1;
    bool m_leftDown = false;
    bool m_rightDown = false;
    bool m_trackingMouse = false;
    int m_prevStrokeSimX = -1;
    int m_prevStrokeSimY = -1;

    // Shift+Drag straight-line state
    bool m_lineDrawing = false;
    bool m_lineIsRight = false;
    int m_lineStartSimX = -1;
    int m_lineStartSimY = -1;
    int m_lineEndSimX = -1;
    int m_lineEndSimY = -1;

    // Transient confirmation banner in status bar
    std::wstring m_statusBanner;
    std::chrono::steady_clock::time_point m_statusBannerUntil{};
    COLORREF m_statusBannerFill = RGB(16, 48, 36);
    COLORREF m_statusBannerBorder = RGB(52, 211, 153);
    COLORREF m_statusBannerText = RGB(167, 243, 208);

    double m_fps = 60.0;
};

} // namespace sim::app
