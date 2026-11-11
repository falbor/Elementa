#include "sim/SimWorld.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sim {

namespace {

inline uint32_t packBGRA(int r, int g, int b) noexcept {
    const uint32_t cr = static_cast<uint32_t>(std::clamp(r, 0, 255));
    const uint32_t cg = static_cast<uint32_t>(std::clamp(g, 0, 255));
    const uint32_t cb = static_cast<uint32_t>(std::clamp(b, 0, 255));
    return 0xFF000000u | (cr << 16) | (cg << 8) | cb;
}

inline uint32_t clampToByte(float v) noexcept {
    if (!(v > 0.0f)) return 0u;
    if (v >= 255.0f) return 255u;
    return static_cast<uint32_t>(v + 0.5f);
}

inline uint32_t packBGRAf(float r, float g, float b) noexcept {
    return 0xFF000000u | (clampToByte(r) << 16) | (clampToByte(g) << 8) | clampToByte(b);
}

// Smooth C1 soft-shoulder tone mapper blended with hue-preserving peak compression
// so bright emissive cores (Plasma, Fire, Lava, Sparks) keep rich color gradients without harsh clipping.
inline uint32_t packToneMappedBGRA(float r255, float g255, float b255) noexcept {
    const float r = (r255 > 0.0f) ? r255 * (1.0f / 255.0f) : 0.0f;
    const float g = (g255 > 0.0f) ? g255 * (1.0f / 255.0f) : 0.0f;
    const float b = (b255 > 0.0f) ? b255 * (1.0f / 255.0f) : 0.0f;

    const float m = std::max({r, g, b});
    constexpr float kKnee = 0.68f;
    if (m <= kKnee) {
        return packBGRAf(r * 255.0f, g * 255.0f, b * 255.0f);
    }

    auto shoulder = [](float x) noexcept -> float {
        if (x <= kKnee) return x;
        const float d = x - kKnee;
        return kKnee + (1.0f - kKnee) * (d / (d + (1.0f - kKnee)));
    };

    const float mMapped = shoulder(m);
    const float scale = mMapped / (m + 1e-6f);
    const float cr = shoulder(r);
    const float cg = shoulder(g);
    const float cb = shoulder(b);

    constexpr float kDesatMix = 0.45f;
    const float outR = (r * scale) * (1.0f - kDesatMix) + cr * kDesatMix;
    const float outG = (g * scale) * (1.0f - kDesatMix) + cg * kDesatMix;
    const float outB = (b * scale) * (1.0f - kDesatMix) + cb * kDesatMix;
    return packBGRAf(outR * 255.0f, outG * 255.0f, outB * 255.0f);
}

// Fast C1 periodic wave in [-1, 1] with period 1.0 (zero trig calls)
inline float fastWave(float t) noexcept {
    const float u = t - std::floor(t);
    const float w = std::abs(2.0f * u - 1.0f);
    return 1.0f - 2.0f * w * w * (3.0f - 2.0f * w);
}

inline uint32_t hash2D(int x, int y) noexcept {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline void checkSolidEdges(const uint8_t* mat, int x, int y, int W, int H,
                            bool& topLeftEdge, bool& botRightEdge) noexcept {
    const int i = y * W + x;
    const bool openL = (x > 0) && !getMaterial(fromIdx(mat[i - 1])).isSolidObstacle();
    const bool openU = (y > 0) && !getMaterial(fromIdx(mat[i - W])).isSolidObstacle();
    const bool openR = (x < W - 1) && !getMaterial(fromIdx(mat[i + 1])).isSolidObstacle();
    const bool openD = (y < H - 1) && !getMaterial(fromIdx(mat[i + W])).isSolidObstacle();
    topLeftEdge = openL || openU;
    botRightEdge = openR || openD;
}

inline uint32_t renderDiagnosticSolid(const uint8_t* mat, int x, int y, int W, int H, uint8_t seed) noexcept {
    bool topLeftEdge = false;
    bool botRightEdge = false;
    checkSolidEdges(mat, x, y, W, H, topLeftEdge, botRightEdge);

    if (topLeftEdge) {
        return packBGRA(145, 162, 188);
    }
    if (botRightEdge) {
        return packBGRA(92, 106, 128);
    }
    const int hatch = (((x + y) & 3) == 0) ? -4 : 0;
    const int grain = (static_cast<int>(seed) - 128) >> 5;
    return packBGRA(38 + hatch + grain, 44 + hatch + grain, 56 + hatch + grain);
}

// Fast bounded Beer-Lambert optical depth probe from nearest open gas surface
inline float computeLiquidDepth(const uint8_t* mat, int x, int y, int W, int H,
                                GravityMode gMode, bool& onCrest) noexcept {
    const int i = y * W + x;
    const bool gasU = (y > 0) && (getMaterial(fromIdx(mat[i - W])).phase == PhaseState::EmptyGas);
    const bool gasD = (y < H - 1) && (getMaterial(fromIdx(mat[i + W])).phase == PhaseState::EmptyGas);
    const bool gasL = (x > 0) && (getMaterial(fromIdx(mat[i - 1])).phase == PhaseState::EmptyGas);
    const bool gasR = (x < W - 1) && (getMaterial(fromIdx(mat[i + 1])).phase == PhaseState::EmptyGas);

    if (gMode == GravityMode::Earth) {
        onCrest = gasU;
        if (gasU) return 0.0f;
        for (int d = 1; d <= 28; d += (d < 4 ? 1 : (d < 10 ? 2 : 4))) {
            const int ny = y - d;
            if (ny < 0) return static_cast<float>(d);
            if (getMaterial(fromIdx(mat[ny * W + x])).phase == PhaseState::EmptyGas) {
                return static_cast<float>(d - 1);
            }
        }
        return 28.0f;
    }

    if (gMode == GravityMode::Inverted) {
        onCrest = gasD;
        if (gasD) return 0.0f;
        for (int d = 1; d <= 28; d += (d < 4 ? 1 : (d < 10 ? 2 : 4))) {
            const int ny = y + d;
            if (ny >= H) return static_cast<float>(d);
            if (getMaterial(fromIdx(mat[ny * W + x])).phase == PhaseState::EmptyGas) {
                return static_cast<float>(d - 1);
            }
        }
        return 28.0f;
    }

    // ZeroG or RadialCenter: measure minimum thickness to nearest open gas surface
    onCrest = gasU || gasD || gasL || gasR;
    if (onCrest) return 0.0f;
    for (int d = 2; d <= 16; d += (d < 6 ? 2 : 3)) {
        if ((y - d >= 0 && getMaterial(fromIdx(mat[(y - d) * W + x])).phase == PhaseState::EmptyGas) ||
            (y + d < H && getMaterial(fromIdx(mat[(y + d) * W + x])).phase == PhaseState::EmptyGas) ||
            (x - d >= 0 && getMaterial(fromIdx(mat[y * W + (x - d)])).phase == PhaseState::EmptyGas) ||
            (x + d < W && getMaterial(fromIdx(mat[y * W + (x + d)])).phase == PhaseState::EmptyGas)) {
            return static_cast<float>(d - 1);
        }
    }
    return 16.0f;
}

inline void shadePhysicalMaterial(MaterialId mat, const MaterialProps& props,
                                  const uint8_t* matPtr, int x, int y, int W, int H,
                                  uint8_t colorSeed, float tempK, float life,
                                  float vx, float vy, float airVx, float airVy,
                                  GravityMode gMode, float tPhase,
                                  float& outR, float& outG, float& outB) noexcept {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float seedNorm = (static_cast<float>(colorSeed) - 128.0f) * (1.0f / 128.0f);
    const float seedDelta = seedNorm * static_cast<float>(props.colorVariance);

    float r = static_cast<float>(props.baseR);
    float g = static_cast<float>(props.baseG);
    float b = static_cast<float>(props.baseB);

    switch (mat) {
    case MaterialId::Stone: {
        // Layered sedimentary rock striations with subtle warp & mineral speckles
        const float warp = 0.38f * fastWave(fx * 0.032f + fy * 0.014f);
        const float strata = fastWave(fy * 0.115f + warp) + 0.35f * fastWave(fy * 0.29f + fx * 0.02f);
        r += strata * 10.5f + seedDelta * 0.72f;
        g += strata * 9.5f + seedDelta * 0.68f;
        b += strata * 7.0f + seedDelta * 0.68f;
        break;
    }
    case MaterialId::Obsidian: {
        // Diagonal vitreous volcanic strata with subtle crystal-glass glints
        const float warp = 0.42f * fastWave((fx + fy) * 0.038f);
        const float strata = fastWave(fy * 0.145f - fx * 0.068f + warp);
        const float glint = (colorSeed > 234) ? 8.0f : 0.0f;
        r += strata * 9.0f + seedDelta * 0.65f + glint * 0.6f;
        g += strata * 6.0f + seedDelta * 0.55f + glint * 0.4f;
        b += strata * 14.5f + seedDelta * 0.85f + glint * 1.2f;
        break;
    }
    case MaterialId::Bedrock: {
        // Dense interlocking dark basalt/masonry pattern
        const float block = fastWave(fx * 0.09f) * fastWave(fy * 0.09f);
        r += block * 6.5f + seedDelta * 0.65f;
        g += block * 7.0f + seedDelta * 0.65f;
        b += block * 8.5f + seedDelta * 0.75f;
        break;
    }
    case MaterialId::Wood: {
        // Organic elongated wood-grain rings and fine fibers
        const float warp = 0.55f * fastWave(fx * 0.024f + fy * 0.011f);
        const float grain = fastWave(fy * 0.23f + fx * 0.018f + warp) +
                            0.32f * fastWave(fy * 0.68f + warp * 0.5f);
        const float delta = grain * 14.0f + seedDelta * 0.50f;
        r += delta * 1.15f;
        g += delta * 0.82f;
        b += delta * 0.46f;
        break;
    }
    case MaterialId::Plant: {
        const float leaf = 0.5f * (fastWave((fx + fy) * 0.15f) + fastWave((fx - fy) * 0.19f));
        r += leaf * 10.0f + seedDelta * 0.65f;
        g += leaf * 18.0f + seedDelta * 0.85f;
        b += leaf * 6.0f + seedDelta * 0.55f;
        break;
    }
    case MaterialId::Metal: {
        // Anisotropic diagonal metallic specular sheen + fine horizontal brushed streaks
        const float diag = 0.5f + 0.5f * fastWave(fx * 0.023f + fy * 0.041f);
        const float sheen = diag * diag * diag * 36.0f;
        const float brush = fastWave(fy * 0.62f + fx * 0.035f) * 5.0f;
        r += sheen * 0.90f + brush + seedDelta * 0.55f;
        g += sheen * 0.98f + brush + seedDelta * 0.55f;
        b += sheen * 1.12f + brush + seedDelta * 0.65f;
        break;
    }
    case MaterialId::Ice:
    case MaterialId::Glass: {
        // Tilted geometric crystal facets via integer rotated cell grid
        const int u = x * 3 + y * 2;
        const int v = y * 3 - x * 2;
        const int cellD = (mat == MaterialId::Ice) ? 20 : 28;
        const int cu = (u >= 0) ? (u / cellD) : ((u - cellD + 1) / cellD);
        const int cv = (v >= 0) ? (v / cellD) : ((v - cellD + 1) / cellD);
        const int fu = u - cu * cellD;
        const int fv = v - cv * cellD;
        const float facetTone = static_cast<float>(hash2D(cu, cv) & 255u) * (1.0f / 255.0f) - 0.5f;
        const bool facetEdge = (fu <= 1 || fv <= 1);

        if (mat == MaterialId::Ice) {
            const float rim = facetEdge ? 18.0f : 0.0f;
            const float frost = (colorSeed > 236) ? 10.0f : 0.0f;
            r += facetTone * 24.0f + rim * 0.85f + frost + seedDelta * 0.55f;
            g += facetTone * 26.0f + rim * 1.00f + frost + seedDelta * 0.55f;
            b += facetTone * 22.0f + rim * 1.05f + frost + seedDelta * 0.50f;
        } else {
            const float diagSpec = 0.5f + 0.5f * fastWave((fx + fy) * 0.032f);
            const float glint = diagSpec * diagSpec * diagSpec * 18.0f + (facetEdge ? 8.0f : 0.0f);
            r += facetTone * 12.0f + glint + seedDelta * 0.5f;
            g += facetTone * 14.0f + glint * 1.05f + seedDelta * 0.5f;
            b += facetTone * 14.0f + glint * 1.10f + seedDelta * 0.5f;
        }
        break;
    }
    case MaterialId::Battery: {
        const float cellBand = fastWave(fy * 0.35f) * 6.0f;
        r += cellBand * 0.6f + seedDelta * 0.5f;
        g += cellBand * 0.9f + seedDelta * 0.5f;
        b += cellBand * 1.1f + seedDelta * 0.5f;
        break;
    }
    case MaterialId::Sand: {
        r += seedDelta * 1.08f;
        g += seedDelta * 0.95f;
        b += seedDelta * 0.68f;
        break;
    }
    case MaterialId::Salt:
    case MaterialId::Snow: {
        const float sparkle = (colorSeed > 236) ? 14.0f : 0.0f;
        r += seedDelta + sparkle;
        g += seedDelta + sparkle;
        b += seedDelta + sparkle;
        break;
    }
    case MaterialId::Water:
    case MaterialId::SaltWater:
    case MaterialId::Acid:
    case MaterialId::Oil:
    case MaterialId::Nitro: {
        bool onCrest = false;
        const float depth = computeLiquidDepth(matPtr, x, y, W, H, gMode, onCrest);
        const float wave = 0.5f + 0.5f * fastWave(fx * 0.17f - tPhase * 1.15f + seedNorm * 0.12f);

        if (mat == MaterialId::Water || mat == MaterialId::SaltWater) {
            const bool isSalt = (mat == MaterialId::SaltWater);
            const float sr = isSalt ? 66.0f : 52.0f;
            const float sg = isSalt ? 172.0f : 152.0f;
            const float sb = isSalt ? 232.0f : 238.0f;
            r = sr * std::exp((isSalt ? -0.062f : -0.075f) * depth) + seedDelta * 0.45f;
            g = sg * std::exp((isSalt ? -0.021f : -0.026f) * depth) + seedDelta * 0.50f;
            b = sb * std::max(0.46f, std::exp((isSalt ? -0.013f : -0.011f) * depth)) + seedDelta * 0.45f;

            // Subsurface caustic shimmer near top surface
            if (depth > 0.0f && depth <= 6.5f) {
                const float c = std::max(0.0f, fastWave(fx * 0.16f + fy * 0.11f - tPhase) *
                                               fastWave(fx * 0.10f - fy * 0.15f + tPhase * 0.7f));
                const float boost = c * (1.0f - depth * (1.0f / 7.0f)) * 26.0f;
                r += boost * 0.45f;
                g += boost * 0.85f;
                b += boost;
            }
            if (onCrest) {
                const float spd = std::abs(vx) + std::abs(vy) + 0.35f * (std::abs(airVx) + std::abs(airVy));
                const float foam = std::clamp((spd - 0.35f) * 0.55f + (wave - 0.65f) * 0.45f, 0.0f, 1.0f);
                // Crisp meniscus sky shimmer + aerated splash foam
                r += 24.0f + wave * 28.0f;
                g += 36.0f + wave * 36.0f;
                b += 42.0f + wave * 42.0f;
                r = r * (1.0f - foam * 0.72f) + 232.0f * (foam * 0.72f);
                g = g * (1.0f - foam * 0.72f) + 246.0f * (foam * 0.72f);
                b = b * (1.0f - foam * 0.72f) + 255.0f * (foam * 0.72f);
            }
        } else if (mat == MaterialId::Acid) {
            r = 136.0f * std::exp(-0.055f * depth) + seedDelta * 0.6f;
            g = 246.0f * std::max(0.55f, std::exp(-0.015f * depth)) + seedDelta * 0.6f;
            b = 72.0f * std::exp(-0.080f * depth) + seedDelta * 0.4f;
            const float fizz = std::max(0.0f, fastWave(fx * 0.22f + fy * 0.18f - tPhase * 1.4f)) * 18.0f;
            r += fizz * 0.6f;
            g += fizz;
            if (onCrest) {
                const float spd = std::abs(vx) + std::abs(vy);
                const float foam = std::clamp(0.25f + (spd - 0.3f) * 0.45f + wave * 0.25f, 0.0f, 0.85f);
                r = r * (1.0f - foam) + 215.0f * foam;
                g = g * (1.0f - foam) + 255.0f * foam;
                b = b * (1.0f - foam) + 165.0f * foam;
            }
        } else if (mat == MaterialId::Oil) {
            const float att = std::max(0.38f, std::exp(-0.060f * depth));
            r = 88.0f * att + seedDelta * 0.6f;
            g = 66.0f * att + seedDelta * 0.5f;
            b = 42.0f * att + seedDelta * 0.4f;
            if (onCrest) {
                // Subtle iridescent hydrocarbon sheen on surface
                r += 14.0f + wave * 18.0f;
                g += 12.0f + (1.0f - wave) * 16.0f;
                b += 16.0f + wave * 20.0f;
            }
        } else { // Nitro
            r = 96.0f * std::exp(-0.040f * depth) + seedDelta * 0.6f;
            g = 218.0f * std::max(0.55f, std::exp(-0.015f * depth)) + seedDelta * 0.6f;
            b = 156.0f * std::exp(-0.025f * depth) + seedDelta * 0.5f;
            if (onCrest) {
                r += 22.0f + wave * 18.0f;
                g += 28.0f + wave * 22.0f;
                b += 24.0f + wave * 20.0f;
            }
        }
        break;
    }
    case MaterialId::Lava: {
        bool onCrest = false;
        const float depth = computeLiquidDepth(matPtr, x, y, W, H, gMode, onCrest);
        const float churn = 0.5f + 0.5f * fastWave(fx * 0.11f + fy * 0.09f - tPhase * 0.45f + seedNorm * 0.35f);
        // Outer exposed/cooler skin develops dark basalt slag veins; deeper/hotter core glows bright magma
        const float heatFactor = std::clamp((tempK - 1050.0f) / 600.0f, 0.25f, 1.25f);
        const float depthHeat = std::min(1.0f, 0.52f + depth * 0.08f);
        const float skin = (onCrest ? 0.62f : 1.0f) * (0.55f + 0.45f * churn) * heatFactor * depthHeat;
        r = (210.0f + seedDelta * 0.8f) * std::clamp(skin * 1.15f, 0.30f, 1.35f);
        g = (76.0f + seedDelta * 0.6f) * std::clamp(skin * 1.10f, 0.18f, 1.40f);
        b = (20.0f + seedDelta * 0.3f) * std::clamp(skin, 0.15f, 1.25f);
        break;
    }
    case MaterialId::MoltenMetal:
    case MaterialId::MoltenGlass: {
        bool onCrest = false;
        const float depth = computeLiquidDepth(matPtr, x, y, W, H, gMode, onCrest);
        const float swirl = 0.88f + 0.16f * fastWave(fx * 0.14f - fy * 0.11f + tPhase * 0.5f);
        const float rim = onCrest ? 0.82f : std::min(1.08f, 0.92f + depth * 0.02f);
        r = (r + seedDelta * 0.8f) * swirl * rim;
        g = (g + seedDelta * 0.7f) * swirl * rim;
        b = (b + seedDelta * 0.5f) * swirl * rim;
        break;
    }
    case MaterialId::Fire: {
        // Hot yellow-gold flame heart when newly born, cooling toward crimson tips as life fades
        const float lifeNorm = (life > 0.0f) ? std::min(1.0f, life * (1.0f / 26.0f)) : 0.45f;
        const float core = 0.55f + 0.55f * lifeNorm;
        r = (r + seedDelta * 0.85f) * std::min(1.12f, core + 0.15f);
        g = (g + seedDelta * 0.70f) * (0.48f + 0.72f * lifeNorm);
        b = (b + seedDelta * 0.40f) * (0.35f + 0.75f * lifeNorm * lifeNorm);
        break;
    }
    default:
        r += seedDelta;
        g += seedDelta;
        b += seedDelta;
        break;
    }

    // Subtle 2.5D bevel & rim contrast on solid obstacles (and crisp rim on Glass vessels)
    if (props.isSolidObstacle()) {
        const int i = y * W + x;
        const bool nonSolidUp = (y > 0) && !getMaterial(fromIdx(matPtr[i - W])).isSolidObstacle();
        const bool nonSolidLeft = (x > 0) && !getMaterial(fromIdx(matPtr[i - 1])).isSolidObstacle();
        const bool nonSolidDown = (y < H - 1) && !getMaterial(fromIdx(matPtr[i + W])).isSolidObstacle();
        if (nonSolidUp) {
            r *= 1.13f;
            g *= 1.13f;
            b *= 1.13f;
            if (mat == MaterialId::Glass) {
                r += 24.0f; g += 28.0f; b += 32.0f;
            }
        } else if (nonSolidLeft) {
            r *= 1.07f;
            g *= 1.07f;
            b *= 1.07f;
            if (mat == MaterialId::Glass) {
                r += 16.0f; g += 20.0f; b += 24.0f;
            }
        } else if (nonSolidDown) {
            r *= 0.88f;
            g *= 0.88f;
            b *= 0.88f;
        }
    }

    outR = r;
    outG = g;
    outB = b;
}

} // namespace

void SimWorld::render(ViewMode mode) {
    const auto t0 = std::chrono::steady_clock::now();
    const int W = m_width;
    const int H = m_height;
    const GravityMode gMode = m_gravityMode;
    const float tPhase = static_cast<float>(m_stats.tick & 0xFFFFu) * 0.055f;

    const uint8_t* __restrict matPtr = m_mat.data();
    const uint8_t* __restrict seedPtr = m_colorSeed.data();
    const float* __restrict tempPtr = m_tempK.data();
    const float* __restrict lifePtr = m_life.data();
    const float* __restrict pressPtr = m_pressure.data();
    const float* __restrict vxPtr = m_vx.data();
    const float* __restrict vyPtr = m_vy.data();
    const float* __restrict airVxPtr = m_airVx.data();
    const float* __restrict airVyPtr = m_airVy.data();
    const float* __restrict chargePtr = m_charge.data();
    const float* __restrict lightRPtr = m_lightR.data();
    const float* __restrict lightGPtr = m_lightG.data();
    const float* __restrict lightBPtr = m_lightB.data();
    uint32_t* __restrict fbPtr = m_framebuffer.data();

    static const float kLogMinT = std::log(180.0f);
    static const float kInvLogSpanT = 1.0f / (std::log(3200.0f) - kLogMinT);

    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                const MaterialId mat = fromIdx(matPtr[i]);
                const auto& props = getMaterial(mat);

                if (mode == ViewMode::Physical) {
                    const float lr = lightRPtr[i];
                    const float lg = lightGPtr[i];
                    const float lb = lightBPtr[i];

                    float r = 0.0f, g = 0.0f, b = 0.0f;
                    if (mat == MaterialId::Air) {
                        const float shock = std::clamp((std::abs(pressPtr[i]) - 0.55f) * 0.18f, 0.0f, 0.40f);
                        r = 10.0f + lr * 40.0f + shock * 165.0f;
                        g = 13.0f + lg * 44.0f + shock * 205.0f;
                        b = 20.0f + lb * 60.0f + shock * 255.0f;
                    } else {
                        shadePhysicalMaterial(mat, props, matPtr, x, y, W, H,
                                              seedPtr[i], tempPtr[i], lifePtr[i],
                                              vxPtr[i], vyPtr[i], airVxPtr[i], airVyPtr[i],
                                              gMode, tPhase, r, g, b);

                        const float emSum = props.emissiveR + props.emissiveG + props.emissiveB;
                        const float selfLit = std::min(0.95f, emSum * 0.72f);
                        const float mulR = std::clamp(lr * 1.15f + 0.28f + selfLit, 0.26f, 2.40f);
                        const float mulG = std::clamp(lg * 1.15f + 0.28f + selfLit, 0.26f, 2.40f);
                        const float mulB = std::clamp(lb * 1.15f + 0.28f + selfLit, 0.26f, 2.40f);
                        r *= mulR;
                        g *= mulG;
                        b *= mulB;
                    }

                    // Physical blackbody thermal glow for heated matter & hot air
                    float bbR = 0.0f, bbG = 0.0f, bbB = 0.0f;
                    blackbodyGlow(tempPtr[i], bbR, bbG, bbB);
                    r += bbR * 210.0f;
                    g += bbG * 210.0f;
                    b += bbB * 210.0f;

                    // Continuous electrical potential & corona highlight
                    const float q = std::abs(chargePtr[i]);
                    if (q > 0.04f) {
                        const float qGlow = std::min(1.2f, (q - 0.04f) * 0.50f);
                        r += qGlow * 70.0f;
                        g += qGlow * 170.0f;
                        b += qGlow * 255.0f;
                    }

                    fbPtr[i] = packToneMappedBGRA(r, g, b);
                } else if (mode == ViewMode::Thermal) {
                    const float rawT = tempPtr[i];
                    const float tClamped = (rawT >= 180.0f) ? std::min(rawT, 3200.0f) : 180.0f;
                    const float norm = (std::log(tClamped) - kLogMinT) * kInvLogSpanT;
                    uint8_t cr = 0, cg = 0, cb = 0;
                    infernoColormap(norm, cr, cg, cb);

                    if (props.isSolidObstacle()) {
                        bool topLeftEdge = false, botRightEdge = false;
                        checkSolidEdges(matPtr, x, y, W, H, topLeftEdge, botRightEdge);
                        float fr = static_cast<float>(cr) * 0.86f;
                        float fg = static_cast<float>(cg) * 0.86f;
                        float fb = static_cast<float>(cb) * 0.86f;
                        if (((x + y) & 3) == 0) {
                            fr *= 0.90f;
                            fg *= 0.90f;
                            fb *= 0.90f;
                        }
                        if (topLeftEdge) {
                            fr = fr * 0.55f + 225.0f * 0.45f;
                            fg = fg * 0.55f + 232.0f * 0.45f;
                            fb = fb * 0.55f + 245.0f * 0.45f;
                        } else if (botRightEdge) {
                            const float lum = 0.30f * fr + 0.59f * fg + 0.11f * fb;
                            if (lum < 110.0f) {
                                fr = fr * 0.65f + 135.0f * 0.35f;
                                fg = fg * 0.65f + 145.0f * 0.35f;
                                fb = fb * 0.65f + 165.0f * 0.35f;
                            } else {
                                fr *= 0.48f;
                                fg *= 0.48f;
                                fb *= 0.48f;
                            }
                        }
                        fbPtr[i] = packBGRAf(fr, fg, fb);
                    } else {
                        fbPtr[i] = packBGRA(cr, cg, cb);
                    }
                } else if (mode == ViewMode::Pressure) {
                    if (props.isSolidObstacle()) {
                        fbPtr[i] = renderDiagnosticSolid(matPtr, x, y, W, H, seedPtr[i]);
                    } else {
                        const float p = std::isfinite(pressPtr[i]) ? pressPtr[i] : 0.0f;
                        const float mag = std::min(1.0f, std::log1p(std::abs(p) * 3.6f) / 3.75f);

                        // Detect sharp shockwave fronts across non-solid neighbors
                        float pL = p, pR = p, pU = p, pD = p;
                        if (x > 0 && !getMaterial(fromIdx(matPtr[i - 1])).isSolidObstacle()) pL = pressPtr[i - 1];
                        if (x < W - 1 && !getMaterial(fromIdx(matPtr[i + 1])).isSolidObstacle()) pR = pressPtr[i + 1];
                        if (y > 0 && !getMaterial(fromIdx(matPtr[i - W])).isSolidObstacle()) pU = pressPtr[i - W];
                        if (y < H - 1 && !getMaterial(fromIdx(matPtr[i + W])).isSolidObstacle()) pD = pressPtr[i + W];
                        const float gradP = 0.5f * (std::abs(pR - pL) + std::abs(pD - pU));
                        const float shockRim = std::min(0.45f, gradP * 0.24f);

                        float r = 0.0f, g = 0.0f, b = 0.0f;
                        const float m2 = mag * mag;
                        if (p >= 0.0f) {
                            r = 12.0f + std::min(1.0f, mag * 1.45f) * 243.0f;
                            g = 15.0f + m2 * 185.0f + std::max(0.0f, mag - 0.65f) * 140.0f;
                            b = 22.0f + m2 * m2 * 195.0f;
                        } else {
                            r = 12.0f + m2 * m2 * 185.0f;
                            g = 15.0f + mag * 180.0f + m2 * 55.0f;
                            b = 22.0f + std::min(1.0f, mag * 1.35f) * 233.0f;
                        }
                        r += shockRim * 165.0f;
                        g += shockRim * 175.0f;
                        b += shockRim * 185.0f;
                        fbPtr[i] = packBGRAf(r, g, b);
                    }
                } else if (mode == ViewMode::Velocity) {
                    if (props.isSolidObstacle()) {
                        fbPtr[i] = renderDiagnosticSolid(matPtr, x, y, W, H, seedPtr[i]);
                    } else {
                        const float vx = props.isMovable() ? vxPtr[i] : airVxPtr[i];
                        const float vy = props.isMovable() ? vyPtr[i] : airVyPtr[i];
                        const float speed = std::sqrt(vx * vx + vy * vy);
                        if (!(speed >= 1e-4f) || !std::isfinite(speed)) {
                            fbPtr[i] = packBGRA(12, 15, 22);
                        } else {
                            const float invSpd = 1.0f / speed;
                            const float ux = vx * invSpd;
                            const float uy = vy * invSpd;
                            const float mag = std::min(1.0f, std::log1p(speed * 3.0f) / 3.10f);

                            // Perceptually balanced 360-degree cosine color wheel (zero atan2/fmod)
                            const float cr = std::clamp(0.52f + 0.48f * ux, 0.02f, 1.0f);
                            const float cg = std::clamp(0.52f + 0.48f * (-0.5f * ux + 0.8660254f * uy), 0.02f, 1.0f);
                            const float cb = std::clamp(0.52f + 0.48f * (-0.5f * ux - 0.8660254f * uy), 0.02f, 1.0f);

                            const float whiteMix = std::max(0.0f, (mag - 0.72f) * 1.25f);
                            const float fr = cr * (1.0f - whiteMix) + whiteMix;
                            const float fg = cg * (1.0f - whiteMix) + whiteMix;
                            const float fb = cb * (1.0f - whiteMix) + whiteMix;

                            const float gain = std::min(1.05f, mag * (props.isMovable() ? 1.10f : 0.96f));
                            const float r = 12.0f + (fr * 245.0f - 12.0f) * gain;
                            const float g = 15.0f + (fg * 245.0f - 15.0f) * gain;
                            const float b = 22.0f + (fb * 245.0f - 22.0f) * gain;
                            fbPtr[i] = packBGRAf(r, g, b);
                        }
                    }
                } else { // ViewMode::EnergyLight
                    const float q = std::min(2.2f, std::abs(chargePtr[i]));
                    const float r = lightRPtr[i] * 215.0f + q * 48.0f;
                    const float g = lightGPtr[i] * 215.0f + q * 175.0f;
                    const float b = lightBPtr[i] * 215.0f + q * 255.0f;
                    if (props.isSolidObstacle()) {
                        bool topLeftEdge = false, botRightEdge = false;
                        checkSolidEdges(matPtr, x, y, W, H, topLeftEdge, botRightEdge);
                        float sr = r * 0.48f + 12.0f;
                        float sg = g * 0.48f + 14.0f;
                        float sb = b * 0.48f + 18.0f;
                        if (topLeftEdge) {
                            sr += 46.0f; sg += 52.0f; sb += 64.0f;
                        } else if (botRightEdge) {
                            sr += 22.0f; sg += 26.0f; sb += 34.0f;
                        }
                        fbPtr[i] = packToneMappedBGRA(sr, sg, sb);
                    } else {
                        fbPtr[i] = packToneMappedBGRA(r, g, b);
                    }
                }
            }
        }
    });

    const auto t1 = std::chrono::steady_clock::now();
    m_stats.renderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace sim
