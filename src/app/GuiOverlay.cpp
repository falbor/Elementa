#include "GuiOverlay.hpp"

#include "sim/Material.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sim::app {

namespace {

constexpr float kSidebarBaseWidth = 242.0f;
constexpr float kStatusBaseHeight = 50.0f;
constexpr float kBaseFontSize = 13.0f;

constexpr ImU32 col32(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
    return IM_COL32(r, g, b, a);
}

constexpr ImVec4 colVec4(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
                  static_cast<float>(a) / 255.0f);
}

const char *phaseNameIt(PhaseState p) noexcept {
    switch (p) {
    case PhaseState::EmptyGas:
        return "Gas/Aria";
    case PhaseState::Powder:
        return "Polvere";
    case PhaseState::Liquid:
        return "Liquido";
    case PhaseState::StaticSolid:
        return "Solido";
    case PhaseState::Energy:
        return "Energia";
    }
    return "-";
}

const char *gravityNameIt(GravityMode g) noexcept {
    switch (g) {
    case GravityMode::Earth:
        return "G: Terra";
    case GravityMode::ZeroG:
        return "G: Zero-G";
    case GravityMode::Inverted:
        return "G: Inversa";
    case GravityMode::RadialCenter:
        return "G: Centro";
    }
    return "G: Terra";
}

const char *toolNameIt(ToolMode t) noexcept {
    switch (t) {
    case ToolMode::Material:
        return "Pittura";
    case ToolMode::Wind:
        return "Vento";
    case ToolMode::Heat:
        return "Calore";
    case ToolMode::Cryo:
        return "Freddo";
    case ToolMode::SparkTool:
        return "Scintilla";
    case ToolMode::Attractor:
        return "Attrae";
    case ToolMode::Repulsor:
        return "Respinge";
    case ToolMode::Erase:
        return "Gomma";
    }
    return "Pittura";
}

const char *toolHintIt(ToolMode t) noexcept {
    switch (t) {
    case ToolMode::Material:
        return "Deposita materiale";
    case ToolMode::Wind:
        return "Impulso flusso aria";
    case ToolMode::Heat:
        return "Torcia termica +180K";
    case ToolMode::Cryo:
        return "Raffredda -120K";
    case ToolMode::SparkTool:
        return "Scarica elettrica +1.4V";
    case ToolMode::Attractor:
        return "Campo attrattivo radiale";
    case ToolMode::Repulsor:
        return "Campo repulsivo radiale";
    case ToolMode::Erase:
        return "Cancella in Aria";
    }
    return "-";
}

ImU32 toolAccentColor(ToolMode t) noexcept {
    switch (t) {
    case ToolMode::Material:
        return col32(59, 130, 246);
    case ToolMode::Wind:
        return col32(56, 189, 248);
    case ToolMode::Heat:
        return col32(249, 115, 22);
    case ToolMode::Cryo:
        return col32(125, 211, 252);
    case ToolMode::SparkTool:
        return col32(250, 204, 21);
    case ToolMode::Attractor:
        return col32(168, 85, 247);
    case ToolMode::Repulsor:
        return col32(244, 63, 94);
    case ToolMode::Erase:
        return col32(248, 113, 113);
    }
    return col32(59, 130, 246);
}

int effectiveBrushRadius(ToolMode tool, int brushRadius) noexcept {
    return (tool == ToolMode::Material || tool == ToolMode::Erase) ? brushRadius : std::max(1, brushRadius);
}

std::string activeToolSummary(const AppState &state) {
    if (state.toolMode == ToolMode::Material) {
        const auto &props = getMaterial(state.paintMat);
        return std::string(props.nameIt) + " [" + phaseNameIt(props.phase) + "]";
    }
    return std::string("Strumento: ") + toolNameIt(state.toolMode);
}

std::string activeToolDetails(const AppState &state) {
    char buf[160]{};
    if (state.toolMode == ToolMode::Material) {
        const auto &props = getMaterial(state.paintMat);
        if (props.electricalConductivity > 0.2f) {
            std::snprintf(buf, sizeof(buf), "Dens %.2f | Cond.El %.0f%% | r=%d px", static_cast<double>(props.density),
                          static_cast<double>(props.electricalConductivity * 100.0f), state.brushRadius);
        } else if (props.ignitionTempK > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Dens %.2f | Igniz %.0f C | r=%d px", static_cast<double>(props.density),
                          static_cast<double>(props.ignitionTempK - kKelvinZeroC), state.brushRadius);
        } else if (props.meltTempK > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Dens %.2f | Fonde %.0f C | r=%d px", static_cast<double>(props.density),
                          static_cast<double>(props.meltTempK - kKelvinZeroC), state.brushRadius);
        } else if (props.boilTempK > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Dens %.2f | Bolle %.0f C | r=%d px", static_cast<double>(props.density),
                          static_cast<double>(props.boilTempK - kKelvinZeroC), state.brushRadius);
        } else {
            std::snprintf(buf, sizeof(buf), "Dens %.2f | Cond.T %.2f | r=%d px", static_cast<double>(props.density),
                          static_cast<double>(props.thermalConductivity), state.brushRadius);
        }
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%s | r=%d px", toolHintIt(state.toolMode), std::max(1, state.brushRadius));
    return buf;
}

ImU32 applyStyleAlpha(ImU32 col) noexcept {
    const float alpha = ImGui::GetStyle().Alpha;
    if (alpha >= 0.999f) return col;
    const uint32_t a = (col >> IM_COL32_A_SHIFT) & 0xFFu;
    const uint32_t scaledA = static_cast<uint32_t>(std::clamp(static_cast<float>(a) * alpha, 0.0f, 255.0f));
    return (col & ~(0xFFu << IM_COL32_A_SHIFT)) | (scaledA << IM_COL32_A_SHIFT);
}

void drawTextEllipsis(ImDrawList *dl, const ImVec2 &boxMin, const ImVec2 &boxMax, ImU32 col, std::string_view text,
                      bool centerX) {
    if (!dl || text.empty()) return;
    col = applyStyleAlpha(col);
    const float maxW = std::max(0.0f, boxMax.x - boxMin.x);
    if (maxW <= 1.0f) return;

    const float lineH = ImGui::GetTextLineHeight();
    const float textY = boxMin.y + std::floor(((boxMax.y - boxMin.y) - lineH) * 0.5f);

    const char *begin = text.data();
    const char *end = begin + text.size();
    const ImVec2 fullSz = ImGui::CalcTextSize(begin, end);

    if (fullSz.x <= maxW) {
        const float textX = centerX ? (boxMin.x + std::floor((maxW - fullSz.x) * 0.5f)) : boxMin.x;
        dl->AddText(ImVec2(textX, textY), col, begin, end);
        return;
    }

    static constexpr std::string_view kEllipsis = "...";
    const ImVec2 ellSz = ImGui::CalcTextSize(kEllipsis.data(), kEllipsis.data() + kEllipsis.size());
    if (ellSz.x >= maxW) {
        dl->PushClipRect(boxMin, boxMax, true);
        dl->AddText(ImVec2(boxMin.x, textY), col, kEllipsis.data(), kEllipsis.data() + kEllipsis.size());
        dl->PopClipRect();
        return;
    }

    const float budget = maxW - ellSz.x;
    size_t bestLen = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = pos + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0u) == 0x80u) {
            ++next;
        }
        const ImVec2 subSz = ImGui::CalcTextSize(begin, begin + next);
        if (subSz.x > budget) break;
        bestLen = next;
        pos = next;
    }

    std::string clipped(begin, bestLen);
    clipped += kEllipsis;
    const ImVec2 clipSz = ImGui::CalcTextSize(clipped.c_str(), clipped.c_str() + clipped.size());
    const float textX = centerX ? (boxMin.x + std::max(0.0f, std::floor((maxW - clipSz.x) * 0.5f))) : boxMin.x;
    dl->PushClipRect(boxMin, boxMax, true);
    dl->AddText(ImVec2(textX, textY), col, clipped.c_str(), clipped.c_str() + clipped.size());
    dl->PopClipRect();
}

bool drawStyledButton(const char *id, const char *label, const ImVec2 &size, float uiScale, bool active,
                      ImU32 accentCol, bool hasSwatch = false, ImU32 swatchCol = 0) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ImVec2 rMin = ImGui::GetItemRectMin();
    const ImVec2 rMax = ImGui::GetItemRectMax();
    if (!ImGui::IsRectVisible(rMin, rMax)) return clicked;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = 5.0f * uiScale;
    const ImU32 fillCol = active ? (hovered ? col32(43, 62, 96) : col32(36, 52, 82))
                                 : (held ? col32(34, 42, 60) : (hovered ? col32(42, 49, 68) : col32(28, 33, 47)));
    const ImU32 borderCol = active ? accentCol : (hovered ? col32(90, 102, 130) : col32(46, 54, 74));

    dl->AddRectFilled(rMin, rMax, applyStyleAlpha(fillCol), rounding);
    dl->AddRect(rMin, rMax, applyStyleAlpha(borderCol), rounding, 1.0f);

    float textLeft = rMin.x + 4.0f * uiScale;
    if (hasSwatch) {
        const float swPadX = 6.0f * uiScale;
        const float swPadY = 4.0f * uiScale;
        const float swSize = std::max(6.0f, (rMax.y - rMin.y) - 2.0f * swPadY);
        const ImVec2 swMin(rMin.x + swPadX, rMin.y + swPadY);
        const ImVec2 swMax(swMin.x + swSize, swMin.y + swSize);
        const float swRound = 2.0f * uiScale;
        dl->AddRectFilled(swMin, swMax, applyStyleAlpha(swatchCol), swRound);
        dl->AddRect(swMin, swMax, applyStyleAlpha(col32(20, 24, 34)), swRound, 1.0f);
        textLeft = swMax.x + 5.0f * uiScale;
    }

    const ImVec2 textMin(textLeft, rMin.y);
    const ImVec2 textMax(std::max(textLeft + 1.0f, rMax.x - 4.0f * uiScale), rMax.y);
    const ImU32 textCol = active ? col32(248, 250, 252) : col32(205, 214, 228);
    drawTextEllipsis(dl, textMin, textMax, textCol, label, !hasSwatch);
    return clicked;
}

void drawDashedLine(ImDrawList *dl, const ImVec2 &p0, const ImVec2 &p1, ImU32 col, float thickness, float dashLen,
                    float gapLen) {
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;
    const float len = std::hypot(dx, dy);
    if (len < 0.5f) return;

    const float ux = dx / len;
    const float uy = dy / len;
    const float step = std::max(1.0f, dashLen + gapLen);
    for (float t = 0.0f; t < len; t += step) {
        const float tEnd = std::min(len, t + dashLen);
        dl->AddLine(ImVec2(p0.x + ux * t, p0.y + uy * t), ImVec2(p0.x + ux * tEnd, p0.y + uy * tEnd), col, thickness);
    }
}

void renderSidebarGraphicsCard(ImDrawList *dl, const AppState &state, GuiFrameOutput &out, float startX, float availW,
                               float uiScale) {
    auto sc = [uiScale](float basePx) noexcept -> float { return std::round(basePx * uiScale); };

    const float cardTop = ImGui::GetCursorScreenPos().y;
    const float smallFontSize = kBaseFontSize * 0.85f;
    const float innerPadX = sc(6.0f);
    const float innerW = std::max(40.0f, availW - 2.0f * innerPadX);
    const bool isPhysical = (state.viewMode == ViewMode::Physical);

    static constexpr const char *kUnavailNote = "Shader GL3 non disponibili (fallback CPU)";
    static constexpr const char *kViewNote = "Effetti attivi in vista 1 Fisico";

    float noteH = 0.0f;
    if (state.showSidebarGraphics && (!state.glShadersAvailable || !isPhysical)) {
        ImGui::PushFont(nullptr, smallFontSize);
        const char *noteTxt = !state.glShadersAvailable ? kUnavailNote : kViewNote;
        noteH = ImGui::CalcTextSize(noteTxt, nullptr, false, innerW).y + sc(3.0f);
        ImGui::PopFont();
    }

    const float headerH = std::max(sc(22.0f), ImGui::GetTextLineHeight() + sc(6.0f));
    const float innerColGap = sc(4.0f);
    const float innerColW = std::floor((innerW - innerColGap) * 0.5f);
    const float gfxBtnH = std::max(sc(20.0f), ImGui::GetTextLineHeight() + sc(5.0f));
    const float gfxRowStep = gfxBtnH + sc(3.0f);
    const float gridH = 2.0f * gfxBtnH + sc(3.0f);
    const float gridToSliderGap = sc(5.0f);

    const ImVec2 sliderPad(sc(5.0f), sc(2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, sliderPad);
    const float sliderH = ImGui::GetFrameHeight();
    ImGui::PopStyleVar();
    const float sliderGap = sc(3.0f);
    const float slidersH = 3.0f * sliderH + 2.0f * sliderGap;

    float bodyH = 0.0f;
    if (state.showSidebarGraphics) {
        if (!state.glShadersAvailable) {
            bodyH = noteH + sc(4.0f);
        } else {
            bodyH = (isPhysical ? 0.0f : noteH + sc(2.0f)) + gridH + gridToSliderGap + slidersH + sc(6.0f);
        }
    }

    const float cardH = headerH + bodyH;
    const ImVec2 cardMin(startX, cardTop);
    const ImVec2 cardMax(startX + availW, cardTop + cardH);

    ImGui::SetCursorScreenPos(cardMin);
    if (ImGui::InvisibleButton("##toggle_gfx_card", ImVec2(availW, headerH))) {
        out.hasSidebarGraphicsToggle = true;
        out.showSidebarGraphics = !state.showSidebarGraphics;
    }
    const bool gfxHovered = ImGui::IsItemHovered();

    dl->AddRectFilled(cardMin, cardMax, col32(25, 31, 46), sc(6.0f));
    dl->AddRect(cardMin, cardMax, gfxHovered ? col32(84, 102, 138) : col32(56, 70, 98), sc(6.0f), 1.0f);

    const ImU32 dotCol = (state.glShadersAvailable && state.gfx.shadersEnabled)
                             ? (isPhysical ? col32(56, 189, 248) : col32(125, 211, 252, 160))
                             : col32(100, 116, 139);
    const float dotR = std::max(2.5f, sc(3.5f));
    const ImVec2 dotCenter(cardMin.x + sc(10.0f), cardMin.y + headerH * 0.5f);
    dl->AddCircleFilled(dotCenter, dotR, dotCol);

    const char *chevron = state.showSidebarGraphics ? "[-]" : "[+]";
    const float chevW = ImGui::CalcTextSize(chevron).x;
    const float titleLeft = dotCenter.x + dotR + sc(6.0f);
    drawTextEllipsis(dl, ImVec2(titleLeft, cardMin.y),
                     ImVec2(std::max(titleLeft + 1.0f, cardMax.x - chevW - sc(10.0f)), cardMin.y + headerH),
                     col32(205, 214, 228), "SHADER & LUCE (F6)", false);
    drawTextEllipsis(dl, ImVec2(cardMax.x - chevW - sc(6.0f), cardMin.y),
                     ImVec2(cardMax.x - sc(4.0f), cardMin.y + headerH), col32(148, 163, 184), chevron, false);

    if (state.showSidebarGraphics) {
        float curY = cardMin.y + headerH;
        if (!state.glShadersAvailable) {
            ImGui::PushFont(nullptr, smallFontSize);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cardMin.x + innerPadX, curY),
                        col32(148, 163, 184), kUnavailNote, nullptr, innerW);
            ImGui::PopFont();
        } else {
            if (!isPhysical) {
                ImGui::PushFont(nullptr, smallFontSize);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cardMin.x + innerPadX, curY),
                            col32(148, 163, 184), kViewNote, nullptr, innerW);
                ImGui::PopFont();
                curY += noteH + sc(2.0f);
            }

            GraphicsSettings editedGfx = state.gfx;
            bool gfxEdited = false;
            const float bx0 = cardMin.x + innerPadX;
            const float bx1 = bx0 + innerColW + innerColGap;

            ImGui::SetCursorScreenPos(ImVec2(bx0, curY));
            if (drawStyledButton("##gfx_shaders", "Shader GL3", ImVec2(innerColW, gfxBtnH), uiScale,
                                 editedGfx.shadersEnabled, col32(56, 189, 248))) {
                editedGfx.shadersEnabled = !editedGfx.shadersEnabled;
                gfxEdited = true;
            }

            const bool subDisabled = !isPhysical || !state.gfx.shadersEnabled;
            ImGui::BeginDisabled(subDisabled);

            struct SubToggleSpec {
                const char *id;
                const char *label;
                bool *flag;
                ImU32 accent;
                float x;
                float y;
            };
            const std::array<SubToggleSpec, 3> subToggles = {{
                {"##gfx_shadows", "Ombre 2D", &editedGfx.shadowsEnabled, col32(59, 130, 246), bx1, curY},
                {"##gfx_refract", "Rifrazione", &editedGfx.refractionEnabled, col32(14, 165, 233), bx0,
                 curY + gfxRowStep},
                {"##gfx_bloom", "Bloom HDR", &editedGfx.bloomEnabled, col32(245, 158, 11), bx1, curY + gfxRowStep},
            }};
            for (const auto &toggle : subToggles) {
                ImGui::SetCursorScreenPos(ImVec2(toggle.x, toggle.y));
                if (drawStyledButton(toggle.id, toggle.label, ImVec2(innerColW, gfxBtnH), uiScale, *toggle.flag,
                                     toggle.accent)) {
                    *toggle.flag = !(*toggle.flag);
                    gfxEdited = true;
                }
            }

            curY += gridH + gridToSliderGap;

            struct SliderSpec {
                const char *id;
                const char *label;
                float *value;
                float minVal;
                float maxVal;
            };
            const std::array<SliderSpec, 3> sliderRows = {{
                {"##gfx_ambient", "Luce Amb.", &editedGfx.ambientLight, 0.05f, 1.00f},
                {"##gfx_sun", "Sole", &editedGfx.sunIntensity, 0.00f, 1.50f},
                {"##gfx_bloom_int", "Bloom", &editedGfx.bloomIntensity, 0.00f, 1.50f},
            }};

            const float labelW = std::ceil(ImGui::CalcTextSize("Luce Amb.").x + sc(6.0f));
            const float sliderW = std::max(sc(50.0f), innerW - labelW);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, sliderPad);
            for (const auto &row : sliderRows) {
                drawTextEllipsis(dl, ImVec2(bx0, curY), ImVec2(bx0 + labelW - sc(4.0f), curY + sliderH),
                                 col32(186, 198, 214), row.label, false);
                ImGui::SetCursorScreenPos(ImVec2(bx0 + labelW, curY));
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderFloat(row.id, row.value, row.minVal, row.maxVal, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    gfxEdited = true;
                }
                curY += sliderH + sliderGap;
            }
            ImGui::PopStyleVar();
            ImGui::EndDisabled();

            if (gfxEdited) {
                out.hasGraphicsChange = true;
                out.newGraphics = editedGfx;
            }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(startX, cardMax.y + sc(6.0f)));
}

} // namespace

struct GuiOverlay::Impl {
    bool initialized = false;
    float uiScale = 1.0f;
    float pixelDensity = 1.0f;

    float sc(float basePx) const noexcept { return std::round(basePx * uiScale); }
};

GuiOverlay::GuiOverlay() : m_impl(std::make_unique<Impl>()) {}

GuiOverlay::~GuiOverlay() = default;

void GuiOverlay::initialize(float uiScale, float pixelDensity) {
    m_impl->initialized = true;

    ImGuiIO &io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) {
        io.Fonts->AddFontDefaultVector();
    }

    applyScale(uiScale, pixelDensity);
}

void GuiOverlay::applyScale(float uiScale, float pixelDensity) {
    const float clampedScale = std::clamp(uiScale, 1.0f, 2.25f);
    const float clampedDensity = std::max(1.0f, pixelDensity);
    m_impl->uiScale = clampedScale;
    m_impl->pixelDensity = clampedDensity;

    // Reset from a fresh base ImGuiStyle every time so repeated DPI changes never compound
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);

    style.FontSizeBase = kBaseFontSize;
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = clampedScale;

    style.WindowPadding = ImVec2(8.0f, 6.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildRounding = 6.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 5.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.FrameRounding = 5.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(4.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.ScrollbarSize = 9.0f;
    style.ScrollbarRounding = 5.0f;
    style.ScrollbarPadding = 2.0f;
    style.GrabRounding = 4.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    style.Colors[ImGuiCol_Text] = colVec4(205, 214, 228);
    style.Colors[ImGuiCol_TextDisabled] = colVec4(148, 163, 184);
    style.Colors[ImGuiCol_WindowBg] = colVec4(20, 24, 35);
    style.Colors[ImGuiCol_ChildBg] = colVec4(22, 27, 40);
    style.Colors[ImGuiCol_PopupBg] = colVec4(22, 26, 38);
    style.Colors[ImGuiCol_Border] = colVec4(42, 49, 68);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = colVec4(28, 33, 47);
    style.Colors[ImGuiCol_FrameBgHovered] = colVec4(42, 49, 68);
    style.Colors[ImGuiCol_FrameBgActive] = colVec4(36, 52, 82);
    style.Colors[ImGuiCol_Button] = colVec4(28, 33, 47);
    style.Colors[ImGuiCol_ButtonHovered] = colVec4(42, 49, 68);
    style.Colors[ImGuiCol_ButtonActive] = colVec4(36, 52, 82);
    style.Colors[ImGuiCol_Header] = colVec4(36, 52, 82);
    style.Colors[ImGuiCol_HeaderHovered] = colVec4(42, 49, 68);
    style.Colors[ImGuiCol_HeaderActive] = colVec4(36, 52, 82);
    style.Colors[ImGuiCol_Separator] = colVec4(42, 49, 68);
    style.Colors[ImGuiCol_ScrollbarBg] = colVec4(16, 20, 30);
    style.Colors[ImGuiCol_ScrollbarGrab] = colVec4(50, 58, 80);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = colVec4(72, 84, 114);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = colVec4(90, 102, 130);
    style.Colors[ImGuiCol_SliderGrab] = colVec4(37, 118, 206, 220);
    style.Colors[ImGuiCol_SliderGrabActive] = colVec4(56, 168, 238, 235);
    style.Colors[ImGuiCol_DockingEmptyBg] = colVec4(14, 17, 25);

    style.ScaleAllSizes(clampedScale);
    ImGui::GetStyle() = style;
}

GuiFrameOutput GuiOverlay::renderFrame(const SimWorld &world, const AppState &state, uint64_t simTextureId,
                                       bool simTextureBottomLeftOrigin) {
    if (!m_impl->initialized) {
        initialize(state.uiScale, state.pixelDensity);
    }

    GuiFrameOutput out{};
    out.newView = state.viewMode;
    out.newPreset = state.currentPreset;
    out.newTool = state.toolMode;
    out.newMaterial = state.paintMat;
    out.newBrushRadius = state.brushRadius;
    out.newGraphics = state.gfx;
    out.showSidebarGraphics = state.showSidebarGraphics;
    out.showSidebarInfo = state.showSidebarInfo;
    out.showSidebarHints = state.showSidebarHints;

    const float scale = m_impl->uiScale;
    auto sc = [&](float basePx) -> float { return m_impl->sc(basePx); };

    ImGuiViewport *vp = ImGui::GetMainViewport();
    const float winX = vp->Pos.x;
    const float winY = vp->Pos.y;
    const float winW = std::max(1.0f, vp->Size.x);
    const float winH = std::max(1.0f, vp->Size.y);

    // =========================================================================
    // 1. Pre-compute Toolbar button layout & wrapping (zero 1-frame flicker)
    // =========================================================================
    enum class ToolbarGroup : uint8_t { Action = 0, View, Preset };
    struct ToolbarBtnSpec {
        ToolbarGroup group;
        int id;
        const char *imguiId;
        std::string label;
        float width;
        bool active;
        ImU32 accent;
        ImVec2 pos{};
    };

    auto measureBtnW = [&](const char *measureText, float minBaseW) -> float {
        const float textW = ImGui::CalcTextSize(measureText).x;
        return std::max(sc(minBaseW), std::ceil(textW + sc(14.0f)));
    };

    std::vector<ToolbarBtnSpec> actionSpecs = {
        {ToolbarGroup::Action, 0, "##act_playpause", state.paused ? "Avvia" : "Pausa", measureBtnW("Pausa", 66.0f),
         !state.paused, col32(34, 197, 94)},
        {ToolbarGroup::Action, 1, "##act_step", "Passo +1", measureBtnW("Passo +1", 64.0f), false, col32(59, 130, 246)},
        {ToolbarGroup::Action, 2, "##act_reset", "Reset (R)", measureBtnW("Reset (R)", 74.0f), false,
         col32(14, 165, 233)},
        {ToolbarGroup::Action, 3, "##act_clear", "Pulisci", measureBtnW("Pulisci", 60.0f), false, col32(239, 68, 68)},
        {ToolbarGroup::Action, 4, "##act_ppm", "PPM (S)", measureBtnW("PPM (S)", 64.0f), false, col32(16, 185, 129)},
        {ToolbarGroup::Action, 5, "##act_gravity", gravityNameIt(world.gravityMode()), measureBtnW("G: Inversa", 86.0f),
         world.gravityMode() != GravityMode::Earth, col32(168, 85, 247)},
        {ToolbarGroup::Action, 6, "##act_boundary",
         world.boundaryMode() == BoundaryMode::Closed ? "Bordi: Chiusi" : "Bordi: Aperti",
         measureBtnW("Bordi: Chiusi", 94.0f), world.boundaryMode() == BoundaryMode::Open, col32(59, 130, 246)},
        {ToolbarGroup::Action, 7, "##act_speed", state.stepsPerFrame == 1 ? "Vel: 1x" : "Vel: 2x",
         measureBtnW("Vel: 2x", 56.0f), state.stepsPerFrame > 1, col32(245, 158, 11)},
    };

    static constexpr std::array<const char *, 5> kViewNames = {"1 Fisico", "2 Termico", "3 Pressione", "4 Velocita",
                                                               "5 Luce"};
    static constexpr std::array<float, 5> kViewMinWidths = {68.0f, 72.0f, 82.0f, 72.0f, 62.0f};
    static constexpr std::array<const char *, 5> kViewIds = {"##view_0", "##view_1", "##view_2", "##view_3",
                                                             "##view_4"};
    std::vector<ToolbarBtnSpec> viewSpecs;
    viewSpecs.reserve(5);
    for (int v = 0; v < 5; ++v) {
        viewSpecs.push_back({ToolbarGroup::View, v, kViewIds[static_cast<size_t>(v)],
                             kViewNames[static_cast<size_t>(v)],
                             measureBtnW(kViewNames[static_cast<size_t>(v)], kViewMinWidths[static_cast<size_t>(v)]),
                             static_cast<int>(state.viewMode) == v, col32(59, 130, 246)});
    }

    static constexpr std::array<const char *, 6> kPresetNames = {"Preset: Lab", "Vulcano",   "Idrodinamica",
                                                                 "Esplosivi",   "Elettrico", "Orbita Zero-G"};
    static constexpr std::array<float, 6> kPresetMinWidths = {88.0f, 70.0f, 94.0f, 76.0f, 72.0f, 96.0f};
    static constexpr std::array<const char *, 6> kPresetIds = {"##preset_0", "##preset_1", "##preset_2",
                                                               "##preset_3", "##preset_4", "##preset_5"};
    std::vector<ToolbarBtnSpec> presetSpecs;
    presetSpecs.reserve(6);
    for (int p = 0; p < 6; ++p) {
        presetSpecs.push_back(
            {ToolbarGroup::Preset, p, kPresetIds[static_cast<size_t>(p)], kPresetNames[static_cast<size_t>(p)],
             measureBtnW(kPresetNames[static_cast<size_t>(p)], kPresetMinWidths[static_cast<size_t>(p)]),
             static_cast<int>(state.currentPreset) == p, col32(245, 158, 11)});
    }

    const float marginX = sc(10.0f);
    const float gapX = sc(4.0f);
    const float groupGap = sc(8.0f);
    const float btnH = std::max(sc(22.0f), std::ceil(ImGui::GetTextLineHeight() + sc(8.0f)));
    const float rowStep = btnH + sc(4.0f);

    auto measureGroupWidth = [&](const std::vector<ToolbarBtnSpec> &specs) -> float {
        float total = 0.0f;
        for (size_t i = 0; i < specs.size(); ++i) {
            total += specs[i].width;
            if (i + 1 < specs.size()) total += gapX;
        }
        return total;
    };

    float tx = marginX;
    float ty = sc(6.0f);
    auto placeGroup = [&](std::vector<ToolbarBtnSpec> &specs) {
        for (auto &s : specs) {
            if (tx > marginX && tx + s.width > winW - marginX) {
                tx = marginX;
                ty += rowStep;
            }
            s.pos = ImVec2(tx, ty);
            tx += s.width + gapX;
        }
    };

    placeGroup(actionSpecs);
    const float viewsTotalW = measureGroupWidth(viewSpecs);
    if (tx + groupGap + viewsTotalW <= winW - marginX) {
        tx += groupGap;
        placeGroup(viewSpecs);
        tx = marginX;
        ty += rowStep;
        placeGroup(presetSpecs);
    } else {
        tx = marginX;
        ty += rowStep;
        placeGroup(presetSpecs);
        if (tx + groupGap + viewsTotalW <= winW - marginX) {
            tx += groupGap;
        } else {
            tx = marginX;
            ty += rowStep;
        }
        placeGroup(viewSpecs);
    }

    const float toolbarH = ty + btnH + sc(6.0f);
    const float statusH = std::max(sc(kStatusBaseHeight), std::ceil(ImGui::GetTextLineHeight() * 2.0f + sc(20.0f)));
    const float sidebarW = sc(kSidebarBaseWidth);

    const float statusTop = std::max(winY + toolbarH, winY + winH - statusH);
    const float sidebarH = std::max(1.0f, statusTop - (winY + toolbarH));

    // =========================================================================
    // 2. Top Toolbar Window
    // =========================================================================
    constexpr ImGuiWindowFlags kFixedPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    constexpr ImGuiWindowFlags kStaticPanelFlags =
        kFixedPanelFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::SetNextWindowPos(ImVec2(winX, winY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, toolbarH), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colVec4(22, 26, 38));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##Toolbar", nullptr, kStaticPanelFlags)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(winX, winY + toolbarH - 1.0f), ImVec2(winX + winW, winY + toolbarH - 1.0f),
                    col32(42, 49, 68), 1.0f);

        auto renderToolbarGroup = [&](const std::vector<ToolbarBtnSpec> &specs) {
            for (const auto &b : specs) {
                ImGui::SetCursorScreenPos(ImVec2(winX + b.pos.x, winY + b.pos.y));
                if (drawStyledButton(b.imguiId, b.label.c_str(), ImVec2(b.width, btnH), scale, b.active, b.accent)) {
                    switch (b.group) {
                    case ToolbarGroup::Action:
                        switch (b.id) {
                        case 0:
                            out.triggerPlayPause = true;
                            break;
                        case 1:
                            out.triggerStepOnce = true;
                            break;
                        case 2:
                            out.triggerResetPreset = true;
                            break;
                        case 3:
                            out.triggerClear = true;
                            break;
                        case 4:
                            out.triggerExportSnapshot = true;
                            break;
                        case 5:
                            out.triggerCycleGravity = true;
                            break;
                        case 6:
                            out.triggerToggleBoundary = true;
                            break;
                        case 7:
                            out.triggerToggleSpeed = true;
                            break;
                        }
                        break;
                    case ToolbarGroup::View:
                        out.hasViewChange = true;
                        out.newView = static_cast<ViewMode>(b.id);
                        break;
                    case ToolbarGroup::Preset:
                        out.hasPresetChange = true;
                        out.newPreset = static_cast<PresetId>(b.id);
                        break;
                    }
                }
            }
        };

        renderToolbarGroup(actionSpecs);
        renderToolbarGroup(viewSpecs);
        renderToolbarGroup(presetSpecs);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // =========================================================================
    // 3. Left Scrollable Sidebar Window
    // =========================================================================
    ImGui::SetNextWindowPos(ImVec2(winX, winY + toolbarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sidebarW, sidebarH), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colVec4(20, 24, 35));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(8.0f), sc(6.0f)));
    if (ImGui::Begin("##Sidebar", nullptr, kFixedPanelFlags)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float sbRight = winX + sidebarW - 1.0f;
        dl->AddLine(ImVec2(sbRight, winY + toolbarH), ImVec2(sbRight, winY + toolbarH + sidebarH), col32(42, 49, 68),
                    1.0f);

        const float availW = std::max(80.0f, ImGui::GetContentRegionAvail().x);
        const float colGap = sc(6.0f);
        const float colW = std::floor((availW - colGap) * 0.5f);
        const float startX = ImGui::GetCursorScreenPos().x;

        // --- Brush Size ---
        ImGui::TextColored(colVec4(148, 163, 184), "PENNELLO (Rotella / [ ])");
        const float brushBtnW = sc(36.0f);
        const float brushBtnH = std::max(sc(22.0f), ImGui::GetTextLineHeight() + sc(6.0f));
        const float rowY = ImGui::GetCursorScreenPos().y;

        ImGui::SetCursorScreenPos(ImVec2(startX, rowY));
        if (drawStyledButton("##brush_minus", "-", ImVec2(brushBtnW, brushBtnH), scale, false, col32(59, 130, 246))) {
            out.hasBrushChange = true;
            out.newBrushRadius = std::clamp(state.brushRadius - 1, kMinBrushRadius, kMaxBrushRadius);
        }

        char brushTxt[32]{};
        std::snprintf(brushTxt, sizeof(brushTxt), "%d px", state.brushRadius);
        const ImVec2 valMin(startX + brushBtnW + sc(4.0f), rowY);
        const ImVec2 valMax(std::max(valMin.x + 1.0f, startX + availW - brushBtnW - sc(4.0f)), rowY + brushBtnH);
        drawTextEllipsis(dl, valMin, valMax, col32(241, 245, 249), brushTxt, true);

        ImGui::SetCursorScreenPos(ImVec2(startX + availW - brushBtnW, rowY));
        if (drawStyledButton("##brush_plus", "+", ImVec2(brushBtnW, brushBtnH), scale, false, col32(59, 130, 246))) {
            out.hasBrushChange = true;
            out.newBrushRadius = std::clamp(state.brushRadius + 1, kMinBrushRadius, kMaxBrushRadius);
        }

        ImGui::SetCursorScreenPos(ImVec2(startX, rowY + brushBtnH + sc(6.0f)));

        // --- Tools (2 columns) ---
        ImGui::TextColored(colVec4(148, 163, 184), "STRUMENTI FISICI");
        struct ToolEntry {
            ToolMode mode;
            const char *id;
            const char *label;
            ImU32 color;
        };
        static constexpr std::array<ToolEntry, 8> kTools = {{
            {ToolMode::Material, "##tool_0", "Pittura", col32(59, 130, 246)},
            {ToolMode::Wind, "##tool_1", "Vento", col32(56, 189, 248)},
            {ToolMode::Heat, "##tool_2", "Calore", col32(249, 115, 22)},
            {ToolMode::Cryo, "##tool_3", "Freddo", col32(125, 211, 252)},
            {ToolMode::SparkTool, "##tool_4", "Scintilla", col32(250, 204, 21)},
            {ToolMode::Attractor, "##tool_5", "Attrae", col32(168, 85, 247)},
            {ToolMode::Repulsor, "##tool_6", "Respinge", col32(244, 63, 94)},
            {ToolMode::Erase, "##tool_7", "Gomma", col32(148, 163, 184)},
        }};

        const float toolBtnH = std::max(sc(21.0f), ImGui::GetTextLineHeight() + sc(6.0f));
        const float toolRowStep = toolBtnH + sc(3.0f);
        const float toolTopY = ImGui::GetCursorScreenPos().y;
        for (size_t idx = 0; idx < kTools.size(); ++idx) {
            const size_t col = idx % 2;
            const size_t row = idx / 2;
            const float bx = startX + static_cast<float>(col) * (colW + colGap);
            const float by = toolTopY + static_cast<float>(row) * toolRowStep;
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            const auto &tool = kTools[idx];
            if (drawStyledButton(tool.id, tool.label, ImVec2(colW, toolBtnH), scale, state.toolMode == tool.mode,
                                 tool.color, true, tool.color)) {
                out.hasToolChange = true;
                out.newTool = tool.mode;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(startX, toolTopY + 4.0f * toolRowStep + sc(4.0f)));

        // --- Materials (2 columns, exact 24-item order) ---
        ImGui::TextColored(colVec4(148, 163, 184), "MATERIALI (Dx = Gomma)");
        static constexpr std::array<MaterialId, 24> kMatOrder = {
            MaterialId::Sand,  MaterialId::Water,  MaterialId::Stone,   MaterialId::Wood,     MaterialId::Metal,
            MaterialId::Glass, MaterialId::Ice,    MaterialId::Snow,    MaterialId::Salt,     MaterialId::Plant,
            MaterialId::Oil,   MaterialId::Acid,   MaterialId::Lava,    MaterialId::Obsidian, MaterialId::Gunpowder,
            MaterialId::Nitro, MaterialId::Fire,   MaterialId::Smoke,   MaterialId::Steam,    MaterialId::Methane,
            MaterialId::Spark, MaterialId::Plasma, MaterialId::Battery, MaterialId::Bedrock,
        };

        const float matBtnH = std::max(sc(19.0f), ImGui::GetTextLineHeight() + sc(5.0f));
        const float matRowStep = matBtnH + sc(3.0f);
        const float matTopY = ImGui::GetCursorScreenPos().y;
        for (size_t idx = 0; idx < kMatOrder.size(); ++idx) {
            const size_t col = idx % 2;
            const size_t row = idx / 2;
            const float bx = startX + static_cast<float>(col) * (colW + colGap);
            const float by = matTopY + static_cast<float>(row) * matRowStep;
            const MaterialId matId = kMatOrder[idx];
            const auto &props = getMaterial(matId);

            char matBtnId[32]{};
            std::snprintf(matBtnId, sizeof(matBtnId), "##mat_%zu", idx);
            std::string shortLbl(props.shortLabel);
            const bool active = (state.toolMode == ToolMode::Material && state.paintMat == matId);
            const ImU32 swCol = col32(props.baseR, props.baseG, props.baseB);

            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            if (drawStyledButton(matBtnId, shortLbl.c_str(), ImVec2(colW, matBtnH), scale, active, col32(59, 130, 246),
                                 true, swCol)) {
                out.hasMaterialChange = true;
                out.newMaterial = matId;
                out.hasToolChange = true;
                out.newTool = ToolMode::Material;
            }
        }
        const float matRows = static_cast<float>((kMatOrder.size() + 1) / 2);
        ImGui::SetCursorScreenPos(ImVec2(startX, matTopY + matRows * matRowStep + sc(6.0f)));

        // --- Collapsible Active Tool Info Card ---
        {
            const float cardTop = ImGui::GetCursorScreenPos().y;
            const float smallFontSize = kBaseFontSize * 0.86f;
            ImGui::PushFont(nullptr, smallFontSize);
            const float smallLineH = ImGui::GetTextLineHeight() + sc(3.0f);
            ImGui::PopFont();
            const float headerH = std::max(sc(22.0f), ImGui::GetTextLineHeight() + sc(6.0f));
            const float cardH = state.showSidebarInfo ? (headerH + 2.0f * smallLineH + sc(6.0f)) : headerH;
            const ImVec2 cardMin(startX, cardTop);
            const ImVec2 cardMax(startX + availW, cardTop + cardH);

            ImGui::SetCursorScreenPos(cardMin);
            if (ImGui::InvisibleButton("##toggle_info_card", ImVec2(availW, headerH))) {
                out.hasSidebarInfoToggle = true;
                out.showSidebarInfo = !state.showSidebarInfo;
            }
            const bool infoHovered = ImGui::IsItemHovered();

            dl->AddRectFilled(cardMin, cardMax, col32(25, 31, 46), sc(6.0f));
            dl->AddRect(cardMin, cardMax, infoHovered ? col32(84, 102, 138) : col32(56, 70, 98), sc(6.0f), 1.0f);

            const ImU32 swCol = (state.toolMode == ToolMode::Material)
                                    ? col32(getMaterial(state.paintMat).baseR, getMaterial(state.paintMat).baseG,
                                            getMaterial(state.paintMat).baseB)
                                    : toolAccentColor(state.toolMode);
            const float swSize = sc(11.0f);
            const float swY = cardMin.y + std::floor((headerH - swSize) * 0.5f);
            const ImVec2 swMin(cardMin.x + sc(8.0f), swY);
            const ImVec2 swMax(swMin.x + swSize, swY + swSize);
            dl->AddRectFilled(swMin, swMax, swCol, sc(2.0f));
            dl->AddRect(swMin, swMax, col32(16, 20, 30), sc(2.0f), 1.0f);

            const std::string summary = activeToolSummary(state);
            const char *chevron = state.showSidebarInfo ? "[-]" : "[+]";
            const float chevW = ImGui::CalcTextSize(chevron).x;
            const ImVec2 titleMin(swMax.x + sc(6.0f), cardMin.y);
            const ImVec2 titleMax(std::max(titleMin.x + 1.0f, cardMax.x - chevW - sc(10.0f)), cardMin.y + headerH);
            drawTextEllipsis(dl, titleMin, titleMax, col32(241, 245, 249), summary, false);
            drawTextEllipsis(dl, ImVec2(cardMax.x - chevW - sc(6.0f), cardMin.y),
                             ImVec2(cardMax.x - sc(4.0f), cardMin.y + headerH), col32(148, 163, 184), chevron, false);

            if (state.showSidebarInfo) {
                ImGui::PushFont(nullptr, smallFontSize);
                const std::string details = activeToolDetails(state);
                const float detY = cardMin.y + headerH;
                drawTextEllipsis(dl, ImVec2(cardMin.x + sc(8.0f), detY),
                                 ImVec2(cardMax.x - sc(8.0f), detY + smallLineH), col32(186, 198, 214), details, false);
                const float modeY = detY + smallLineH;
                drawTextEllipsis(dl, ImVec2(cardMin.x + sc(8.0f), modeY),
                                 ImVec2(cardMax.x - sc(8.0f), modeY + smallLineH), col32(125, 211, 252),
                                 "Tratto: Click Sx | Linea: Shift+Drag", false);
                ImGui::PopFont();
            }

            ImGui::SetCursorScreenPos(ImVec2(startX, cardMax.y + sc(6.0f)));
        }

        // --- Collapsible Shader & Light Card ---
        renderSidebarGraphicsCard(dl, state, out, startX, availW, scale);

        // --- Collapsible Quick Shortcuts Card ---
        {
            static constexpr std::array<const char *, 5> kHints = {
                "Shift+Drag: Linea retta (Sx / Dx)",   "Click Dx: Gomma | Alt/Mid: Pipetta",
                "Spazio: Pausa | N: Passo | R: Reset", "S: PPM | C: Pulisci | G/B | 1-5",
                "F6: Shader | Rotella/[ ]: Pennello",
            };
            const float cardTop = ImGui::GetCursorScreenPos().y;
            const float smallFontSize = kBaseFontSize * 0.85f;
            ImGui::PushFont(nullptr, smallFontSize);
            const float smallLineH = ImGui::GetTextLineHeight() + sc(3.0f);
            ImGui::PopFont();
            const float headerH = std::max(sc(22.0f), ImGui::GetTextLineHeight() + sc(6.0f));
            const float cardH = state.showSidebarHints
                                    ? (headerH + static_cast<float>(kHints.size()) * smallLineH + sc(6.0f))
                                    : headerH;
            const ImVec2 cardMin(startX, cardTop);
            const ImVec2 cardMax(startX + availW, cardTop + cardH);

            ImGui::SetCursorScreenPos(cardMin);
            if (ImGui::InvisibleButton("##toggle_hints_card", ImVec2(availW, headerH))) {
                out.hasSidebarHintsToggle = true;
                out.showSidebarHints = !state.showSidebarHints;
            }
            const bool hintsHovered = ImGui::IsItemHovered();

            dl->AddRectFilled(cardMin, cardMax, col32(22, 27, 40), sc(6.0f));
            dl->AddRect(cardMin, cardMax, hintsHovered ? col32(72, 84, 114) : col32(42, 49, 68), sc(6.0f), 1.0f);

            const char *chevron = state.showSidebarHints ? "[-]" : "[+]";
            const float chevW = ImGui::CalcTextSize(chevron).x;
            drawTextEllipsis(dl, ImVec2(cardMin.x + sc(8.0f), cardMin.y),
                             ImVec2(cardMax.x - chevW - sc(10.0f), cardMin.y + headerH), col32(148, 163, 184),
                             "SCORCIATOIE RAPIDE", false);
            drawTextEllipsis(dl, ImVec2(cardMax.x - chevW - sc(6.0f), cardMin.y),
                             ImVec2(cardMax.x - sc(4.0f), cardMin.y + headerH), col32(148, 163, 184), chevron, false);

            if (state.showSidebarHints) {
                ImGui::PushFont(nullptr, smallFontSize);
                float hy = cardMin.y + headerH;
                for (const char *hint : kHints) {
                    drawTextEllipsis(dl, ImVec2(cardMin.x + sc(8.0f), hy),
                                     ImVec2(cardMax.x - sc(8.0f), hy + smallLineH), col32(186, 198, 214), hint, false);
                    hy += smallLineH;
                }
                ImGui::PopFont();
            }

            ImGui::SetCursorScreenPos(ImVec2(startX, cardMax.y));
            ImGui::Dummy(ImVec2(availW, sc(2.0f)));
        }

        out.sidebarScrollable = (ImGui::GetScrollMaxY() > 0.5f);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // =========================================================================
    // 4. Bottom 2-Line Status Bar Window
    // =========================================================================
    const float actualStatusH = std::max(1.0f, (winY + winH) - statusTop);
    ImGui::SetNextWindowPos(ImVec2(winX, statusTop), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, actualStatusH), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colVec4(20, 24, 35));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##StatusBar", nullptr, kStaticPanelFlags)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(winX, statusTop), ImVec2(winX + winW, statusTop), col32(42, 49, 68), 1.0f);

        const float padX = sc(12.0f);
        const float line1Top = statusTop + sc(4.0f);
        const float line1Bot = statusTop + sc(24.0f);
        float line1Right = winX + winW - padX;

        if (state.banner.isActive()) {
            ImU32 fillCol = col32(20, 44, 68);
            ImU32 borderCol = col32(56, 189, 248);
            ImU32 textCol = col32(186, 230, 253);
            if (state.banner.tone == BannerTone::Success) {
                fillCol = col32(16, 48, 36);
                borderCol = col32(52, 211, 153);
                textCol = col32(167, 243, 208);
            } else if (state.banner.tone == BannerTone::Error) {
                fillCol = col32(64, 22, 26);
                borderCol = col32(248, 113, 113);
                textCol = col32(254, 202, 202);
            }

            const ImVec2 bSz =
                ImGui::CalcTextSize(state.banner.text.c_str(), state.banner.text.c_str() + state.banner.text.size());
            const float maxBannerW = std::max(sc(140.0f), winW * 0.45f);
            const float pillW = std::min(bSz.x + sc(18.0f), maxBannerW);
            const ImVec2 pillMin(winX + winW - padX - pillW, statusTop + sc(3.0f));
            const ImVec2 pillMax(winX + winW - padX, line1Bot);

            dl->AddRectFilled(pillMin, pillMax, fillCol, sc(4.0f));
            dl->AddRect(pillMin, pillMax, borderCol, sc(4.0f), 1.0f);
            drawTextEllipsis(dl, ImVec2(pillMin.x + sc(6.0f), pillMin.y), ImVec2(pillMax.x - sc(6.0f), pillMax.y),
                             textCol, state.banner.text, true);
            line1Right = pillMin.x - sc(10.0f);
        }

        char line1[360]{};
        if (state.lineDrawing && state.lineStartSimX >= 0 && state.lineStartSimY >= 0 && state.lineEndSimX >= 0 &&
            state.lineEndSimY >= 0) {
            const double dx = static_cast<double>(state.lineEndSimX - state.lineStartSimX);
            const double dy = static_cast<double>(state.lineEndSimY - state.lineStartSimY);
            const double dist = std::hypot(dx, dy);
            const double deg = std::atan2(-dy, dx) * 57.29577951308232;
            const std::string toolStr = state.lineIsRight ? std::string("Gomma") : activeToolSummary(state);
            std::snprintf(
                line1, sizeof(line1),
                "Linea (%d,%d)->(%d,%d) | Lun: %.1f px | Ang: %.1f deg | %s (r=%d) | Rilascia=Applica, Esc=Annulla",
                state.lineStartSimX, state.lineStartSimY, state.lineEndSimX, state.lineEndSimY, dist, deg,
                toolStr.c_str(), state.brushRadius);
        } else if (state.hoverSimX >= 0 && state.hoverSimY >= 0 && world.inBounds(state.hoverSimX, state.hoverSimY)) {
            const PixelInfo px = world.inspect(state.hoverSimX, state.hoverSimY);
            const auto &props = getMaterial(px.material);
            const float tempC = px.tempK - kKelvinZeroC;
            std::snprintf(line1, sizeof(line1),
                          "Pixel (%d,%d) | %.*s [%s] | T: %.1f C (%.0f K) | P: %+.2f | Vp: (%.2f, %.2f) | Va: (%.2f, "
                          "%.2f) | Q: %.2fV",
                          px.x, px.y, static_cast<int>(props.nameIt.size()), props.nameIt.data(),
                          phaseNameIt(props.phase), static_cast<double>(tempC), static_cast<double>(px.tempK),
                          static_cast<double>(px.pressure), static_cast<double>(px.vx), static_cast<double>(px.vy),
                          static_cast<double>(px.airVx), static_cast<double>(px.airVy), static_cast<double>(px.charge));
        } else {
            const std::string summary = activeToolSummary(state);
            std::snprintf(line1, sizeof(line1),
                          "Attivo: %s (r=%d px) | Shift+Drag: Linea | Dx: Gomma | Alt/Mid: Pipetta | S: PPM | R: Reset",
                          summary.c_str(), state.brushRadius);
        }

        if (line1Right > winX + padX + sc(40.0f)) {
            drawTextEllipsis(dl, ImVec2(winX + padX, line1Top), ImVec2(line1Right, line1Bot), col32(125, 211, 252),
                             line1, false);
        }

        const auto &st = world.stats();
        const std::string activeStr = activeToolSummary(state);
        char line2[360]{};
        std::snprintf(line2, sizeof(line2),
                      "FPS: %.1f | Step: %.2f ms (Flu %.2f | Ter %.2f | Reaz %.2f | Part %.2f | Luce %.2f) | Attivi: "
                      "%u/%d | Tick: %llu | [%s | r=%d]",
                      state.fps, st.totalStepMs, st.fluidMs, st.thermoMs, st.reactionMs, st.particleMs, st.lightMs,
                      static_cast<unsigned int>(st.activeParticles), world.pixelCount(),
                      static_cast<unsigned long long>(st.tick), activeStr.c_str(), state.brushRadius);

        const float line2Top = statusTop + sc(25.0f);
        const float line2Bot = statusTop + actualStatusH - sc(3.0f);
        if (line2Bot > line2Top + 1.0f) {
            drawTextEllipsis(dl, ImVec2(winX + padX, line2Top), ImVec2(winX + winW - padX, line2Bot),
                             col32(148, 163, 184), line2, false);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // =========================================================================
    // 5. Central Canvas & Overlays via BackgroundDrawList
    // =========================================================================
    const float pad = sc(8.0f);
    const float boundsLeft = winX + sidebarW + pad;
    const float boundsTop = winY + toolbarH + pad;
    const float boundsRight = std::max(boundsLeft + 1.0f, winX + winW - pad);
    const float boundsBottom = std::max(boundsTop + 1.0f, statusTop - pad);

    const float availW = std::max(64.0f, boundsRight - boundsLeft);
    const float availH = std::max(64.0f, boundsBottom - boundsTop);
    const double worldW = static_cast<double>(std::max(1, world.width()));
    const double worldH = static_cast<double>(std::max(1, world.height()));
    const double fitScale = std::min(static_cast<double>(availW) / worldW, static_cast<double>(availH) / worldH);
    const float drawW = std::max(1.0f, static_cast<float>(std::floor(worldW * fitScale)));
    const float drawH = std::max(1.0f, static_cast<float>(std::floor(worldH * fitScale)));
    const float ox = boundsLeft + std::floor((availW - drawW) * 0.5f);
    const float oy = boundsTop + std::floor((availH - drawH) * 0.5f);
    out.canvasDrawRect = {ox, oy, ox + drawW, oy + drawH};

    const ImVec2 mousePos = ImGui::GetMousePos();
    const bool inCentralRegion =
        (mousePos.x >= winX + sidebarW) && (mousePos.y >= winY + toolbarH) && (mousePos.y < statusTop);
    const bool inCanvas =
        ImGui::IsMousePosValid(&mousePos) && inCentralRegion && out.canvasDrawRect.contains(mousePos.x, mousePos.y);
    out.canvasHovered = inCanvas && !ImGui::GetIO().WantCaptureMouse;

    ImDrawList *bg = ImGui::GetBackgroundDrawList(vp);
    const ImVec2 cMin(out.canvasDrawRect.left, out.canvasDrawRect.top);
    const ImVec2 cMax(out.canvasDrawRect.right, out.canvasDrawRect.bottom);
    if (simTextureId != 0) {
        const ImVec2 uv0 = simTextureBottomLeftOrigin ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = simTextureBottomLeftOrigin ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        bg->AddImage(ImTextureRef(static_cast<ImTextureID>(simTextureId)), cMin, cMax, uv0, uv1);
    } else {
        bg->AddRectFilled(cMin, cMax, col32(11, 14, 22));
    }
    bg->AddRect(ImVec2(cMin.x - 1.0f, cMin.y - 1.0f), ImVec2(cMax.x + 1.0f, cMax.y + 1.0f), col32(55, 65, 90), 0.0f,
                1.0f);

    if (state.lineDrawing && state.lineStartSimX >= 0 && state.lineStartSimY >= 0 && state.lineEndSimX >= 0 &&
        state.lineEndSimY >= 0) {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        simToScreenCenter(out.canvasDrawRect, world.width(), world.height(), state.lineStartSimX, state.lineStartSimY,
                          x0, y0);
        simToScreenCenter(out.canvasDrawRect, world.width(), world.height(), state.lineEndSimX, state.lineEndSimY, x1,
                          y1);

        const float canvasScale = drawW / static_cast<float>(std::max(1, world.width()));
        const ToolMode effTool = state.lineIsRight ? ToolMode::Erase : state.toolMode;
        const int effRadius = effectiveBrushRadius(effTool, state.brushRadius);
        const float rPx = std::max(2.0f, std::round((static_cast<float>(effRadius) + 0.5f) * canvasScale));

        ImU32 accent = col32(56, 189, 248);
        if (state.lineIsRight || state.toolMode == ToolMode::Erase) {
            accent = col32(248, 113, 113);
        } else if (state.toolMode == ToolMode::Material) {
            const auto &props = getMaterial(state.paintMat);
            const int lum = (299 * props.baseR + 587 * props.baseG + 114 * props.baseB) / 1000;
            if (lum >= 105) {
                accent = col32(static_cast<uint8_t>(std::min(255, static_cast<int>(props.baseR) + 35)),
                               static_cast<uint8_t>(std::min(255, static_cast<int>(props.baseG) + 35)),
                               static_cast<uint8_t>(std::min(255, static_cast<int>(props.baseB) + 35)));
            }
        } else {
            accent = toolAccentColor(state.toolMode);
        }

        bg->PushClipRect(cMin, cMax, true);

        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::hypot(dx, dy);
        if (rPx >= 4.0f && len >= 2.0f) {
            const float nx = -dy / len;
            const float ny = dx / len;
            const float oxOff = nx * rPx;
            const float oyOff = ny * rPx;
            const ImU32 guideCol = col32(148, 163, 184);
            drawDashedLine(bg, ImVec2(x0 + oxOff, y0 + oyOff), ImVec2(x1 + oxOff, y1 + oyOff), guideCol, 1.0f, sc(2.0f),
                           sc(4.0f));
            drawDashedLine(bg, ImVec2(x0 - oxOff, y0 - oyOff), ImVec2(x1 - oxOff, y1 - oyOff), guideCol, 1.0f, sc(2.0f),
                           sc(4.0f));
        }

        drawDashedLine(bg, ImVec2(x0, y0), ImVec2(x1, y1), accent, std::max(2.0f, sc(2.0f)), sc(8.0f), sc(5.0f));
        bg->AddCircle(ImVec2(x0, y0), rPx, accent, 0, 1.0f);
        bg->AddCircle(ImVec2(x1, y1), rPx, accent, 0, 1.0f);

        const float cross = sc(4.0f);
        bg->AddLine(ImVec2(x0 - cross, y0), ImVec2(x0 + cross + 1.0f, y0), accent, 1.0f);
        bg->AddLine(ImVec2(x0, y0 - cross), ImVec2(x0, y0 + cross + 1.0f), accent, 1.0f);

        bg->PopClipRect();
    } else if (state.hoverSimX >= 0 && state.hoverSimY >= 0) {
        const float canvasScale = drawW / static_cast<float>(std::max(1, world.width()));
        float cx = 0.0f;
        float cy = 0.0f;
        simToScreenCenter(out.canvasDrawRect, world.width(), world.height(), state.hoverSimX, state.hoverSimY, cx, cy);
        const int effRadius = effectiveBrushRadius(state.toolMode, state.brushRadius);
        const float rPx = std::max(2.0f, std::round((static_cast<float>(effRadius) + 0.5f) * canvasScale));

        bg->PushClipRect(cMin, cMax, true);
        bg->AddCircle(ImVec2(cx, cy), rPx, col32(248, 250, 252), 0, 1.0f);
        bg->PopClipRect();
    }

    return out;
}

} // namespace sim::app
