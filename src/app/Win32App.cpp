#include "Win32App.hpp"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace sim::app {

namespace {

constexpr int kSidebarWidth = 236;
constexpr int kStatusHeight = 50;

enum ButtonGroup : int {
    GrpAction = 1,
    GrpView = 2,
    GrpPreset = 3,
    GrpBrush = 4,
    GrpTool = 5,
    GrpMaterial = 6
};

enum ActionId : int {
    ActPlayPause = 0,
    ActStep,
    ActReset,
    ActClear,
    ActSnapshot,
    ActGravity,
    ActBoundary,
    ActSpeed
};

std::wstring utf8ToWide(std::string_view sv) {
    if (sv.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), needed);
    return out;
}

const wchar_t* phaseNameIt(PhaseState p) noexcept {
    switch (p) {
    case PhaseState::EmptyGas: return L"Gas/Aria";
    case PhaseState::Powder: return L"Polvere";
    case PhaseState::Liquid: return L"Liquido";
    case PhaseState::StaticSolid: return L"Solido";
    case PhaseState::Energy: return L"Energia";
    }
    return L"-";
}

const wchar_t* gravityNameIt(GravityMode g) noexcept {
    switch (g) {
    case GravityMode::Earth: return L"G: Terra";
    case GravityMode::ZeroG: return L"G: Zero-G";
    case GravityMode::Inverted: return L"G: Inversa";
    case GravityMode::RadialCenter: return L"G: Centro";
    }
    return L"G: Terra";
}

const wchar_t* toolNameIt(ToolMode t) noexcept {
    switch (t) {
    case ToolMode::Material: return L"Pittura";
    case ToolMode::Wind: return L"Vento";
    case ToolMode::Heat: return L"Calore";
    case ToolMode::Cryo: return L"Freddo";
    case ToolMode::SparkTool: return L"Scintilla";
    case ToolMode::Attractor: return L"Attrae";
    case ToolMode::Repulsor: return L"Respinge";
    case ToolMode::Erase: return L"Gomma";
    }
    return L"Pittura";
}

const wchar_t* toolHintIt(ToolMode t) noexcept {
    switch (t) {
    case ToolMode::Material: return L"Deposita materiale";
    case ToolMode::Wind: return L"Impulso flusso aria";
    case ToolMode::Heat: return L"Torcia termica +180K";
    case ToolMode::Cryo: return L"Raffredda -120K";
    case ToolMode::SparkTool: return L"Scarica elettrica +1.4V";
    case ToolMode::Attractor: return L"Campo attrattivo radiale";
    case ToolMode::Repulsor: return L"Campo repulsivo radiale";
    case ToolMode::Erase: return L"Cancella in Aria";
    }
    return L"-";
}

COLORREF toolAccentColor(ToolMode t) noexcept {
    switch (t) {
    case ToolMode::Material: return RGB(59, 130, 246);
    case ToolMode::Wind: return RGB(56, 189, 248);
    case ToolMode::Heat: return RGB(249, 115, 22);
    case ToolMode::Cryo: return RGB(125, 211, 252);
    case ToolMode::SparkTool: return RGB(250, 204, 21);
    case ToolMode::Attractor: return RGB(168, 85, 247);
    case ToolMode::Repulsor: return RGB(244, 63, 94);
    case ToolMode::Erase: return RGB(248, 113, 113);
    }
    return RGB(59, 130, 246);
}

void fillSolidRect(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

void drawRoundedBox(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius = 5) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

} // namespace

Win32App::Win32App(SimWorld& world, ViewMode initialView, PresetId initialPreset)
    : m_world(world)
    , m_viewMode(initialView)
    , m_currentPreset(initialPreset) {}

Win32App::~Win32App() {
    if (m_backDC) {
        SelectObject(m_backDC, GetStockObject(DEFAULT_GUI_FONT));
        if (m_oldBmp) SelectObject(m_backDC, m_oldBmp);
        if (m_backBmp) DeleteObject(m_backBmp);
        DeleteDC(m_backDC);
        m_backDC = nullptr;
        m_backBmp = nullptr;
        m_oldBmp = nullptr;
    }
    if (m_fontUI) { DeleteObject(m_fontUI); m_fontUI = nullptr; }
    if (m_fontBold) { DeleteObject(m_fontBold); m_fontBold = nullptr; }
    if (m_fontMono) { DeleteObject(m_fontMono); m_fontMono = nullptr; }
    if (m_fontSmall) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
}

int Win32App::uiPx(int basePx) const noexcept {
    return static_cast<int>(std::round(static_cast<float>(basePx) * m_uiScale));
}

void Win32App::recreateFonts() {
    // Deselect custom fonts before deleting to guarantee zero GDI leaks
    if (m_backDC) {
        SelectObject(m_backDC, GetStockObject(DEFAULT_GUI_FONT));
    }
    if (m_fontUI) { DeleteObject(m_fontUI); m_fontUI = nullptr; }
    if (m_fontBold) { DeleteObject(m_fontBold); m_fontBold = nullptr; }
    if (m_fontMono) { DeleteObject(m_fontMono); m_fontMono = nullptr; }
    if (m_fontSmall) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }

    const int fontH = uiPx(15);
    const int monoH = uiPx(14);
    const int smallH = uiPx(13);
    m_fontUI = CreateFontW(fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_fontBold = CreateFontW(fontH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_fontMono = CreateFontW(monoH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    m_fontSmall = CreateFontW(smallH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

bool Win32App::create(int windowWidth, int windowHeight) {
    // Enable Per-Monitor DPI awareness v2 for crisp rendering across displays
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
        auto fn = reinterpret_cast<SetDpiCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            fn(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        } else {
            SetProcessDPIAware();
        }
        using GetDpiSysFn = UINT(WINAPI*)();
        if (auto dpiFn = reinterpret_cast<GetDpiSysFn>(GetProcAddress(user32, "GetDpiForSystem"))) {
            const UINT dpi = dpiFn();
            if (dpi > 0) {
                m_uiScale = std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 2.25f);
            }
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &Win32App::staticWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PixelPhysicsSim2DWindow";

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int desiredW = uiPx(windowWidth);
    const int desiredH = uiPx(windowHeight);
    const int w = std::min(desiredW, std::max(uiPx(800), sw - uiPx(40)));
    const int h = std::min(desiredH, std::max(uiPx(600), sh - uiPx(60)));
    const int x = std::max(0, (sw - w) / 2);
    const int y = std::max(0, (sh - h) / 2);

    m_hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Simulatore Fisico 2D Per-Pixel (C++20)",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        nullptr, nullptr, wc.hInstance, this);

    if (!m_hwnd) return false;

    if (user32) {
        using GetDpiWinFn = UINT(WINAPI*)(HWND);
        if (auto dpiWinFn = reinterpret_cast<GetDpiWinFn>(GetProcAddress(user32, "GetDpiForWindow"))) {
            const UINT dpi = dpiWinFn(m_hwnd);
            if (dpi > 0) {
                m_uiScale = std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 2.25f);
            }
        }
    }

    // Opt into immersive dark titlebar on Windows 10/11 when available
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto fn = reinterpret_cast<DwmSetAttrFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            const BOOL dark = TRUE;
            fn(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }

    recreateFonts();

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    ensureBackbuffer(rc.right - rc.left, rc.bottom - rc.top);
    rebuildLayout();
    m_world.render(m_viewMode);
    renderFrame();

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

int Win32App::run() {
    using Clock = std::chrono::steady_clock;
    auto prevTime = Clock::now();
    auto fpsTimer = prevTime;
    int frameCount = 0;

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const auto now = Clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(now - prevTime).count();
        if (elapsedMs >= 15.5) {
            prevTime = now;
            if (!m_paused) {
                for (int s = 0; s < m_stepsPerFrame; ++s) {
                    m_world.step();
                }
            }
            if (!IsIconic(m_hwnd)) {
                m_world.render(m_viewMode);
                renderFrame();
                InvalidateRect(m_hwnd, nullptr, FALSE);
                UpdateWindow(m_hwnd);
            }

            ++frameCount;
            const double fpsElapsed = std::chrono::duration<double>(now - fpsTimer).count();
            if (fpsElapsed >= 0.4) {
                m_fps = static_cast<double>(frameCount) / fpsElapsed;
                frameCount = 0;
                fpsTimer = now;
            }
        } else {
            Sleep(1);
        }
    }
}

LRESULT CALLBACK Win32App::staticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Win32App* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<Win32App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->m_hwnd = hwnd;
    } else {
        app = reinterpret_cast<Win32App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app) return app->wndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Win32App::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        const int w = LOWORD(lp);
        const int h = HIWORD(lp);
        if (w > 0 && h > 0) {
            ensureBackbuffer(w, h);
            rebuildLayout();
            renderFrame();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
        if (newDpi > 0) {
            m_uiScale = std::clamp(static_cast<float>(newDpi) / 96.0f, 1.0f, 2.25f);
        }
        recreateFonts();
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        ensureBackbuffer(rc.right - rc.left, rc.bottom - rc.top);
        rebuildLayout();
        renderFrame();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (m_backDC && m_clientW > 0 && m_clientH > 0) {
            BitBlt(dc, 0, 0, m_clientW, m_clientH, m_backDC, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        m_mouseScreenX = GET_X_LPARAM(lp);
        m_mouseScreenY = GET_Y_LPARAM(lp);

        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) {
                m_trackingMouse = true;
            }
        }

        int sx = 0, sy = 0;
        if (m_lineDrawing) {
            if (screenToSimClamped(m_mouseScreenX, m_mouseScreenY, sx, sy)) {
                m_lineEndSimX = sx;
                m_lineEndSimY = sy;
            }
        }

        if (screenToSim(m_mouseScreenX, m_mouseScreenY, sx, sy)) {
            m_hoverSimX = sx;
            m_hoverSimY = sy;
            if (!m_lineDrawing) {
                if (m_leftDown) {
                    handleMouseAction(m_mouseScreenX, m_mouseScreenY, true, false, false, true);
                } else if (m_rightDown) {
                    handleMouseAction(m_mouseScreenX, m_mouseScreenY, false, true, false, true);
                }
            }
        } else {
            m_hoverSimX = -1;
            m_hoverSimY = -1;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        if (!m_leftDown && !m_rightDown) {
            m_mouseScreenX = -1;
            m_mouseScreenY = -1;
            m_hoverSimX = -1;
            m_hoverSimY = -1;
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != hwnd) {
            m_leftDown = false;
            m_rightDown = false;
            m_lineDrawing = false;
            m_prevStrokeSimX = -1;
            m_prevStrokeSimY = -1;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        m_mouseScreenX = GET_X_LPARAM(lp);
        m_mouseScreenY = GET_Y_LPARAM(lp);

        const POINT pt{m_mouseScreenX, m_mouseScreenY};
        for (const auto& btn : m_buttons) {
            if (PtInRect(&btn.rect, pt)) {
                handleButtonClick(btn);
                rebuildLayout();
                return 0;
            }
        }

        const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (altDown) {
            handleMouseAction(m_mouseScreenX, m_mouseScreenY, false, false, true, false);
            return 0;
        }

        if (m_lineDrawing && m_lineIsRight) {
            m_lineDrawing = false;
            m_rightDown = false;
            ReleaseCapture();
            return 0;
        }

        int sx = 0, sy = 0;
        if (screenToSim(m_mouseScreenX, m_mouseScreenY, sx, sy)) {
            SetCapture(hwnd);
            m_leftDown = true;
            m_prevStrokeSimX = -1;
            m_prevStrokeSimY = -1;
            const bool shiftDown = (wp & MK_SHIFT) != 0 || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shiftDown) {
                m_lineDrawing = true;
                m_lineIsRight = false;
                m_lineStartSimX = sx;
                m_lineStartSimY = sy;
                m_lineEndSimX = sx;
                m_lineEndSimY = sy;
            } else {
                m_lineDrawing = false;
                handleMouseAction(m_mouseScreenX, m_mouseScreenY, true, false, false, false);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        m_mouseScreenX = GET_X_LPARAM(lp);
        m_mouseScreenY = GET_Y_LPARAM(lp);
        if (m_lineDrawing && !m_lineIsRight) {
            int sx = 0, sy = 0;
            if (screenToSimClamped(m_mouseScreenX, m_mouseScreenY, sx, sy)) {
                m_lineEndSimX = sx;
                m_lineEndSimY = sy;
            }
            m_world.applyToolLine(m_toolMode, m_lineStartSimX, m_lineStartSimY,
                                  m_lineEndSimX, m_lineEndSimY, m_brushRadius, m_paintMat);
            m_lineDrawing = false;
        }
        m_leftDown = false;
        m_prevStrokeSimX = -1;
        m_prevStrokeSimY = -1;
        if (!m_rightDown) ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONDOWN: {
        m_mouseScreenX = GET_X_LPARAM(lp);
        m_mouseScreenY = GET_Y_LPARAM(lp);

        if (m_lineDrawing && !m_lineIsRight) {
            m_lineDrawing = false;
            m_leftDown = false;
            ReleaseCapture();
            return 0;
        }

        int sx = 0, sy = 0;
        if (screenToSim(m_mouseScreenX, m_mouseScreenY, sx, sy)) {
            SetCapture(hwnd);
            m_rightDown = true;
            m_prevStrokeSimX = -1;
            m_prevStrokeSimY = -1;
            const bool shiftDown = (wp & MK_SHIFT) != 0 || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shiftDown) {
                m_lineDrawing = true;
                m_lineIsRight = true;
                m_lineStartSimX = sx;
                m_lineStartSimY = sy;
                m_lineEndSimX = sx;
                m_lineEndSimY = sy;
            } else {
                m_lineDrawing = false;
                handleMouseAction(m_mouseScreenX, m_mouseScreenY, false, true, false, false);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        m_mouseScreenX = GET_X_LPARAM(lp);
        m_mouseScreenY = GET_Y_LPARAM(lp);
        if (m_lineDrawing && m_lineIsRight) {
            int sx = 0, sy = 0;
            if (screenToSimClamped(m_mouseScreenX, m_mouseScreenY, sx, sy)) {
                m_lineEndSimX = sx;
                m_lineEndSimY = sy;
            }
            m_world.applyToolLine(ToolMode::Erase, m_lineStartSimX, m_lineStartSimY,
                                  m_lineEndSimX, m_lineEndSimY, m_brushRadius, m_paintMat);
            m_lineDrawing = false;
        }
        m_rightDown = false;
        m_prevStrokeSimX = -1;
        m_prevStrokeSimY = -1;
        if (!m_leftDown) ReleaseCapture();
        return 0;
    }
    case WM_MBUTTONDOWN:
        m_mouseScreenX = GET_X_LPARAM(lp);
        m_mouseScreenY = GET_Y_LPARAM(lp);
        handleMouseAction(m_mouseScreenX, m_mouseScreenY, false, false, true, false);
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta > 0) m_brushRadius = std::min(35, m_brushRadius + 1);
        else if (delta < 0) m_brushRadius = std::max(0, m_brushRadius - 1);
        return 0;
    }
    case WM_KEYDOWN: {
        const bool isRepeat = (lp & (1LL << 30)) != 0;
        handleKeyDown(wp, isRepeat);
        rebuildLayout();
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Win32App::ensureBackbuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == m_clientW && height == m_clientH && m_backDC) return;

    HDC screenDC = GetDC(m_hwnd);
    if (!screenDC) return;

    if (m_backDC) {
        SelectObject(m_backDC, GetStockObject(DEFAULT_GUI_FONT));
        if (m_oldBmp) {
            SelectObject(m_backDC, m_oldBmp);
            m_oldBmp = nullptr;
        }
        if (m_backBmp) {
            DeleteObject(m_backBmp);
            m_backBmp = nullptr;
        }
        DeleteDC(m_backDC);
        m_backDC = nullptr;
    }

    m_backDC = CreateCompatibleDC(screenDC);
    m_backBmp = CreateCompatibleBitmap(screenDC, width, height);
    m_oldBmp = static_cast<HBITMAP>(SelectObject(m_backDC, m_backBmp));
    m_clientW = width;
    m_clientH = height;
    ReleaseDC(m_hwnd, screenDC);
}

void Win32App::rebuildLayout() {
    m_buttons.clear();
    if (m_clientW <= 0 || m_clientH <= 0) return;

    // --- Toolbar with responsive wrapping so buttons never clip at >= 800px (and 1024px) ---
    struct TopBtnSpec {
        int group;
        int id;
        int baseW;
        std::wstring label;
        bool active;
        COLORREF accent;
    };

    const std::vector<TopBtnSpec> actionSpecs = {
        {GrpAction, ActPlayPause, 66, m_paused ? L"Avvia" : L"Pausa", !m_paused, RGB(34, 197, 94)},
        {GrpAction, ActStep, 64, L"Passo +1", false, RGB(59, 130, 246)},
        {GrpAction, ActReset, 74, L"Reset (R)", false, RGB(14, 165, 233)},
        {GrpAction, ActClear, 60, L"Pulisci", false, RGB(239, 68, 68)},
        {GrpAction, ActSnapshot, 64, L"PPM (S)", false, RGB(16, 185, 129)},
        {GrpAction, ActGravity, 86, gravityNameIt(m_world.gravityMode()),
         m_world.gravityMode() != GravityMode::Earth, RGB(168, 85, 247)},
        {GrpAction, ActBoundary, 94,
         m_world.boundaryMode() == BoundaryMode::Closed ? L"Bordi: Chiusi" : L"Bordi: Aperti",
         m_world.boundaryMode() == BoundaryMode::Open, RGB(59, 130, 246)},
        {GrpAction, ActSpeed, 56, m_stepsPerFrame == 1 ? L"Vel: 1x" : L"Vel: 2x",
         m_stepsPerFrame > 1, RGB(245, 158, 11)},
    };

    const wchar_t* viewNames[5] = {L"1 Fisico", L"2 Termico", L"3 Pressione", L"4 Velocita", L"5 Luce"};
    const int viewWidths[5] = {68, 72, 82, 72, 62};
    std::vector<TopBtnSpec> viewSpecs;
    viewSpecs.reserve(5);
    for (int v = 0; v < 5; ++v) {
        viewSpecs.push_back({GrpView, v, viewWidths[v], viewNames[v],
                             static_cast<int>(m_viewMode) == v, RGB(59, 130, 246)});
    }

    const wchar_t* presetNames[6] = {
        L"Preset: Lab", L"Vulcano", L"Idrodinamica", L"Esplosivi", L"Elettrico", L"Orbita Zero-G"
    };
    const int presetWidths[6] = {88, 70, 94, 76, 72, 96};
    std::vector<TopBtnSpec> presetSpecs;
    presetSpecs.reserve(6);
    for (int p = 0; p < 6; ++p) {
        presetSpecs.push_back({GrpPreset, p, presetWidths[p], presetNames[p],
                               static_cast<int>(m_currentPreset) == p, RGB(245, 158, 11)});
    }

    const int marginX = uiPx(10);
    const int gapX = uiPx(4);
    const int groupGap = uiPx(8);
    const int btnH = uiPx(22);
    const int rowStep = uiPx(26);

    auto measureGroup = [&](const std::vector<TopBtnSpec>& specs) {
        int total = 0;
        for (size_t i = 0; i < specs.size(); ++i) {
            total += uiPx(specs[i].baseW);
            if (i + 1 < specs.size()) total += gapX;
        }
        return total;
    };

    int tx = marginX;
    int ty = uiPx(6);
    auto placeGroup = [&](const std::vector<TopBtnSpec>& specs) {
        for (const auto& s : specs) {
            const int w = uiPx(s.baseW);
            if (tx > marginX && tx + w > m_clientW - marginX) {
                tx = marginX;
                ty += rowStep;
            }
            UiButton b{};
            b.rect = {tx, ty, tx + w, ty + btnH};
            b.label = s.label;
            b.group = s.group;
            b.id = s.id;
            b.active = s.active;
            b.accent = s.accent;
            m_buttons.push_back(std::move(b));
            tx += w + gapX;
        }
    };

    placeGroup(actionSpecs);
    const int viewsTotalW = measureGroup(viewSpecs);
    if (tx + groupGap + viewsTotalW <= m_clientW - marginX) {
        tx += groupGap;
        placeGroup(viewSpecs);
        tx = marginX;
        ty += rowStep;
        placeGroup(presetSpecs);
    } else {
        tx = marginX;
        ty += rowStep;
        placeGroup(presetSpecs);
        if (tx + groupGap + viewsTotalW <= m_clientW - marginX) {
            tx += groupGap;
        } else {
            tx = marginX;
            ty += rowStep;
        }
        placeGroup(viewSpecs);
    }

    m_toolbarHeightPx = ty + btnH + uiPx(6);
    const int tbH = m_toolbarHeightPx;
    const int sbW = uiPx(kSidebarWidth);
    const int stH = uiPx(kStatusHeight);
    const int pad = uiPx(8);

    m_toolbarRect = {0, 0, m_clientW, tbH};
    m_sidebarRect = {0, tbH, sbW, std::max(tbH, m_clientH - stH)};
    m_statusRect = {0, std::max(tbH, m_clientH - stH), m_clientW, m_clientH};
    m_canvasBounds = {sbW + pad, tbH + pad, std::max(sbW + pad + 1, m_clientW - pad),
                      std::max(tbH + pad + 1, static_cast<int>(m_statusRect.top) - pad)};

    // Fit simulation aspect ratio inside m_canvasBounds
    const int availW = std::max(64, static_cast<int>(m_canvasBounds.right - m_canvasBounds.left));
    const int availH = std::max(64, static_cast<int>(m_canvasBounds.bottom - m_canvasBounds.top));
    const double scaleX = static_cast<double>(availW) / static_cast<double>(m_world.width());
    const double scaleY = static_cast<double>(availH) / static_cast<double>(m_world.height());
    const double scale = std::min(scaleX, scaleY);
    const int drawW = std::max(1, static_cast<int>(std::floor(m_world.width() * scale)));
    const int drawH = std::max(1, static_cast<int>(std::floor(m_world.height() * scale)));
    const int ox = m_canvasBounds.left + (availW - drawW) / 2;
    const int oy = m_canvasBounds.top + (availH - drawH) / 2;
    m_canvasDrawRect = {ox, oy, ox + drawW, oy + drawH};

    // --- Sidebar: Brush Size, Tools, Materials, Summary & Hints ---
    const int sbPadX = uiPx(10);
    m_brushHeaderRect = {sbPadX, tbH + uiPx(6), sbW - sbPadX, tbH + uiPx(22)};
    const int brushRowY = tbH + uiPx(24);
    m_buttons.push_back({{sbPadX, brushRowY, sbPadX + uiPx(36), brushRowY + uiPx(22)},
                         L"-", GrpBrush, 0, RGB(59, 130, 246), false, 0, false});
    m_buttons.push_back({{sbW - sbPadX - uiPx(36), brushRowY, sbW - sbPadX, brushRowY + uiPx(22)},
                         L"+", GrpBrush, 1, RGB(59, 130, 246), false, 0, false});
    m_brushValueRect = {sbPadX + uiPx(40), brushRowY, sbW - sbPadX - uiPx(40), brushRowY + uiPx(22)};

    m_toolsHeaderRect = {sbPadX, tbH + uiPx(52), sbW - sbPadX, tbH + uiPx(68)};
    int sy = tbH + uiPx(70);

    struct ToolEntry { ToolMode mode; const wchar_t* label; COLORREF color; };
    static constexpr ToolEntry tools[] = {
        {ToolMode::Material,  L"Pittura",   RGB(59, 130, 246)},
        {ToolMode::Wind,      L"Vento",     RGB(56, 189, 248)},
        {ToolMode::Heat,      L"Calore",    RGB(249, 115, 22)},
        {ToolMode::Cryo,      L"Freddo",    RGB(125, 211, 252)},
        {ToolMode::SparkTool, L"Scintilla", RGB(250, 204, 21)},
        {ToolMode::Attractor, L"Attrae",    RGB(168, 85, 247)},
        {ToolMode::Repulsor,  L"Respinge",  RGB(244, 63, 94)},
        {ToolMode::Erase,     L"Gomma",     RGB(148, 163, 184)},
    };
    const int colStride = uiPx(110);
    const int sbBtnW = uiPx(104);
    const int toolRowStep = uiPx(24);
    for (int idx = 0; idx < 8; ++idx) {
        const int col = idx % 2;
        const int row = idx / 2;
        const int bx = sbPadX + col * colStride;
        const int by = sy + row * toolRowStep;
        if (by + uiPx(21) > m_statusRect.top - uiPx(4)) break;
        UiButton b{};
        b.rect = {bx, by, bx + sbBtnW, by + uiPx(21)};
        b.label = tools[idx].label;
        b.group = GrpTool;
        b.id = static_cast<int>(tools[idx].mode);
        b.active = (m_toolMode == tools[idx].mode);
        b.accent = tools[idx].color;
        b.hasSwatch = true;
        b.swatchColor = tools[idx].color;
        m_buttons.push_back(std::move(b));
    }

    sy += 4 * toolRowStep + uiPx(4);
    m_materialsHeaderRect = {sbPadX, sy, sbW - sbPadX, sy + uiPx(16)};
    sy += uiPx(18);

    static constexpr MaterialId matOrder[] = {
        MaterialId::Sand, MaterialId::Water,
        MaterialId::Stone, MaterialId::Wood,
        MaterialId::Metal, MaterialId::Glass,
        MaterialId::Ice, MaterialId::Snow,
        MaterialId::Salt, MaterialId::Plant,
        MaterialId::Oil, MaterialId::Acid,
        MaterialId::Lava, MaterialId::Obsidian,
        MaterialId::Gunpowder, MaterialId::Nitro,
        MaterialId::Fire, MaterialId::Smoke,
        MaterialId::Steam, MaterialId::Methane,
        MaterialId::Spark, MaterialId::Plasma,
        MaterialId::Battery, MaterialId::Bedrock
    };
    const int matCount = static_cast<int>(sizeof(matOrder) / sizeof(matOrder[0]));
    const int matRowStep = uiPx(22);
    int matBottom = sy;
    for (int idx = 0; idx < matCount; ++idx) {
        const int col = idx % 2;
        const int row = idx / 2;
        const int bx = sbPadX + col * colStride;
        const int by = sy + row * matRowStep;
        if (by + uiPx(20) > m_statusRect.top - uiPx(4)) break;
        const auto& props = getMaterial(matOrder[idx]);
        UiButton b{};
        b.rect = {bx, by, bx + sbBtnW, by + uiPx(19)};
        b.label = utf8ToWide(props.shortLabel);
        b.group = GrpMaterial;
        b.id = static_cast<int>(matOrder[idx]);
        b.active = (m_toolMode == ToolMode::Material && m_paintMat == matOrder[idx]);
        b.accent = RGB(59, 130, 246);
        b.hasSwatch = true;
        b.swatchColor = RGB(props.baseR, props.baseG, props.baseB);
        m_buttons.push_back(std::move(b));
        matBottom = std::max(matBottom, static_cast<int>(b.rect.bottom));
    }

    const int infoTop = matBottom + uiPx(6);
    m_sidebarInfoRect = {sbPadX, infoTop, sbW - sbPadX, infoTop + uiPx(62)};
    m_showSidebarInfo = (m_sidebarInfoRect.bottom <= m_statusRect.top - uiPx(6));

    const int hintsTop = m_sidebarInfoRect.bottom + uiPx(6);
    m_sidebarHintsRect = {sbPadX, hintsTop, sbW - sbPadX, hintsTop + uiPx(88)};
    m_showSidebarHints = m_showSidebarInfo && (m_sidebarHintsRect.bottom <= m_statusRect.top - uiPx(6));
}

void Win32App::drawButton(HDC dc, const UiButton& btn, bool hovered) const {
    const COLORREF fill = btn.active ? RGB(36, 52, 82) : (hovered ? RGB(42, 49, 68) : RGB(28, 33, 47));
    const COLORREF border = btn.active ? btn.accent : (hovered ? RGB(90, 102, 130) : RGB(46, 54, 74));
    drawRoundedBox(dc, btn.rect, fill, border, uiPx(5));

    RECT tr = btn.rect;
    if (btn.hasSwatch) {
        RECT sw{btn.rect.left + uiPx(6), btn.rect.top + uiPx(4),
                btn.rect.left + uiPx(16), btn.rect.bottom - uiPx(4)};
        drawRoundedBox(dc, sw, btn.swatchColor, RGB(20, 24, 34), uiPx(2));
        tr.left += uiPx(20);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, btn.active ? RGB(248, 250, 252) : RGB(205, 214, 228));
    HGDIOBJ oldFont = SelectObject(dc, btn.active ? m_fontBold : m_fontUI);
    DrawTextW(dc, btn.label.c_str(), -1, &tr,
              (btn.hasSwatch ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void Win32App::renderFrame() {
    if (!m_backDC || m_clientW <= 0 || m_clientH <= 0) return;

    RECT full{0, 0, m_clientW, m_clientH};
    fillSolidRect(m_backDC, full, RGB(14, 17, 25));

    drawToolbar(m_backDC);
    drawSidebar(m_backDC);
    drawCanvas(m_backDC);
    drawStatusBar(m_backDC);

    const POINT pt{m_mouseScreenX, m_mouseScreenY};
    for (const auto& btn : m_buttons) {
        drawButton(m_backDC, btn, PtInRect(&btn.rect, pt) != FALSE);
    }
}

void Win32App::drawToolbar(HDC dc) {
    fillSolidRect(dc, m_toolbarRect, RGB(22, 26, 38));

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(42, 49, 68));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, 0, m_toolbarRect.bottom - 1, nullptr);
    LineTo(dc, m_clientW, m_toolbarRect.bottom - 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

std::wstring Win32App::activeToolSummary() const {
    if (m_toolMode == ToolMode::Material) {
        const auto& props = getMaterial(m_paintMat);
        return utf8ToWide(props.nameIt) + L" [" + phaseNameIt(props.phase) + L"]";
    }
    return std::wstring(L"Strumento: ") + toolNameIt(m_toolMode);
}

std::wstring Win32App::activeToolDetails() const {
    wchar_t buf[128]{};
    if (m_toolMode == ToolMode::Material) {
        const auto& props = getMaterial(m_paintMat);
        if (props.electricalConductivity > 0.2f) {
            std::swprintf(buf, 128, L"Dens %.2f | Cond.El %.0f%% | r=%d px",
                          props.density, props.electricalConductivity * 100.0f, m_brushRadius);
        } else if (props.ignitionTempK > 0.0f) {
            std::swprintf(buf, 128, L"Dens %.2f | Igniz %.0f C | r=%d px",
                          props.density, props.ignitionTempK - kKelvinZeroC, m_brushRadius);
        } else if (props.meltTempK > 0.0f) {
            std::swprintf(buf, 128, L"Dens %.2f | Fonde %.0f C | r=%d px",
                          props.density, props.meltTempK - kKelvinZeroC, m_brushRadius);
        } else if (props.boilTempK > 0.0f) {
            std::swprintf(buf, 128, L"Dens %.2f | Bolle %.0f C | r=%d px",
                          props.density, props.boilTempK - kKelvinZeroC, m_brushRadius);
        } else {
            std::swprintf(buf, 128, L"Dens %.2f | Cond.T %.2f | r=%d px",
                          props.density, props.thermalConductivity, m_brushRadius);
        }
        return buf;
    }
    std::swprintf(buf, 128, L"%ls | r=%d px", toolHintIt(m_toolMode), std::max(1, m_brushRadius));
    return buf;
}

void Win32App::drawSidebar(HDC dc) {
    fillSolidRect(dc, m_sidebarRect, RGB(20, 24, 35));

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(42, 49, 68));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, m_sidebarRect.right - 1, m_sidebarRect.top, nullptr);
    LineTo(dc, m_sidebarRect.right - 1, m_sidebarRect.bottom);
    SelectObject(dc, old);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, m_fontBold);

    SetTextColor(dc, RGB(148, 163, 184));
    DrawTextW(dc, L"PENNELLO (Rotella / [ ])", -1, &m_brushHeaderRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t brushTxt[32]{};
    std::swprintf(brushTxt, 32, L"%d px", m_brushRadius);
    SetTextColor(dc, RGB(241, 245, 249));
    DrawTextW(dc, brushTxt, -1, &m_brushValueRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, RGB(148, 163, 184));
    DrawTextW(dc, L"STRUMENTI FISICI", -1, &m_toolsHeaderRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(dc, L"MATERIALI (Dx = Gomma)", -1, &m_materialsHeaderRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (m_showSidebarInfo) {
        drawRoundedBox(dc, m_sidebarInfoRect, RGB(25, 31, 46), RGB(56, 70, 98), uiPx(6));
        const COLORREF swCol = (m_toolMode == ToolMode::Material)
                                   ? RGB(getMaterial(m_paintMat).baseR,
                                         getMaterial(m_paintMat).baseG,
                                         getMaterial(m_paintMat).baseB)
                                   : toolAccentColor(m_toolMode);
        const RECT swRect{m_sidebarInfoRect.left + uiPx(8), m_sidebarInfoRect.top + uiPx(7),
                          m_sidebarInfoRect.left + uiPx(19), m_sidebarInfoRect.top + uiPx(18)};
        drawRoundedBox(dc, swRect, swCol, RGB(16, 20, 30), uiPx(2));

        RECT titleR{m_sidebarInfoRect.left + uiPx(24), m_sidebarInfoRect.top + uiPx(4),
                    m_sidebarInfoRect.right - uiPx(8), m_sidebarInfoRect.top + uiPx(21)};
        SetTextColor(dc, RGB(241, 245, 249));
        SelectObject(dc, m_fontBold);
        const std::wstring summary = activeToolSummary();
        DrawTextW(dc, summary.c_str(), -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT detR{m_sidebarInfoRect.left + uiPx(8), m_sidebarInfoRect.top + uiPx(23),
                  m_sidebarInfoRect.right - uiPx(8), m_sidebarInfoRect.top + uiPx(39)};
        SetTextColor(dc, RGB(186, 198, 214));
        SelectObject(dc, m_fontSmall);
        const std::wstring details = activeToolDetails();
        DrawTextW(dc, details.c_str(), -1, &detR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT modeR{m_sidebarInfoRect.left + uiPx(8), m_sidebarInfoRect.top + uiPx(40),
                   m_sidebarInfoRect.right - uiPx(8), m_sidebarInfoRect.bottom - uiPx(4)};
        SetTextColor(dc, RGB(125, 211, 252));
        DrawTextW(dc, L"Tratto: Click Sx | Linea: Shift+Drag", -1, &modeR,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (m_showSidebarHints) {
        drawRoundedBox(dc, m_sidebarHintsRect, RGB(22, 27, 40), RGB(42, 49, 68), uiPx(6));

        RECT hdr{m_sidebarHintsRect.left + uiPx(8), m_sidebarHintsRect.top + uiPx(4),
                 m_sidebarHintsRect.right - uiPx(8), m_sidebarHintsRect.top + uiPx(20)};
        SetTextColor(dc, RGB(148, 163, 184));
        SelectObject(dc, m_fontBold);
        DrawTextW(dc, L"SCORCIATOIE RAPIDE", -1, &hdr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        static constexpr const wchar_t* kHints[4] = {
            L"Shift+Drag: Linea retta (Sx / Dx)",
            L"Click Dx: Gomma | Alt/Mid: Pipetta",
            L"Spazio: Pausa | N: Passo | R: Reset",
            L"S: PPM | C: Pulisci | G/B | 1-5"
        };
        SelectObject(dc, m_fontSmall);
        SetTextColor(dc, RGB(186, 198, 214));
        const int lineStep = uiPx(16);
        int hy = m_sidebarHintsRect.top + uiPx(21);
        for (int i = 0; i < 4; ++i) {
            RECT lr{m_sidebarHintsRect.left + uiPx(8), hy, m_sidebarHintsRect.right - uiPx(8), hy + lineStep};
            DrawTextW(dc, kHints[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            hy += lineStep;
        }
    }

    SelectObject(dc, oldFont);
}

void Win32App::drawLinePreview(HDC dc) const {
    if (!m_lineDrawing || m_lineStartSimX < 0 || m_lineEndSimX < 0) return;

    const int cw = m_canvasDrawRect.right - m_canvasDrawRect.left;
    const int ch = m_canvasDrawRect.bottom - m_canvasDrawRect.top;
    if (cw <= 0 || ch <= 0) return;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    simToScreenCenter(m_lineStartSimX, m_lineStartSimY, x0, y0);
    simToScreenCenter(m_lineEndSimX, m_lineEndSimY, x1, y1);

    const double scale = static_cast<double>(cw) / static_cast<double>(m_world.width());
    const ToolMode effTool = m_lineIsRight ? ToolMode::Erase : m_toolMode;
    const int effRadius = (effTool == ToolMode::Material || effTool == ToolMode::Erase)
                              ? m_brushRadius
                              : std::max(1, m_brushRadius);
    const int rPx = std::max(2, static_cast<int>(std::round((effRadius + 0.5) * scale)));

    COLORREF accent = RGB(56, 189, 248);
    if (m_lineIsRight || m_toolMode == ToolMode::Erase) {
        accent = RGB(248, 113, 113);
    } else if (m_toolMode == ToolMode::Material) {
        const auto& props = getMaterial(m_paintMat);
        const int lum = (299 * props.baseR + 587 * props.baseG + 114 * props.baseB) / 1000;
        if (lum >= 105) {
            accent = RGB(std::min(255, static_cast<int>(props.baseR) + 35),
                         std::min(255, static_cast<int>(props.baseG) + 35),
                         std::min(255, static_cast<int>(props.baseB) + 35));
        }
    } else {
        accent = toolAccentColor(m_toolMode);
    }

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, m_canvasDrawRect.left, m_canvasDrawRect.top,
                      m_canvasDrawRect.right, m_canvasDrawRect.bottom);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));

    // Outer thickness corridor guides for thick brushes
    const double dx = static_cast<double>(x1 - x0);
    const double dy = static_cast<double>(y1 - y0);
    const double len = std::hypot(dx, dy);
    if (rPx >= 4 && len >= 2.0) {
        const double nx = -dy / len;
        const double ny = dx / len;
        const int ox = static_cast<int>(std::round(nx * static_cast<double>(rPx)));
        const int oy = static_cast<int>(std::round(ny * static_cast<double>(rPx)));
        HPEN guidePen = CreatePen(PS_DOT, 1, RGB(148, 163, 184));
        HGDIOBJ oldPen = SelectObject(dc, guidePen);
        MoveToEx(dc, x0 + ox, y0 + oy, nullptr);
        LineTo(dc, x1 + ox, y1 + oy);
        MoveToEx(dc, x0 - ox, y0 - oy, nullptr);
        LineTo(dc, x1 - ox, y1 - oy);
        SelectObject(dc, oldPen);
        DeleteObject(guidePen);
    }

    // Center bold dashed accent line
    LOGBRUSH lb{};
    lb.lbStyle = BS_SOLID;
    lb.lbColor = accent;
    HPEN dashPen = ExtCreatePen(PS_GEOMETRIC | PS_DASH | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                static_cast<DWORD>(std::max(2, uiPx(2))),
                                &lb, 0, nullptr);
    if (!dashPen) {
        dashPen = CreatePen(PS_DASH, 1, accent);
    }
    HGDIOBJ oldDashPen = SelectObject(dc, dashPen);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, oldDashPen);
    DeleteObject(dashPen);

    // Endpoint rings & start anchor crosshair
    HPEN endPen = CreatePen(PS_SOLID, 1, accent);
    HGDIOBJ oldEndPen = SelectObject(dc, endPen);
    Ellipse(dc, x0 - rPx, y0 - rPx, x0 + rPx, y0 + rPx);
    Ellipse(dc, x1 - rPx, y1 - rPx, x1 + rPx, y1 + rPx);
    const int cross = uiPx(4);
    MoveToEx(dc, x0 - cross, y0, nullptr);
    LineTo(dc, x0 + cross + 1, y0);
    MoveToEx(dc, x0, y0 - cross, nullptr);
    LineTo(dc, x0, y0 + cross + 1);
    SelectObject(dc, oldEndPen);
    DeleteObject(endPen);

    SelectObject(dc, oldBr);
    RestoreDC(dc, saved);
}

void Win32App::drawCanvas(HDC dc) {
    RECT borderRect{m_canvasDrawRect.left - 1, m_canvasDrawRect.top - 1,
                    m_canvasDrawRect.right + 1, m_canvasDrawRect.bottom + 1};
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(55, 65, 90));
    HGDIOBJ oldPen = SelectObject(dc, borderPen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, borderRect.left, borderRect.top, borderRect.right, borderRect.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBr);
    DeleteObject(borderPen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_world.width();
    bmi.bmiHeader.biHeight = -m_world.height(); // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc,
                  m_canvasDrawRect.left,
                  m_canvasDrawRect.top,
                  m_canvasDrawRect.right - m_canvasDrawRect.left,
                  m_canvasDrawRect.bottom - m_canvasDrawRect.top,
                  0, 0, m_world.width(), m_world.height(),
                  m_world.framebuffer(),
                  &bmi,
                  DIB_RGB_COLORS,
                  SRCCOPY);

    if (m_lineDrawing) {
        drawLinePreview(dc);
    } else if (m_hoverSimX >= 0 && m_hoverSimY >= 0) {
        // Draw live brush ring preview clipped strictly to the canvas bounds
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, m_canvasDrawRect.left, m_canvasDrawRect.top,
                          m_canvasDrawRect.right, m_canvasDrawRect.bottom);
        const double scale = static_cast<double>(m_canvasDrawRect.right - m_canvasDrawRect.left) /
                             static_cast<double>(m_world.width());
        int cx = 0, cy = 0;
        simToScreenCenter(m_hoverSimX, m_hoverSimY, cx, cy);
        const int effRadius = (m_toolMode == ToolMode::Material || m_toolMode == ToolMode::Erase)
                                  ? m_brushRadius
                                  : std::max(1, m_brushRadius);
        const int rPx = std::max(2, static_cast<int>(std::round((effRadius + 0.5) * scale)));

        HPEN ringPen = CreatePen(PS_SOLID, 1, RGB(248, 250, 252));
        HGDIOBJ op = SelectObject(dc, ringPen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - rPx, cy - rPx, cx + rPx, cy + rPx);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(ringPen);
        RestoreDC(dc, saved);
    }
}

void Win32App::drawStatusBar(HDC dc) {
    fillSolidRect(dc, m_statusRect, RGB(20, 24, 35));

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(42, 49, 68));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, 0, m_statusRect.top, nullptr);
    LineTo(dc, m_clientW, m_statusRect.top);
    SelectObject(dc, old);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, m_fontMono);

    const bool bannerActive = !m_statusBanner.empty() &&
                              std::chrono::steady_clock::now() < m_statusBannerUntil;
    if (!bannerActive && !m_statusBanner.empty()) {
        m_statusBanner.clear();
    }

    RECT r1{uiPx(12), m_statusRect.top + uiPx(4), m_clientW - uiPx(12), m_statusRect.top + uiPx(24)};
    if (bannerActive) {
        SelectObject(dc, m_fontBold);
        SIZE sz{};
        GetTextExtentPoint32W(dc, m_statusBanner.c_str(), static_cast<int>(m_statusBanner.size()), &sz);
        const int maxBannerW = std::max(uiPx(140), (m_clientW * 45) / 100);
        const int pillW = std::min(static_cast<int>(sz.cx) + uiPx(18), maxBannerW);
        RECT pill{m_clientW - uiPx(12) - pillW, m_statusRect.top + uiPx(3),
                  m_clientW - uiPx(12), m_statusRect.top + uiPx(24)};
        drawRoundedBox(dc, pill, m_statusBannerFill, m_statusBannerBorder, uiPx(4));

        RECT pillText{pill.left + uiPx(6), pill.top, pill.right - uiPx(6), pill.bottom};
        SetTextColor(dc, m_statusBannerText);
        DrawTextW(dc, m_statusBanner.c_str(), -1, &pillText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        r1.right = pill.left - uiPx(10);
        SelectObject(dc, m_fontMono);
    }

    wchar_t line1[320]{};
    if (m_lineDrawing) {
        const double dx = static_cast<double>(m_lineEndSimX - m_lineStartSimX);
        const double dy = static_cast<double>(m_lineEndSimY - m_lineStartSimY);
        const double dist = std::hypot(dx, dy);
        const double deg = std::atan2(-dy, dx) * 57.29577951308232;
        const std::wstring toolStr = m_lineIsRight ? L"Gomma" : activeToolSummary();
        std::swprintf(line1, 320,
                      L"Linea (%d,%d)->(%d,%d) | Lun: %.1f px | Ang: %.1f deg | %ls (r=%d) | Rilascia=Applica, Esc=Annulla",
                      m_lineStartSimX, m_lineStartSimY, m_lineEndSimX, m_lineEndSimY,
                      dist, deg, toolStr.c_str(), m_brushRadius);
        SetTextColor(dc, RGB(125, 211, 252));
    } else if (m_hoverSimX >= 0 && m_hoverSimY >= 0) {
        const PixelInfo px = m_world.inspect(m_hoverSimX, m_hoverSimY);
        const auto& props = getMaterial(px.material);
        const std::wstring matW = utf8ToWide(props.nameIt);
        const float tempC = px.tempK - kKelvinZeroC;
        std::swprintf(line1, 320,
                      L"Pixel (%d,%d) | %ls [%ls] | T: %.1f C (%.0f K) | P: %+.2f | Vp: (%.2f, %.2f) | Va: (%.2f, %.2f) | Q: %.2fV",
                      px.x, px.y, matW.c_str(), phaseNameIt(props.phase),
                      tempC, px.tempK, px.pressure, px.vx, px.vy, px.airVx, px.airVy, px.charge);
        SetTextColor(dc, RGB(125, 211, 252));
    } else {
        const std::wstring summary = activeToolSummary();
        std::swprintf(line1, 320,
                      L"Attivo: %ls (r=%d px) | Shift+Drag: Linea | Dx: Gomma | Alt/Mid: Pipetta | S: PPM | R: Reset",
                      summary.c_str(), m_brushRadius);
        SetTextColor(dc, RGB(125, 211, 252));
    }

    if (r1.right > r1.left + uiPx(40)) {
        DrawTextW(dc, line1, -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const auto& st = m_world.stats();
    const std::wstring activeStr = activeToolSummary();
    wchar_t line2[320]{};
    std::swprintf(line2, 320,
                  L"FPS: %.1f | Step: %.2f ms (Flu %.2f | Ter %.2f | Reaz %.2f | Part %.2f | Luce %.2f) | Attivi: %u/%d | Tick: %llu | [%ls | r=%d]",
                  m_fps, st.totalStepMs, st.fluidMs, st.thermoMs, st.reactionMs, st.particleMs, st.lightMs,
                  st.activeParticles, m_world.pixelCount(),
                  static_cast<unsigned long long>(st.tick),
                  activeStr.c_str(), m_brushRadius);

    SetTextColor(dc, RGB(148, 163, 184));
    RECT r2{uiPx(12), m_statusRect.top + uiPx(25), m_clientW - uiPx(12), m_statusRect.bottom - uiPx(3)};
    DrawTextW(dc, line2, -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, oldFont);
}

bool Win32App::screenToSim(int sx, int sy, int& outSimX, int& outSimY) const noexcept {
    const int cw = m_canvasDrawRect.right - m_canvasDrawRect.left;
    const int ch = m_canvasDrawRect.bottom - m_canvasDrawRect.top;
    if (cw <= 0 || ch <= 0) return false;
    if (sx < m_canvasDrawRect.left || sx >= m_canvasDrawRect.right ||
        sy < m_canvasDrawRect.top || sy >= m_canvasDrawRect.bottom) {
        return false;
    }
    outSimX = ((sx - m_canvasDrawRect.left) * m_world.width()) / cw;
    outSimY = ((sy - m_canvasDrawRect.top) * m_world.height()) / ch;
    outSimX = std::clamp(outSimX, 0, m_world.width() - 1);
    outSimY = std::clamp(outSimY, 0, m_world.height() - 1);
    return true;
}

bool Win32App::screenToSimClamped(int sx, int sy, int& outSimX, int& outSimY) const noexcept {
    const int cw = m_canvasDrawRect.right - m_canvasDrawRect.left;
    const int ch = m_canvasDrawRect.bottom - m_canvasDrawRect.top;
    if (cw <= 0 || ch <= 0) return false;
    const int cx = std::clamp(sx, static_cast<int>(m_canvasDrawRect.left),
                              static_cast<int>(m_canvasDrawRect.right - 1));
    const int cy = std::clamp(sy, static_cast<int>(m_canvasDrawRect.top),
                              static_cast<int>(m_canvasDrawRect.bottom - 1));
    outSimX = std::clamp(((cx - static_cast<int>(m_canvasDrawRect.left)) * m_world.width()) / cw,
                         0, m_world.width() - 1);
    outSimY = std::clamp(((cy - static_cast<int>(m_canvasDrawRect.top)) * m_world.height()) / ch,
                         0, m_world.height() - 1);
    return true;
}

void Win32App::simToScreenCenter(int simX, int simY, int& outScreenX, int& outScreenY) const noexcept {
    const int cw = std::max(1, static_cast<int>(m_canvasDrawRect.right - m_canvasDrawRect.left));
    const int ch = std::max(1, static_cast<int>(m_canvasDrawRect.bottom - m_canvasDrawRect.top));
    const double scaleX = static_cast<double>(cw) / static_cast<double>(m_world.width());
    const double scaleY = static_cast<double>(ch) / static_cast<double>(m_world.height());
    outScreenX = m_canvasDrawRect.left + static_cast<int>((simX + 0.5) * scaleX);
    outScreenY = m_canvasDrawRect.top + static_cast<int>((simY + 0.5) * scaleY);
}

void Win32App::handleMouseAction(int sx, int sy, bool isLeft, bool isRight, bool isMiddle, bool isDrag) {
    int simX = 0, simY = 0;
    if (!screenToSim(sx, sy, simX, simY)) return;

    if (isMiddle) {
        const MaterialId sampled = m_world.inspect(simX, simY).material;
        if (sampled != MaterialId::Air) {
            m_paintMat = sampled;
            m_toolMode = ToolMode::Material;
            rebuildLayout();
        }
        return;
    }

    const ToolMode effectiveTool = isRight ? ToolMode::Erase : m_toolMode;
    if (isDrag && m_prevStrokeSimX >= 0 && m_prevStrokeSimY >= 0) {
        m_world.applyToolLine(effectiveTool, m_prevStrokeSimX, m_prevStrokeSimY, simX, simY,
                              m_brushRadius, m_paintMat);
    } else if (isLeft || isRight) {
        m_world.applyToolCircle(effectiveTool, simX, simY, m_brushRadius, 0.0f, 0.0f, m_paintMat);
    }
    m_prevStrokeSimX = simX;
    m_prevStrokeSimY = simY;
}

void Win32App::showStatusBanner(const std::wstring& msg,
                                COLORREF fill,
                                COLORREF border,
                                COLORREF text,
                                int durationMs) {
    m_statusBanner = msg;
    m_statusBannerFill = fill;
    m_statusBannerBorder = border;
    m_statusBannerText = text;
    m_statusBannerUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
}

void Win32App::exportSnapshot() {
    char filename[128]{};
    const unsigned long long tick = static_cast<unsigned long long>(m_world.stats().tick);
    std::snprintf(filename, sizeof(filename), "snapshot_tick_%llu.ppm", tick);
    const std::wstring wName = utf8ToWide(filename);

    if (m_world.savePPM(filename, m_viewMode)) {
        showStatusBanner(L"Snapshot salvato: " + wName,
                         RGB(16, 48, 36), RGB(52, 211, 153), RGB(167, 243, 208), 4000);
    } else {
        showStatusBanner(L"Errore snapshot: " + wName,
                         RGB(64, 22, 26), RGB(248, 113, 113), RGB(254, 202, 202), 4000);
    }
}

void Win32App::handleButtonClick(const UiButton& btn) {
    switch (btn.group) {
    case GrpAction:
        switch (btn.id) {
        case ActPlayPause: m_paused = !m_paused; break;
        case ActStep: m_paused = true; m_world.step(); break;
        case ActReset:
            m_world.loadPreset(m_currentPreset);
            showStatusBanner(L"Preset ripristinato", RGB(20, 44, 68), RGB(56, 189, 248), RGB(186, 230, 253), 2500);
            break;
        case ActClear: m_world.clear(MaterialId::Air); break;
        case ActSnapshot: exportSnapshot(); break;
        case ActGravity: {
            const int nextG = (static_cast<int>(m_world.gravityMode()) + 1) % 4;
            m_world.setGravityMode(static_cast<GravityMode>(nextG));
            break;
        }
        case ActBoundary:
            m_world.setBoundaryMode(m_world.boundaryMode() == BoundaryMode::Closed
                                        ? BoundaryMode::Open
                                        : BoundaryMode::Closed);
            break;
        case ActSpeed:
            m_stepsPerFrame = (m_stepsPerFrame == 1) ? 2 : 1;
            break;
        }
        break;
    case GrpView:
        m_viewMode = static_cast<ViewMode>(btn.id);
        break;
    case GrpPreset:
        m_currentPreset = static_cast<PresetId>(btn.id);
        m_world.loadPreset(m_currentPreset);
        break;
    case GrpBrush:
        if (btn.id == 0) m_brushRadius = std::max(0, m_brushRadius - 1);
        else m_brushRadius = std::min(35, m_brushRadius + 1);
        break;
    case GrpTool:
        m_toolMode = static_cast<ToolMode>(btn.id);
        break;
    case GrpMaterial:
        m_paintMat = static_cast<MaterialId>(btn.id);
        m_toolMode = ToolMode::Material;
        break;
    }
}

void Win32App::handleKeyDown(WPARAM vk, bool isRepeat) {
    if (isRepeat && vk != VK_OEM_4 && vk != VK_OEM_6 && vk != 'N') {
        return;
    }

    switch (vk) {
    case VK_SPACE: m_paused = !m_paused; break;
    case 'N': m_paused = true; m_world.step(); break;
    case 'C': m_world.clear(MaterialId::Air); break;
    case 'R':
        m_world.loadPreset(m_currentPreset);
        showStatusBanner(L"Preset ripristinato", RGB(20, 44, 68), RGB(56, 189, 248), RGB(186, 230, 253), 2500);
        break;
    case 'S': exportSnapshot(); break;
    case 'G': {
        const int nextG = (static_cast<int>(m_world.gravityMode()) + 1) % 4;
        m_world.setGravityMode(static_cast<GravityMode>(nextG));
        break;
    }
    case 'B':
        m_world.setBoundaryMode(m_world.boundaryMode() == BoundaryMode::Closed
                                    ? BoundaryMode::Open
                                    : BoundaryMode::Closed);
        break;
    case '1': m_viewMode = ViewMode::Physical; break;
    case '2': m_viewMode = ViewMode::Thermal; break;
    case '3': m_viewMode = ViewMode::Pressure; break;
    case '4': m_viewMode = ViewMode::Velocity; break;
    case '5': m_viewMode = ViewMode::EnergyLight; break;
    case VK_OEM_4: m_brushRadius = std::max(0, m_brushRadius - 1); break; // '['
    case VK_OEM_6: m_brushRadius = std::min(35, m_brushRadius + 1); break; // ']'
    case VK_ESCAPE:
        if (m_lineDrawing) {
            m_lineDrawing = false;
            m_leftDown = false;
            m_rightDown = false;
            m_prevStrokeSimX = -1;
            m_prevStrokeSimY = -1;
            ReleaseCapture();
        } else {
            PostQuitMessage(0);
        }
        break;
    }
}

} // namespace sim::app
