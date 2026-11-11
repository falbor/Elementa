#include "SdlApp.hpp"

#include "AppState.hpp"
#include "GlShaderPipeline.hpp"
#include "GuiOverlay.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace sim::app {

struct SdlApp::Impl {
    Impl(SimWorld &simWorld, ViewMode initialView, PresetId initialPreset) : world(simWorld) {
        state.viewMode = initialView;
        state.currentPreset = initialPreset;
    }

    ~Impl() { shutdown(); }

    void shutdown() {
        cancelActiveInteraction();

        if (window && glContext) {
            SDL_GL_MakeCurrent(window, glContext);
            glPipeline.shutdown();
            if (simTexture != 0) {
                glDeleteTextures(1, &simTexture);
                simTexture = 0;
            }
        } else {
            glPipeline.shutdown();
        }
        state.glShadersAvailable = false;
        texWidth = 0;
        texHeight = 0;

        if (imguiInitialized) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            imguiInitialized = false;
        }

        if (crosshairCursor) {
            SDL_DestroyCursor(crosshairCursor);
            crosshairCursor = nullptr;
        }
        if (arrowCursor) {
            SDL_DestroyCursor(arrowCursor);
            arrowCursor = nullptr;
        }
        usingCrosshairCursor = false;

        if (glContext) {
            SDL_GL_DestroyContext(glContext);
            glContext = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        if (sdlInitialized) {
            SDL_Quit();
            sdlInitialized = false;
        }
    }

    bool updateScaleMetrics() {
        if (!window) return false;
        const float rawDensity = SDL_GetWindowPixelDensity(window);
        const float newDensity = (rawDensity > 0.0f) ? rawDensity : 1.0f;
        const float rawDisplayScale = SDL_GetWindowDisplayScale(window);
        const float displayScale = (rawDisplayScale > 0.0f) ? rawDisplayScale : 1.0f;
        const float newUiScale = std::clamp(displayScale / std::max(1.0f, newDensity), 1.0f, 2.25f);

        const bool changed =
            (std::fabs(newDensity - state.pixelDensity) > 1e-3f) || (std::fabs(newUiScale - state.uiScale) > 1e-3f);
        state.pixelDensity = newDensity;
        state.uiScale = newUiScale;
        return changed;
    }

    void uploadSimTexture() {
        const int w = world.width();
        const int h = world.height();
        const uint32_t *pixels = world.framebuffer();
        if (w <= 0 || h <= 0 || !pixels) return;

        if (simTexture == 0) {
            glGenTextures(1, &simTexture);
        }
        glBindTexture(GL_TEXTURE_2D, simTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        if (texWidth != w || texHeight != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
            texWidth = w;
            texHeight = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void updateCursor() {
        const ImGuiIO &io = ImGui::GetIO();
        const bool wantCrosshair = state.leftDown || state.rightDown || canvasHovered ||
                                   (!io.WantCaptureMouse && state.canvasDrawRect.contains(mouseX, mouseY));
        if (wantCrosshair != usingCrosshairCursor) {
            SDL_Cursor *target = wantCrosshair ? crosshairCursor : arrowCursor;
            if (target) {
                SDL_SetCursor(target);
            }
            usingCrosshairCursor = wantCrosshair;
        }
    }

    // --- Shared state mutation helpers (used by both GUI triggers and keyboard/mouse shortcuts) ---

    void markSimDirty() noexcept {
        simDirty = true;
        cpuTextureDirty = true;
    }

    void togglePlayPause() {
        state.paused = !state.paused;
        if (state.paused) simAccumulatorSec = 0.0;
    }

    void stepOnce() {
        state.paused = true;
        simAccumulatorSec = 0.0;
        world.step();
        markSimDirty();
    }

    void resetPreset() {
        world.loadPreset(state.currentPreset);
        state.banner.show("Preset ripristinato", BannerTone::Info, 2500);
        markSimDirty();
    }

    void selectPreset(PresetId preset) {
        state.currentPreset = preset;
        world.loadPreset(state.currentPreset);
        markSimDirty();
    }

    void clearWorld() {
        world.clear(MaterialId::Air);
        markSimDirty();
    }

    void exportSnapshot() {
        char filename[128]{};
        const auto tick = static_cast<unsigned long long>(world.stats().tick);
        std::snprintf(filename, sizeof(filename), "snapshot_tick_%llu.ppm", tick);
        const std::string name(filename);

        if (world.savePPM(name, state.viewMode)) {
            state.banner.show("Snapshot salvato: " + name, BannerTone::Success, 4000);
        } else {
            state.banner.show("Errore snapshot: " + name, BannerTone::Error, 4000);
        }
    }

    void cycleGravity() {
        const int nextG = (static_cast<int>(world.gravityMode()) + 1) % 4;
        world.setGravityMode(static_cast<GravityMode>(nextG));
    }

    void toggleBoundary() {
        world.setBoundaryMode(world.boundaryMode() == BoundaryMode::Closed ? BoundaryMode::Open : BoundaryMode::Closed);
    }

    void toggleSpeed() { state.stepsPerFrame = (state.stepsPerFrame == 1) ? 2 : 1; }

    void setViewMode(ViewMode view) {
        if (state.viewMode != view) {
            state.viewMode = view;
            markSimDirty();
        }
    }

    void toggleShaders() {
        if (!state.glShadersAvailable) {
            state.banner.show("Shader GL3 non disponibili (fallback CPU)", BannerTone::Error, 3000);
            return;
        }
        state.gfx.shadersEnabled = !state.gfx.shadersEnabled;
        markSimDirty();
        state.banner.show(state.gfx.shadersEnabled ? "Shader GL3: ON" : "Shader GL3: OFF", BannerTone::Info, 2500);
    }

    void setToolMode(ToolMode tool) { state.toolMode = tool; }

    void selectMaterial(MaterialId mat) {
        state.paintMat = mat;
        state.toolMode = ToolMode::Material;
    }

    void setBrushRadius(int radius) { state.brushRadius = std::clamp(radius, kMinBrushRadius, kMaxBrushRadius); }

    void adjustBrushRadius(int delta) { setBrushRadius(state.brushRadius + delta); }

    void cancelActiveInteraction() {
        const bool wasDragging = state.leftDown || state.rightDown;
        state.lineDrawing = false;
        state.leftDown = false;
        state.rightDown = false;
        state.prevStrokeSimX = -1;
        state.prevStrokeSimY = -1;
        if (wasDragging) {
            SDL_CaptureMouse(false);
        }
    }

    void sampleEyedropper(int simX, int simY) {
        const MaterialId sampled = world.inspect(simX, simY).material;
        if (sampled != MaterialId::Air) {
            selectMaterial(sampled);
        }
    }

    void applyCanvasStroke(int simX, int simY, bool isRight, bool isDrag) {
        const ToolMode effectiveTool = isRight ? ToolMode::Erase : state.toolMode;
        if (isDrag && state.prevStrokeSimX >= 0 && state.prevStrokeSimY >= 0) {
            world.applyToolLine(effectiveTool, state.prevStrokeSimX, state.prevStrokeSimY, simX, simY,
                                state.brushRadius, state.paintMat);
        } else {
            world.applyToolCircle(effectiveTool, simX, simY, state.brushRadius, 0.0f, 0.0f, state.paintMat);
        }
        state.prevStrokeSimX = simX;
        state.prevStrokeSimY = simY;
        markSimDirty();
    }

    void applyGuiOutput(const GuiFrameOutput &out) {
        state.canvasDrawRect = out.canvasDrawRect;
        canvasHovered = out.canvasHovered;
        sidebarScrollable = out.sidebarScrollable;

        if (!state.leftDown && !state.rightDown) {
            int sx = 0;
            int sy = 0;
            if (canvasHovered &&
                screenToSim(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                state.hoverSimX = sx;
                state.hoverSimY = sy;
            } else if (!state.canvasDrawRect.contains(mouseX, mouseY)) {
                state.hoverSimX = -1;
                state.hoverSimY = -1;
            }
        }

        if (out.triggerPlayPause) togglePlayPause();
        if (out.triggerStepOnce) stepOnce();
        if (out.triggerResetPreset) resetPreset();
        if (out.triggerClear) clearWorld();
        if (out.triggerExportSnapshot) exportSnapshot();
        if (out.triggerCycleGravity) cycleGravity();
        if (out.triggerToggleBoundary) toggleBoundary();
        if (out.triggerToggleSpeed) toggleSpeed();

        if (out.hasViewChange) setViewMode(out.newView);
        if (out.hasPresetChange) selectPreset(out.newPreset);
        if (out.hasToolChange) setToolMode(out.newTool);
        if (out.hasMaterialChange) selectMaterial(out.newMaterial);
        if (out.hasBrushChange) setBrushRadius(out.newBrushRadius);
        if (out.hasGraphicsChange) {
            const bool shadersToggled = (state.gfx.shadersEnabled != out.newGraphics.shadersEnabled);
            state.gfx = out.newGraphics;
            if (shadersToggled) {
                markSimDirty();
            }
        }

        if (out.hasSidebarGraphicsToggle) state.showSidebarGraphics = out.showSidebarGraphics;
        if (out.hasSidebarInfoToggle) state.showSidebarInfo = out.showSidebarInfo;
        if (out.hasSidebarHintsToggle) state.showSidebarHints = out.showSidebarHints;
    }

    // --- Event handling ---

    bool canStartCanvasClick(float x, float y) const {
        const ImGuiIO &io = ImGui::GetIO();
        return !io.WantCaptureMouse && state.canvasDrawRect.contains(x, y);
    }

    void handleMouseMotion(const SDL_MouseMotionEvent &motion) {
        mouseX = motion.x;
        mouseY = motion.y;

        int sx = 0;
        int sy = 0;
        if (state.lineDrawing) {
            if (screenToSimClamped(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                state.lineEndSimX = sx;
                state.lineEndSimY = sy;
            }
        }

        const ImGuiIO &io = ImGui::GetIO();
        const bool dragging = state.leftDown || state.rightDown;
        if ((dragging || !io.WantCaptureMouse) &&
            screenToSim(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
            state.hoverSimX = sx;
            state.hoverSimY = sy;
            if (!state.lineDrawing) {
                if (state.leftDown) {
                    applyCanvasStroke(sx, sy, false, true);
                } else if (state.rightDown) {
                    applyCanvasStroke(sx, sy, true, true);
                }
            }
        } else {
            state.hoverSimX = -1;
            state.hoverSimY = -1;
        }
    }

    void handleMouseButtonDown(const SDL_MouseButtonEvent &button) {
        mouseX = button.x;
        mouseY = button.y;
        const SDL_Keymod mods = SDL_GetModState();

        if (button.button == SDL_BUTTON_LEFT) {
            if (state.lineDrawing && state.lineIsRight) {
                cancelActiveInteraction();
                return;
            }
            if ((mods & SDL_KMOD_ALT) != 0) {
                int sx = 0;
                int sy = 0;
                if (canStartCanvasClick(mouseX, mouseY) &&
                    screenToSim(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                    sampleEyedropper(sx, sy);
                }
                return;
            }
            int sx = 0;
            int sy = 0;
            if (canStartCanvasClick(mouseX, mouseY) &&
                screenToSim(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                SDL_CaptureMouse(true);
                state.leftDown = true;
                state.prevStrokeSimX = -1;
                state.prevStrokeSimY = -1;
                if ((mods & SDL_KMOD_SHIFT) != 0) {
                    state.lineDrawing = true;
                    state.lineIsRight = false;
                    state.lineStartSimX = sx;
                    state.lineStartSimY = sy;
                    state.lineEndSimX = sx;
                    state.lineEndSimY = sy;
                } else {
                    state.lineDrawing = false;
                    applyCanvasStroke(sx, sy, false, false);
                }
            }
            return;
        }

        if (button.button == SDL_BUTTON_RIGHT) {
            if (state.lineDrawing && !state.lineIsRight) {
                cancelActiveInteraction();
                return;
            }
            int sx = 0;
            int sy = 0;
            if (canStartCanvasClick(mouseX, mouseY) &&
                screenToSim(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                SDL_CaptureMouse(true);
                state.rightDown = true;
                state.prevStrokeSimX = -1;
                state.prevStrokeSimY = -1;
                if ((mods & SDL_KMOD_SHIFT) != 0) {
                    state.lineDrawing = true;
                    state.lineIsRight = true;
                    state.lineStartSimX = sx;
                    state.lineStartSimY = sy;
                    state.lineEndSimX = sx;
                    state.lineEndSimY = sy;
                } else {
                    state.lineDrawing = false;
                    applyCanvasStroke(sx, sy, true, false);
                }
            }
            return;
        }

        if (button.button == SDL_BUTTON_MIDDLE) {
            int sx = 0;
            int sy = 0;
            if (canStartCanvasClick(mouseX, mouseY) &&
                screenToSim(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                sampleEyedropper(sx, sy);
            }
        }
    }

    void handleMouseButtonUp(const SDL_MouseButtonEvent &button) {
        mouseX = button.x;
        mouseY = button.y;

        if (button.button == SDL_BUTTON_LEFT) {
            if (state.lineDrawing && !state.lineIsRight) {
                int sx = 0;
                int sy = 0;
                if (screenToSimClamped(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                    state.lineEndSimX = sx;
                    state.lineEndSimY = sy;
                }
                world.applyToolLine(state.toolMode, state.lineStartSimX, state.lineStartSimY, state.lineEndSimX,
                                    state.lineEndSimY, state.brushRadius, state.paintMat);
                state.lineDrawing = false;
                markSimDirty();
            }
            state.leftDown = false;
            state.prevStrokeSimX = -1;
            state.prevStrokeSimY = -1;
            if (!state.rightDown) {
                SDL_CaptureMouse(false);
            }
            ImGui::GetIO().AddMousePosEvent(mouseX, mouseY);
            return;
        }

        if (button.button == SDL_BUTTON_RIGHT) {
            if (state.lineDrawing && state.lineIsRight) {
                int sx = 0;
                int sy = 0;
                if (screenToSimClamped(state.canvasDrawRect, world.width(), world.height(), mouseX, mouseY, sx, sy)) {
                    state.lineEndSimX = sx;
                    state.lineEndSimY = sy;
                }
                world.applyToolLine(ToolMode::Erase, state.lineStartSimX, state.lineStartSimY, state.lineEndSimX,
                                    state.lineEndSimY, state.brushRadius, state.paintMat);
                state.lineDrawing = false;
                markSimDirty();
            }
            state.rightDown = false;
            state.prevStrokeSimX = -1;
            state.prevStrokeSimY = -1;
            if (!state.leftDown) {
                SDL_CaptureMouse(false);
            }
            ImGui::GetIO().AddMousePosEvent(mouseX, mouseY);
        }
    }

    void handleMouseWheel(const SDL_MouseWheelEvent &wheel) {
        const ImGuiIO &io = ImGui::GetIO();
        const bool dragging = state.leftDown || state.rightDown;
        const bool overCanvas = canvasHovered || state.canvasDrawRect.contains(wheel.mouse_x, wheel.mouse_y);
        if (!dragging && io.WantCaptureMouse && !overCanvas && sidebarScrollable) {
            return;
        }

        float wheelY = wheel.y;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            wheelY = -wheelY;
        }
        if (wheelY == 0.0f) return;

        if ((wheelY > 0.0f && wheelAccum < 0.0f) || (wheelY < 0.0f && wheelAccum > 0.0f)) {
            wheelAccum = 0.0f;
        }
        wheelAccum += wheelY;
        while (wheelAccum >= 1.0f) {
            adjustBrushRadius(+1);
            wheelAccum -= 1.0f;
        }
        while (wheelAccum <= -1.0f) {
            adjustBrushRadius(-1);
            wheelAccum += 1.0f;
        }
    }

    void handleKeyDown(const SDL_KeyboardEvent &keyEvent) {
        if (ImGui::GetIO().WantTextInput) return;

        const SDL_Keycode key = keyEvent.key;
        const SDL_Keycode baseKey = SDL_GetKeyFromScancode(keyEvent.scancode, SDL_KMOD_NONE, true);
        const bool isRepeat = keyEvent.repeat;
        const bool isBrushMinus = (key == SDLK_LEFTBRACKET) || (keyEvent.scancode == SDL_SCANCODE_LEFTBRACKET);
        const bool isBrushPlus = (key == SDLK_RIGHTBRACKET) || (keyEvent.scancode == SDL_SCANCODE_RIGHTBRACKET);
        const bool isStepKey = (baseKey == SDLK_N);

        if (isRepeat && !isBrushMinus && !isBrushPlus && !isStepKey) {
            return;
        }

        if (key == SDLK_ESCAPE) {
            if (state.lineDrawing) {
                cancelActiveInteraction();
            } else {
                running = false;
            }
            return;
        }

        // Handle layout-aware bracket keys first so AltGr on European keyboards still works.
        if (key == SDLK_LEFTBRACKET) {
            adjustBrushRadius(-1);
            return;
        }
        if (key == SDLK_RIGHTBRACKET) {
            adjustBrushRadius(+1);
            return;
        }

        if ((keyEvent.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
            return;
        }

        if (isBrushMinus) {
            adjustBrushRadius(-1);
            return;
        }
        if (isBrushPlus) {
            adjustBrushRadius(+1);
            return;
        }

        switch (baseKey) {
        case SDLK_SPACE:
            togglePlayPause();
            break;
        case SDLK_N:
            stepOnce();
            break;
        case SDLK_C:
            clearWorld();
            break;
        case SDLK_R:
            resetPreset();
            break;
        case SDLK_S:
            exportSnapshot();
            break;
        case SDLK_G:
            cycleGravity();
            break;
        case SDLK_B:
            toggleBoundary();
            break;
        case SDLK_1:
            setViewMode(ViewMode::Physical);
            break;
        case SDLK_2:
            setViewMode(ViewMode::Thermal);
            break;
        case SDLK_3:
            setViewMode(ViewMode::Pressure);
            break;
        case SDLK_4:
            setViewMode(ViewMode::Velocity);
            break;
        case SDLK_5:
            setViewMode(ViewMode::EnergyLight);
            break;
        case SDLK_F6:
            if ((keyEvent.mod & (SDL_KMOD_SHIFT | SDL_KMOD_MODE | SDL_KMOD_LEVEL5)) == 0) {
                toggleShaders();
            }
            break;
        default:
            break;
        }
    }

    void handleEvent(const SDL_Event &event) {
        const SDL_WindowID myWindowId = SDL_GetWindowID(window);
        const bool canvasDragging = state.leftDown || state.rightDown;
        const bool isMouseEvent = (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                   event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL);

        bool forwardToImGui = !isMouseEvent || !canvasDragging;
        if (forwardToImGui && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT) &&
            canStartCanvasClick(event.button.x, event.button.y)) {
            forwardToImGui = false;
        }
        if (forwardToImGui) {
            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        switch (event.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == myWindowId) {
                running = false;
            }
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (event.window.windowID == myWindowId && updateScaleMetrics() && imguiInitialized) {
                overlay.applyScale(state.uiScale, state.pixelDensity);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
            if (event.window.windowID == myWindowId) {
                cancelActiveInteraction();
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (event.window.windowID == myWindowId && !state.leftDown && !state.rightDown) {
                mouseX = -1.0f;
                mouseY = -1.0f;
                state.hoverSimX = -1;
                state.hoverSimY = -1;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.windowID == myWindowId) {
                handleMouseMotion(event.motion);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.windowID == myWindowId) {
                handleMouseButtonDown(event.button);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.windowID == myWindowId) {
                handleMouseButtonUp(event.button);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.windowID == myWindowId) {
                handleMouseWheel(event.wheel);
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.windowID == myWindowId) {
                handleKeyDown(event.key);
            }
            break;
        default:
            break;
        }
    }

    void renderAndPresent() {
        const bool useGpuShaders = state.glShadersAvailable && state.gfx.shadersEnabled &&
                                   state.viewMode == ViewMode::Physical && glPipeline.isReady();

        uint64_t activeTextureId = 0;
        bool simTextureBottomLeftOrigin = false;
        bool gpuRendered = false;

        if (useGpuShaders) {
            int targetW = std::max(64, world.width() * 2);
            int targetH = std::max(64, world.height() * 2);
            if (state.canvasDrawRect.width() > 0.0f && state.canvasDrawRect.height() > 0.0f) {
                targetW = std::max(64, static_cast<int>(std::round(state.canvasDrawRect.width() * state.pixelDensity)));
                targetH =
                    std::max(64, static_cast<int>(std::round(state.canvasDrawRect.height() * state.pixelDensity)));
            }
            const float timeSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();
            gpuRendered = glPipeline.render(world, state.gfx, targetW, targetH, timeSec, simDirty);
            if (gpuRendered) {
                simDirty = false;
                activeTextureId = glPipeline.outputTextureId();
                simTextureBottomLeftOrigin = true;
            } else if (!glPipeline.isReady()) {
                state.glShadersAvailable = false;
            }
        }

        if (!gpuRendered) {
            if (cpuTextureDirty || simTexture == 0) {
                world.render(state.viewMode);
                uploadSimTexture();
                cpuTextureDirty = false;
            }
            activeTextureId = static_cast<uint64_t>(simTexture);
            simTextureBottomLeftOrigin = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const GuiFrameOutput guiOut = overlay.renderFrame(world, state, activeTextureId, simTextureBottomLeftOrigin);
        applyGuiOutput(guiOut);
        updateCursor();

        ImGui::Render();

        int pixelW = 0;
        int pixelH = 0;
        SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
        glViewport(0, 0, std::max(1, pixelW), std::max(1, pixelH));
        glClearColor(14.0f / 255.0f, 17.0f / 255.0f, 25.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    SimWorld &world;
    AppState state{};
    GuiOverlay overlay;
    GlShaderPipeline glPipeline;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    bool sdlInitialized = false;
    bool imguiInitialized = false;
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;
    SDL_Cursor *arrowCursor = nullptr;
    SDL_Cursor *crosshairCursor = nullptr;
    bool usingCrosshairCursor = false;

    GLuint simTexture = 0;
    int texWidth = 0;
    int texHeight = 0;

    bool vsyncActive = false;
    bool running = false;
    bool simDirty = true;
    bool cpuTextureDirty = true;
    bool canvasHovered = false;
    bool sidebarScrollable = false;

    float mouseX = -1.0f;
    float mouseY = -1.0f;
    float wheelAccum = 0.0f;
    double simAccumulatorSec = 0.0;
};

SdlApp::SdlApp(SimWorld &world, ViewMode initialView, PresetId initialPreset)
    : m_impl(std::make_unique<Impl>(world, initialView, initialPreset)) {}

SdlApp::~SdlApp() = default;

bool SdlApp::create(int windowWidth, int windowHeight) {
    if (m_impl->window) return true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init(SDL_INIT_VIDEO) fallito: " << SDL_GetError() << '\n';
        return false;
    }
    m_impl->sdlInitialized = true;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    constexpr SDL_WindowFlags kWindowFlags =
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    m_impl->window =
        SDL_CreateWindow("Simulatore Fisico 2D Per-Pixel (C++20)", windowWidth, windowHeight, kWindowFlags);
    if (!m_impl->window) {
        std::cerr << "SDL_CreateWindow fallito: " << SDL_GetError() << '\n';
        m_impl->shutdown();
        return false;
    }

    m_impl->updateScaleMetrics();
    SDL_Rect usable{0, 0, 1920, 1080};
    SDL_DisplayID displayId = SDL_GetDisplayForWindow(m_impl->window);
    if (displayId == 0) {
        displayId = SDL_GetPrimaryDisplay();
    }
    if (displayId != 0) {
        SDL_GetDisplayUsableBounds(displayId, &usable);
    }

    const float scale = m_impl->state.uiScale;
    auto uiPx = [scale](int basePx) -> int { return static_cast<int>(std::round(static_cast<float>(basePx) * scale)); };
    const int desiredW = uiPx(windowWidth);
    const int desiredH = uiPx(windowHeight);
    const int minClampW = std::min(uiPx(800), usable.w);
    const int minClampH = std::min(uiPx(600), usable.h);
    const int w = std::max(1, std::min(desiredW, std::max(minClampW, usable.w - uiPx(40))));
    const int h = std::max(1, std::min(desiredH, std::max(minClampH, usable.h - uiPx(60))));
    const int x = usable.x + std::max(0, (usable.w - w) / 2);
    const int y = usable.y + std::max(0, (usable.h - h) / 2);

    SDL_SetWindowSize(m_impl->window, w, h);
    SDL_SetWindowMinimumSize(m_impl->window, std::min(uiPx(640), w), std::min(uiPx(480), h));
    SDL_SetWindowPosition(m_impl->window, x, y);
    SDL_ShowWindow(m_impl->window);

    m_impl->glContext = SDL_GL_CreateContext(m_impl->window);
    if (!m_impl->glContext) {
        std::cerr << "SDL_GL_CreateContext fallito: " << SDL_GetError() << '\n';
        m_impl->shutdown();
        return false;
    }
    if (!SDL_GL_MakeCurrent(m_impl->window, m_impl->glContext)) {
        std::cerr << "SDL_GL_MakeCurrent fallito: " << SDL_GetError() << '\n';
        m_impl->shutdown();
        return false;
    }
    if (SDL_GL_SetSwapInterval(1)) {
        m_impl->vsyncActive = true;
    } else {
        SDL_GL_SetSwapInterval(0);
        m_impl->vsyncActive = false;
    }

    m_impl->arrowCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    m_impl->crosshairCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    if (m_impl->arrowCursor) {
        SDL_SetCursor(m_impl->arrowCursor);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    if (!ImGui_ImplSDL3_InitForOpenGL(m_impl->window, m_impl->glContext)) {
        std::cerr << "ImGui_ImplSDL3_InitForOpenGL fallito.\n";
        ImGui::DestroyContext();
        m_impl->shutdown();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        std::cerr << "ImGui_ImplOpenGL3_Init fallito.\n";
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_impl->shutdown();
        return false;
    }
    m_impl->imguiInitialized = true;

    m_impl->updateScaleMetrics();
    m_impl->overlay.initialize(m_impl->state.uiScale, m_impl->state.pixelDensity);

    const bool glReady = m_impl->glPipeline.initialize();
    m_impl->state.glShadersAvailable = glReady && m_impl->glPipeline.isReady();
    m_impl->startTime = std::chrono::steady_clock::now();
    m_impl->markSimDirty();
    m_impl->renderAndPresent();
    return true;
}

int SdlApp::run() {
    if (!m_impl->window) return 1;

    using Clock = std::chrono::steady_clock;
    constexpr double kSimTickSec = 1.0 / 60.0;
    constexpr double kMaxFrameDtSec = 0.1;
    constexpr int kMaxTicksPerFrame = 3;

    m_impl->running = true;
    m_impl->simAccumulatorSec = 0.0;

    auto prevTime = Clock::now();
    auto fpsTimer = prevTime;
    int frameCount = 0;

    while (m_impl->running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            m_impl->handleEvent(event);
        }
        if (!m_impl->running) break;

        const auto now = Clock::now();
        double dtSec = std::chrono::duration<double>(now - prevTime).count();
        prevTime = now;
        dtSec = std::clamp(dtSec, 0.0, kMaxFrameDtSec);

        // Snap ~60 Hz VSync intervals to exact 1/60 s to prevent 0/2 tick micro-stutter.
        if (m_impl->vsyncActive && dtSec >= (1.0 / 62.5) && dtSec <= (1.0 / 57.5)) {
            dtSec = kSimTickSec;
        }

        if (!m_impl->state.banner.text.empty() && !m_impl->state.banner.isActive(now)) {
            m_impl->state.banner.clear();
        }

        bool stepped = false;
        if (!m_impl->state.paused) {
            m_impl->simAccumulatorSec += dtSec;
            int simBatches = 0;
            while (m_impl->simAccumulatorSec >= kSimTickSec && simBatches < kMaxTicksPerFrame) {
                for (int s = 0; s < m_impl->state.stepsPerFrame; ++s) {
                    m_impl->world.step();
                }
                m_impl->simAccumulatorSec -= kSimTickSec;
                ++simBatches;
                stepped = true;
            }
            if (simBatches >= kMaxTicksPerFrame && m_impl->simAccumulatorSec > kSimTickSec) {
                m_impl->simAccumulatorSec = std::fmod(m_impl->simAccumulatorSec, kSimTickSec);
            }
        } else {
            m_impl->simAccumulatorSec = 0.0;
        }

        if (stepped) {
            m_impl->markSimDirty();
        }

        int winPixelW = 0;
        int winPixelH = 0;
        SDL_GetWindowSizeInPixels(m_impl->window, &winPixelW, &winPixelH);
        if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_MINIMIZED) != 0 || winPixelW <= 0 || winPixelH <= 0) {
            SDL_Delay(16);
            continue;
        }

        m_impl->renderAndPresent();

        ++frameCount;
        const double fpsElapsed = std::chrono::duration<double>(now - fpsTimer).count();
        if (fpsElapsed >= 0.4) {
            m_impl->state.fps = static_cast<double>(frameCount) / fpsElapsed;
            frameCount = 0;
            fpsTimer = now;
        }

        if (!m_impl->vsyncActive) {
            SDL_Delay(1);
        }
    }

    return 0;
}

} // namespace sim::app
