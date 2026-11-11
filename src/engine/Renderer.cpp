#include "sim/MaterialOptics.hpp"
#include "sim/SimWorld.hpp"

#include <algorithm>
#include <array>
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

inline MaterialId sampleMatClamped(const uint8_t *mat, int x, int y, int W, int H) noexcept {
    const int cx = std::clamp(x, 0, W - 1);
    const int cy = std::clamp(y, 0, H - 1);
    return fromIdx(mat[cy * W + cx]);
}

inline float cellOccupancy(MaterialId id) noexcept {
    switch (getMaterial(id).phase) {
    case PhaseState::StaticSolid:
        return 1.00f;
    case PhaseState::Powder:
        return 0.78f;
    case PhaseState::Liquid:
        return 0.55f;
    default:
        return 0.00f;
    }
}

// Layered slate-indigo atmospheric chamber backdrop with subtle micro-strata and 32px/128px lab grid
inline void computeBackdrop(int x, int y, int W, int H, float &outR, float &outG, float &outB) noexcept {
    const float invW = (W > 1) ? (1.0f / static_cast<float>(W - 1)) : 0.0f;
    const float invH = (H > 1) ? (1.0f / static_cast<float>(H - 1)) : 0.0f;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float u = fx * invW * 2.0f - 1.0f;
    const float v = fy * invH;

    // Deep slate-indigo gradient with gentle radial chamber depth
    const float vig = 1.0f - 0.14f * u * u;
    float r = (8.5f + 5.5f * v) * vig;
    float g = (11.5f + 6.5f * v) * vig;
    float b = (19.5f + 10.5f * v) * vig;

    // Subtle atmospheric strata haze
    const float strata =
        fastWave(fy * 0.036f + 0.32f * fastWave(fx * 0.014f)) + 0.45f * fastWave(fy * 0.092f - fx * 0.011f);
    r += strata * 0.85f;
    g += strata * 1.05f;
    b += strata * 1.60f;

    // Faint scientific chamber grid (32px minor, 128px major, subtle tick intersections)
    const int gx32 = x & 31;
    const int gy32 = y & 31;
    const bool minorLine = (gx32 == 0) || (gy32 == 0);
    const bool majorLine = ((x & 127) == 0) || ((y & 127) == 0);
    if (majorLine) {
        r += 3.2f;
        g += 4.2f;
        b += 6.8f;
    } else if (minorLine) {
        r += 1.4f;
        g += 1.9f;
        b += 3.2f;
    }
    if (gx32 == 0 && gy32 == 0) {
        r += 1.8f;
        g += 2.4f;
        b += 3.8f;
    }

    // Fine dither to prevent 8-bit gradient banding
    const float dither = static_cast<float>(hash2D(x, y) & 7u) * 0.16f - 0.56f;
    outR = std::max(4.0f, r + dither);
    outG = std::max(5.0f, g + dither);
    outB = std::max(9.0f, b + dither);
}

inline void normalizeNormal(float nx, float ny, float nz, float bumpX, float bumpY, float &snx, float &sny,
                            float &snz) noexcept {
    const float bnx = nx + bumpX;
    const float bny = ny + bumpY;
    const float invL = 1.0f / std::sqrt(bnx * bnx + bny * bny + nz * nz);
    snx = bnx * invL;
    sny = bny * invL;
    snz = nz * invL;
}

inline void computeAirLightBackdrop(int x, int y, int W, int H, float sunVis, float lr, float lg, float lb, float &outR,
                                    float &outG, float &outB) noexcept {
    float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f;
    computeBackdrop(x, y, W, H, bgR, bgG, bgB);
    const float bgSun = 0.80f + 0.20f * sunVis;
    outR = bgR * bgSun + lr * 40.0f;
    outG = bgG * bgSun + lg * 44.0f;
    outB = bgB * bgSun + lb * 60.0f;
}

inline void estimateNormalAndAO(const uint8_t *mat, int x, int y, int W, int H, const MaterialProps &selfProps,
                                float &outNx, float &outNy, float &outNz, float &outAO, bool &outTopEdge,
                                bool &outLeftEdge, bool &outBotEdge) noexcept {
    const MaterialId mTL = sampleMatClamped(mat, x - 1, y - 1, W, H);
    const MaterialId mT = sampleMatClamped(mat, x, y - 1, W, H);
    const MaterialId mTR = sampleMatClamped(mat, x + 1, y - 1, W, H);
    const MaterialId mL = sampleMatClamped(mat, x - 1, y, W, H);
    const MaterialId mR = sampleMatClamped(mat, x + 1, y, W, H);
    const MaterialId mBL = sampleMatClamped(mat, x - 1, y + 1, W, H);
    const MaterialId mB = sampleMatClamped(mat, x, y + 1, W, H);
    const MaterialId mBR = sampleMatClamped(mat, x + 1, y + 1, W, H);

    const float oTL = cellOccupancy(mTL);
    const float oT = cellOccupancy(mT);
    const float oTR = cellOccupancy(mTR);
    const float oL = cellOccupancy(mL);
    const float oR = cellOccupancy(mR);
    const float oBL = cellOccupancy(mBL);
    const float oB = cellOccupancy(mB);
    const float oBR = cellOccupancy(mBR);

    // 3x3 Sobel occupancy gradient -> outward 2.5D surface normal
    const float dx = (oTR + 2.0f * oR + oBR) - (oTL + 2.0f * oL + oBL);
    const float dy = (oBL + 2.0f * oB + oBR) - (oTL + 2.0f * oT + oTR);
    outNx = -dx;
    outNy = -dy;
    outNz = 1.65f;

    const bool sTL = getMaterial(mTL).isSolidObstacle();
    const bool sT = getMaterial(mT).isSolidObstacle();
    const bool sTR = getMaterial(mTR).isSolidObstacle();
    const bool sL = getMaterial(mL).isSolidObstacle();
    const bool sR = getMaterial(mR).isSolidObstacle();
    const bool sBL = getMaterial(mBL).isSolidObstacle();
    const bool sB = getMaterial(mB).isSolidObstacle();
    const bool sBR = getMaterial(mBR).isSolidObstacle();

    outTopEdge = (y > 0) && !sT;
    outLeftEdge = (x > 0) && !sL;
    outBotEdge = (y < H - 1) && !sB;

    const int solid3x3 = static_cast<int>(sTL) + static_cast<int>(sT) + static_cast<int>(sTR) + static_cast<int>(sL) +
                         static_cast<int>(sR) + static_cast<int>(sBL) + static_cast<int>(sB) + static_cast<int>(sBR);

    if (selfProps.isSolidObstacle()) {
        if (sT && sL && sR && sB) {
            // Interior solid cell: gently darken deeper cores so outer bevels & relief pop
            const int wideSolid =
                static_cast<int>(getMaterial(sampleMatClamped(mat, x, y - 2, W, H)).isSolidObstacle()) +
                static_cast<int>(getMaterial(sampleMatClamped(mat, x, y + 2, W, H)).isSolidObstacle()) +
                static_cast<int>(getMaterial(sampleMatClamped(mat, x - 2, y, W, H)).isSolidObstacle()) +
                static_cast<int>(getMaterial(sampleMatClamped(mat, x + 2, y, W, H)).isSolidObstacle());
            outAO = 0.94f - static_cast<float>(wideSolid) * 0.020f;
        } else {
            // Exposed solid edge/corner: convex tips open bright, tight concave pockets softly occluded
            outAO = std::clamp(1.04f - static_cast<float>(solid3x3) * 0.030f, 0.78f, 1.03f);
        }
    } else {
        // Soft contact AO from nearby StaticSolid obstacles onto powders/liquids
        const float wideSolid =
            0.5f * (static_cast<float>(getMaterial(sampleMatClamped(mat, x, y - 2, W, H)).isSolidObstacle()) +
                    static_cast<float>(getMaterial(sampleMatClamped(mat, x - 2, y - 2, W, H)).isSolidObstacle()) +
                    static_cast<float>(getMaterial(sampleMatClamped(mat, x + 2, y - 2, W, H)).isSolidObstacle()) +
                    static_cast<float>(getMaterial(sampleMatClamped(mat, x - 2, y, W, H)).isSolidObstacle()) +
                    static_cast<float>(getMaterial(sampleMatClamped(mat, x + 2, y, W, H)).isSolidObstacle()));
        outAO = std::clamp(1.0f - (static_cast<float>(solid3x3) + wideSolid) * 0.032f, 0.70f, 1.0f);
    }
}

// Dual-ray cone march toward upper-left sun for smooth zero-noise soft shadows
inline float computeSunShadow(const uint8_t *mat, int x, int y, int W, int H, bool startIsSolid) noexcept {
    constexpr std::array<float, 9> kStepDist = {1.2f, 2.5f, 4.2f, 6.4f, 9.2f, 12.6f, 16.8f, 21.8f, 28.0f};
    constexpr float kRayDir[2][2] = {{-0.44f, -0.898f}, {-0.62f, -0.785f}};

    const bool topExposed = (y > 0) && !getMaterial(fromIdx(mat[(y - 1) * W + x])).isSolidObstacle();
    const bool initOpen = !startIsSolid || topExposed;

    float totalTrans = 0.0f;
    for (int r = 0; r < 2; ++r) {
        const float rx = kRayDir[r][0];
        const float ry = kRayDir[r][1];
        float trans = 1.0f;
        bool seenOpen = initOpen;

        for (float d : kStepDist) {
            const int sx = static_cast<int>(std::floor(static_cast<float>(x) + rx * d + 0.5f));
            const int sy = static_cast<int>(std::floor(static_cast<float>(y) + ry * d + 0.5f));
            if (sx < 0 || sx >= W || sy < 0 || sy >= H) {
                break; // Exited to open sky
            }
            const MaterialId hm = fromIdx(mat[sy * W + sx]);
            if (!seenOpen) {
                if (!getMaterial(hm).isSolidObstacle()) {
                    seenOpen = true;
                }
                continue;
            }
            const float sop = getMaterialOptics(hm).shadowOpacity;
            if (sop > 0.005f) {
                trans *= std::max(0.0f, 1.0f - sop);
                if (trans < 0.04f) {
                    trans = 0.0f;
                    break;
                }
            }
        }
        totalTrans += trans;
    }
    return 0.5f * totalTrans;
}

inline void checkSolidEdges(const uint8_t *mat, int x, int y, int W, int H, bool &topLeftEdge,
                            bool &botRightEdge) noexcept {
    const int i = y * W + x;
    const bool openL = (x > 0) && !getMaterial(fromIdx(mat[i - 1])).isSolidObstacle();
    const bool openU = (y > 0) && !getMaterial(fromIdx(mat[i - W])).isSolidObstacle();
    const bool openR = (x < W - 1) && !getMaterial(fromIdx(mat[i + 1])).isSolidObstacle();
    const bool openD = (y < H - 1) && !getMaterial(fromIdx(mat[i + W])).isSolidObstacle();
    topLeftEdge = openL || openU;
    botRightEdge = openR || openD;
}

inline uint32_t renderDiagnosticSolid(const uint8_t *mat, int x, int y, int W, int H, uint8_t seed) noexcept {
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
inline float computeLiquidDepth(const uint8_t *mat, int x, int y, int W, int H, GravityMode gMode,
                                bool &onCrest) noexcept {
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

// Compute GGX-lite specular & Schlick Fresnel term using MaterialOptics
inline void computeSpecularFresnel(const MaterialOptics &optics, float baseR, float baseG, float baseB, float nx,
                                   float ny, float nz, float sunVis, float ao, float &outSpecR, float &outSpecG,
                                   float &outSpecB, float &outFresnel) noexcept {
    // Normalized directional sun vector L = (-0.48, -0.66, 0.58) and half-vector H = (-0.270, -0.371, 0.888)
    constexpr float kHx = -0.270f;
    constexpr float kHy = -0.371f;
    constexpr float kHz = 0.888f;

    const float ndotv = std::clamp(nz, 0.05f, 1.0f);
    const float oneMinusV = 1.0f - ndotv;
    const float omv2 = oneMinusV * oneMinusV;
    const float schlickPow = omv2 * omv2 * oneMinusV;

    float f0 = 0.04f;
    if (optics.ior > 1.01f) {
        const float num = optics.ior - 1.0f;
        const float den = optics.ior + 1.0f;
        const float r0 = num / den;
        f0 = r0 * r0;
    }
    outFresnel = f0 + (1.0f - f0) * schlickPow;

    if (optics.specularStrength <= 0.01f) {
        outSpecR = 0.0f;
        outSpecG = 0.0f;
        outSpecB = 0.0f;
        return;
    }

    const float ndoth = std::max(0.0f, nx * kHx + ny * kHy + nz * kHz);
    const float rough = std::clamp(optics.roughness, 0.06f, 1.0f);
    const float alpha = rough * rough;
    const float alpha2 = alpha * alpha;
    const float denom = ndoth * ndoth * (alpha2 - 1.0f) + 1.0f;
    const float ggxD = alpha2 / (3.14159265f * denom * denom + 1e-5f);

    const float specLobe = std::min(1.65f, ggxD * 0.24f) * optics.specularStrength * (0.18f + 0.82f * sunVis) * ao;
    const float albR = std::clamp(baseR * (1.0f / 255.0f), 0.0f, 1.0f);
    const float albG = std::clamp(baseG * (1.0f / 255.0f), 0.0f, 1.0f);
    const float albB = std::clamp(baseB * (1.0f / 255.0f), 0.0f, 1.0f);
    const float m = optics.metallic;

    outSpecR = specLobe * 195.0f * ((1.0f - m) * 1.00f + m * albR);
    outSpecG = specLobe * 202.0f * ((1.0f - m) * 1.00f + m * albG);
    outSpecB = specLobe * 215.0f * ((1.0f - m) * 1.00f + m * albB);
}

inline void shadePhysicalMaterial(MaterialId mat, const MaterialProps &props, const uint8_t *matPtr, int x, int y,
                                  int W, int H, uint8_t colorSeed, float tempK, float life, float charge, float vx,
                                  float vy, float airVx, float airVy, float lr, float lg, float lb, GravityMode gMode,
                                  float tPhase, float &outR, float &outG, float &outB, float &outBbScale,
                                  float &outChargeScale) noexcept {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float seedNorm = (static_cast<float>(colorSeed) - 128.0f) * (1.0f / 128.0f);
    const float seedDelta = seedNorm * static_cast<float>(props.colorVariance);
    const auto &optics = getMaterialOptics(mat);

    outBbScale = 1.0f;
    outChargeScale = 1.0f;

    // 2.5D occupancy normal, solid ambient occlusion & directional sun shadow
    float nx = 0.0f, ny = 0.0f, nz = 1.0f, ao = 1.0f;
    bool topEdge = false, leftEdge = false, botEdge = false;
    float sunVis = 1.0f;

    const bool isGasOrEnergy = (props.phase == PhaseState::EmptyGas || props.phase == PhaseState::Energy);
    if (!isGasOrEnergy) {
        estimateNormalAndAO(matPtr, x, y, W, H, props, nx, ny, nz, ao, topEdge, leftEdge, botEdge);
        sunVis = computeSunShadow(matPtr, x, y, W, H, props.isSolidObstacle());
    } else {
        sunVis = computeSunShadow(matPtr, x, y, W, H, false);
    }

    float bumpX = 0.0f;
    float bumpY = 0.0f;
    float r = static_cast<float>(props.baseR);
    float g = static_cast<float>(props.baseG);
    float b = static_cast<float>(props.baseB);

    bool handledComposite = false;

    switch (mat) {
    case MaterialId::Stone: {
        // Warped multi-octave sedimentary strata + micro-pits + crystalline quartz specks
        const float warp = 0.40f * fastWave(fx * 0.030f + fy * 0.015f) + 0.16f * fastWave(fx * 0.072f - fy * 0.032f);
        const float strata = fastWave(fy * 0.112f + warp) + 0.36f * fastWave(fy * 0.285f + fx * 0.022f + warp * 0.5f);
        const uint32_t h = hash2D(x, y);
        const float pit = ((h & 31u) == 0u) ? -14.0f : 0.0f;
        const float quartz = ((colorSeed > 238) || ((h & 63u) == 17u)) ? 16.0f : 0.0f;
        bumpY = 0.22f * fastWave(fy * 0.112f + warp + 0.25f);
        r += strata * 10.5f + seedDelta * 0.72f + pit + quartz;
        g += strata * 9.5f + seedDelta * 0.68f + pit + quartz;
        b += strata * 7.2f + seedDelta * 0.68f + pit + quartz * 1.15f;
        break;
    }
    case MaterialId::Bedrock: {
        // Interlocking staggered dark basalt ashlar masonry with beveled mortar joints
        constexpr int kBH = 9;
        constexpr int kBW = 14;
        const int rowIdx = y / kBH;
        const int by = y - rowIdx * kBH;
        const int sx = x + ((rowIdx & 1) * (kBW / 2));
        const int colIdx = sx / kBW;
        const int bx = sx - colIdx * kBW;
        const uint32_t bh = hash2D(colIdx, rowIdx);
        const float blockTone = (static_cast<float>(bh & 31u) - 15.5f) * 0.45f;

        float bevel = 0.0f;
        if (bx == 0 || by == 0) {
            bevel = -13.5f;
            if (bx == 0) bumpX += 0.35f;
            if (by == 0) bumpY += 0.35f;
        } else if (bx == 1 || by == 1) {
            bevel = 8.5f;
            if (bx == 1) bumpX -= 0.32f;
            if (by == 1) bumpY -= 0.32f;
        } else if (bx == kBW - 1 || by == kBH - 1) {
            bevel = -6.0f;
            if (bx == kBW - 1) bumpX += 0.25f;
            if (by == kBH - 1) bumpY += 0.25f;
        }

        r += blockTone + bevel + seedDelta * 0.55f;
        g += blockTone * 1.05f + bevel + seedDelta * 0.58f;
        b += blockTone * 1.18f + bevel * 1.08f + seedDelta * 0.65f;
        break;
    }
    case MaterialId::Sand: {
        // Multi-tone quartz/gold/ochre/mica grains driven by Lagrangian colorSeed
        float toneR = seedDelta * 1.08f;
        float toneG = seedDelta * 0.95f;
        float toneB = seedDelta * 0.68f;
        if (colorSeed > 230) {
            toneR += 16.0f;
            toneG += 15.0f;
            toneB += 12.0f;
        } else if (colorSeed < 32) {
            toneR += 12.0f;
            toneG += 3.0f;
            toneB -= 12.0f;
        } else if ((colorSeed & 31u) == 7u) {
            toneR -= 22.0f;
            toneG -= 20.0f;
            toneB -= 16.0f;
        }
        bumpX = seedNorm * 0.20f;
        bumpY = (static_cast<float>(colorSeed & 15u) - 7.5f) * 0.024f;
        r += toneR;
        g += toneG;
        b += toneB;
        break;
    }
    case MaterialId::Salt: {
        const float glitter = (colorSeed > 228) ? (14.0f + 22.0f * sunVis) : 0.0f;
        bumpX = seedNorm * 0.16f;
        r += seedDelta * 0.85f + glitter;
        g += seedDelta * 0.88f + glitter;
        b += seedDelta * 0.95f + glitter * 1.05f;
        break;
    }
    case MaterialId::Snow: {
        const float glitter = (colorSeed > 234) ? (14.0f + 20.0f * sunVis) : 0.0f;
        const float coolShade = (1.0f - sunVis) * 8.0f;
        r += seedDelta * 0.65f + glitter - coolShade * 1.3f;
        g += seedDelta * 0.70f + glitter - coolShade * 0.4f;
        b += seedDelta * 0.78f + glitter + coolShade * 0.9f;
        break;
    }
    case MaterialId::Wood: {
        // Warped growth rings, longitudinal fibers, periodic knot whorls, and thermal char
        const float warp = 0.55f * fastWave(fx * 0.022f + fy * 0.011f);
        const float rings = fastWave(fy * 0.22f + fx * 0.016f + warp) + 0.35f * fastWave(fy * 0.66f + warp * 0.5f);
        const float fibers = 0.26f * fastWave(fx * 0.44f + fy * 0.04f + warp);
        const float knotWave = fastWave(fx * 0.031f + 0.17f) * fastWave(fy * 0.047f + 0.31f);
        const float knot = (knotWave > 0.72f) ? -18.0f * (knotWave - 0.72f) * 3.5f : 0.0f;
        const float delta = (rings + fibers) * 12.5f + seedDelta * 0.50f + knot;
        bumpY = 0.18f * rings;
        r += delta * 1.15f;
        g += delta * 0.82f;
        b += delta * 0.45f;

        if (tempK > 410.0f) {
            const float charT = std::clamp((tempK - 410.0f) / 135.0f, 0.0f, 1.0f);
            r = r * (1.0f - charT * 0.86f) + 24.0f * (charT * 0.86f);
            g = g * (1.0f - charT * 0.88f) + 20.0f * (charT * 0.88f);
            b = b * (1.0f - charT * 0.88f) + 18.0f * (charT * 0.88f);
            if (tempK > 490.0f && rings > 0.15f) {
                const float ember = std::clamp((tempK - 490.0f) / 60.0f, 0.0f, 1.0f);
                r += ember * 45.0f;
                g += ember * 12.0f;
            }
        }
        break;
    }
    case MaterialId::Plant: {
        // Organic leaf-lobe clusters + midrib veins + sunlit chlorophyll translucency
        const float lobeA = fastWave((fx + fy) * 0.145f + seedNorm * 0.18f);
        const float lobeB = fastWave((fx - fy) * 0.185f - seedNorm * 0.14f);
        const float cluster = 0.5f * (lobeA + lobeB);
        const float vein = std::max(0.0f, 1.0f - std::abs(fastWave(fx * 0.22f + fy * 0.09f)) * 3.5f);
        const float sss = sunVis * std::max(0.0f, cluster) * 14.0f;
        bumpX = lobeA * 0.18f;
        bumpY = lobeB * 0.18f;
        r += cluster * 10.5f - vein * 6.0f + seedDelta * 0.62f + sss * 0.65f;
        g += cluster * 19.0f + vein * 8.0f + seedDelta * 0.85f + sss;
        b += cluster * 6.0f - vein * 4.0f + seedDelta * 0.52f + sss * 0.25f;
        if (tempK > 395.0f) {
            const float wilt = std::clamp((tempK - 395.0f) / 110.0f, 0.0f, 1.0f);
            r = r * (1.0f - wilt * 0.65f) + 64.0f * (wilt * 0.65f);
            g = g * (1.0f - wilt * 0.72f) + 42.0f * (wilt * 0.72f);
            b = b * (1.0f - wilt * 0.75f) + 22.0f * (wilt * 0.75f);
        }
        break;
    }
    case MaterialId::Metal: {
        // Anisotropic diagonal sheen + fine brushed horizontal streaks + 16px panel seams & rivets
        const float diag = 0.5f + 0.5f * fastWave(fx * 0.023f + fy * 0.041f);
        const float sheen = diag * diag * diag * 28.0f;
        const float brush = fastWave(fy * 0.64f + fx * 0.032f) * 5.5f;
        const int px16 = x & 15;
        const int py16 = y & 15;
        float seam = 0.0f;
        if (px16 == 0 || py16 == 0) {
            seam = -16.0f;
            if (px16 == 0) bumpX += 0.35f;
            if (py16 == 0) bumpY += 0.35f;
        } else if (px16 == 1 || py16 == 1) {
            seam = 9.0f;
            if (px16 == 1) bumpX -= 0.28f;
            if (py16 == 1) bumpY -= 0.28f;
        } else if ((px16 == 3 || px16 == 13) && (py16 == 3 || py16 == 13)) {
            seam = 12.0f;
        }
        bumpY += fastWave(fy * 0.64f) * 0.08f;
        r += sheen * 0.90f + brush + seam + seedDelta * 0.55f;
        g += sheen * 0.98f + brush + seam + seedDelta * 0.55f;
        b += sheen * 1.12f + brush + seam + seedDelta * 0.65f;

        // Thermal temper oxidation tint when heated (straw -> violet -> steel-blue)
        if (tempK > 480.0f && tempK < 920.0f) {
            const float ht = std::clamp((tempK - 480.0f) / 340.0f, 0.0f, 1.0f);
            const float temper = std::sin(ht * 3.14159265f);
            r += temper * (24.0f * (1.0f - ht) - 8.0f * ht);
            g += temper * (8.0f - 22.0f * ht);
            b += temper * (38.0f * ht - 12.0f * (1.0f - ht));
        }
        outChargeScale = 0.72f;
        break;
    }
    case MaterialId::Battery: {
        // Brushed metallic casing + plate seams + charge-reactive glowing cyan bus-bar indicator
        const int px16 = x & 15;
        const int py16 = y & 15;
        const float brush = fastWave(fy * 0.55f + fx * 0.03f) * 4.5f;
        const float qNorm = std::clamp(std::abs(charge) * 0.25f, 0.0f, 1.0f);
        const bool isIndicatorStrip = ((x & 3) == 1 || (x & 3) == 2) && (py16 >= 3 && py16 <= 12);

        float casingR = 54.0f + brush + seedDelta * 0.45f;
        float casingG = 66.0f + brush + seedDelta * 0.45f;
        float casingB = 82.0f + brush + seedDelta * 0.55f;
        if (px16 == 0 || py16 == 0) {
            casingR -= 14.0f;
            casingG -= 14.0f;
            casingB -= 14.0f;
            if (px16 == 0) bumpX += 0.32f;
            if (py16 == 0) bumpY += 0.32f;
        } else if (px16 == 1 || py16 == 1) {
            casingR += 10.0f;
            casingG += 12.0f;
            casingB += 15.0f;
        }

        if (isIndicatorStrip) {
            const float pulse = 0.85f + 0.15f * fastWave(fy * 0.16f - tPhase * 1.4f);
            const float active = (0.18f + 0.82f * qNorm) * pulse;
            r = casingR * (1.0f - active * 0.75f) + 52.0f * active;
            g = casingG * (1.0f - active * 0.75f) + 210.0f * active;
            b = casingB * (1.0f - active * 0.75f) + 255.0f * active;
            outChargeScale = 1.15f;
        } else {
            r = casingR + qNorm * 8.0f;
            g = casingG + qNorm * 24.0f;
            b = casingB + qNorm * 38.0f;
            outChargeScale = 0.38f;
        }
        break;
    }
    case MaterialId::Obsidian: {
        // Conchoidal volcanic fracture arcs + glassy faceted glints
        const float warp = 0.52f * fastWave((fx + fy) * 0.042f);
        const float arc = fastWave(fy * 0.155f - fx * 0.072f + warp) + 0.35f * fastWave((fx - fy) * 0.11f + warp);
        const float glint = (colorSeed > 232) ? 14.0f : 0.0f;
        bumpX = 0.22f * arc;
        bumpY = -0.18f * arc;
        r += arc * 9.5f + seedDelta * 0.65f + glint * 0.75f;
        g += arc * 6.5f + seedDelta * 0.55f + glint * 0.50f;
        b += arc * 15.5f + seedDelta * 0.85f + glint * 1.35f;
        break;
    }
    case MaterialId::Ice:
    case MaterialId::Glass: {
        // Rotated crystal facets + refracted Beer-Lambert backdrop + Fresnel edge glow
        const int u = x * 3 + y * 2;
        const int v = y * 3 - x * 2;
        const int cellD = (mat == MaterialId::Ice) ? 18 : 26;
        const int cu = (u >= 0) ? (u / cellD) : ((u - cellD + 1) / cellD);
        const int cv = (v >= 0) ? (v / cellD) : ((v - cellD + 1) / cellD);
        const int fu = u - cu * cellD;
        const int fv = v - cv * cellD;
        const float facetTone = static_cast<float>(hash2D(cu, cv) & 255u) * (1.0f / 255.0f) - 0.5f;
        const bool facetEdge = (fu <= 1 || fv <= 1);
        if (facetEdge) {
            bumpX += (fu <= 1) ? 0.28f : 0.0f;
            bumpY += (fv <= 1) ? 0.28f : 0.0f;
        }

        // Normalize normal with facet micro-bump for refraction & Fresnel
        float snx = 0.0f, sny = 0.0f, snz = 1.0f;
        normalizeNormal(nx, ny, nz, bumpX, bumpY, snx, sny, snz);

        const float refrShift = (mat == MaterialId::Glass) ? 6.5f : 4.5f;
        const int rx = std::clamp(x + static_cast<int>(std::lround(snx * refrShift)), 0, W - 1);
        const int ry = std::clamp(y + static_cast<int>(std::lround(sny * refrShift)), 0, H - 1);
        float airBgR = 0.0f, airBgG = 0.0f, airBgB = 0.0f;
        computeAirLightBackdrop(rx, ry, W, H, sunVis, lr, lg, lb, airBgR, airBgG, airBgB);

        const float sunDiff = std::max(0.0f, snx * -0.48f + sny * -0.66f + snz * 0.58f);
        const float litMul = ao * (0.28f + sunVis * (0.08f + 0.44f * sunDiff)) + (lr + lg + lb) * 0.35f;

        float specR = 0.0f, specG = 0.0f, specB = 0.0f, fresnel = 0.0f;
        computeSpecularFresnel(optics, r, g, b, snx, sny, snz, sunVis, ao, specR, specG, specB, fresnel);

        if (mat == MaterialId::Ice) {
            const float rim = facetEdge ? 22.0f : 0.0f;
            const float frost = (colorSeed > 236) ? 12.0f : 0.0f;
            const float bodyR = (r + facetTone * 22.0f + rim * 0.85f + frost + seedDelta * 0.55f) *
                                std::clamp(0.32f + litMul * 0.68f, 0.28f, 1.45f);
            const float bodyG = (g + facetTone * 24.0f + rim * 1.00f + frost + seedDelta * 0.55f) *
                                std::clamp(0.32f + litMul * 0.68f, 0.28f, 1.45f);
            const float bodyB = (b + facetTone * 20.0f + rim * 1.05f + frost + seedDelta * 0.50f) *
                                std::clamp(0.32f + litMul * 0.68f, 0.28f, 1.45f);
            // Ice transmits refracted backdrop through translucent crystal thickness + Fresnel rim
            const float transR = std::exp(-optics.sigmaR * 3.0f) * 0.55f;
            const float transG = std::exp(-optics.sigmaG * 3.0f) * 0.55f;
            const float transB = std::exp(-optics.sigmaB * 3.0f) * 0.55f;
            r = airBgR * transR + bodyR * (1.0f - transR * 0.65f) + fresnel * 95.0f + specR;
            g = airBgG * transG + bodyG * (1.0f - transG * 0.65f) + fresnel * 135.0f + specG;
            b = airBgB * transB + bodyB * (1.0f - transB * 0.65f) + fresnel * 165.0f + specB;
            if (topEdge || leftEdge) {
                const float edgeBoost = topEdge ? 26.0f : 16.0f;
                r += edgeBoost * 0.78f;
                g += edgeBoost * 0.96f;
                b += edgeBoost * 1.12f;
            }
        } else {
            const float diagSpec = 0.5f + 0.5f * fastWave((fx + fy) * 0.032f);
            const float glint = diagSpec * diagSpec * diagSpec * 16.0f + (facetEdge ? 7.0f : 0.0f);
            const float bodyR = (r + facetTone * 10.0f + glint + seedDelta * 0.45f) * std::clamp(litMul, 0.30f, 1.35f);
            const float bodyG =
                (g + facetTone * 12.0f + glint * 1.05f + seedDelta * 0.45f) * std::clamp(litMul, 0.30f, 1.35f);
            const float bodyB =
                (b + facetTone * 12.0f + glint * 1.10f + seedDelta * 0.45f) * std::clamp(litMul, 0.30f, 1.35f);
            const float transR = std::exp(-optics.sigmaR * 2.5f) * optics.translucency;
            const float transG = std::exp(-optics.sigmaG * 2.5f) * optics.translucency;
            const float transB = std::exp(-optics.sigmaB * 2.5f) * optics.translucency;
            const float rimGlow = fresnel * 130.0f + (topEdge ? 30.0f : (leftEdge ? 20.0f : 0.0f));
            r = airBgR * transR + bodyR * 0.16f + rimGlow * 0.80f + specR;
            g = airBgG * transG + bodyG * 0.16f + rimGlow * 0.95f + specG;
            b = airBgB * transB + bodyB * 0.16f + rimGlow * 1.08f + specB;
        }
        handledComposite = true;
        break;
    }
    case MaterialId::Gunpowder: {
        // Coarse matte charcoal with scattered pale saltpeter & muted sulfur specks
        float grain = seedDelta * 0.85f;
        float gR = r + grain;
        float gG = g + grain;
        float gB = b + grain;
        if (colorSeed < 22) {
            gR += 24.0f;
            gG += 24.0f;
            gB += 26.0f;
        } else if (colorSeed > 234) {
            gR += 20.0f;
            gG += 16.0f;
            gB -= 8.0f;
        }
        r = gR;
        g = gG;
        b = gB;
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
        const float rippleSlope = fastWave(fx * 0.21f - tPhase * 1.35f + fy * 0.08f);
        bumpX = rippleSlope * (onCrest ? 0.42f : 0.18f);
        bumpY = (onCrest ? -0.32f : 0.08f) * fastWave(fx * 0.13f + tPhase * 0.9f);

        float snx = 0.0f, sny = 0.0f, snz = 1.0f;
        normalizeNormal(nx, ny, nz, bumpX, bumpY, snx, sny, snz);

        // Sample refracted backdrop behind the liquid column
        const float shift = 5.5f * optics.refractStrength;
        const int rx = std::clamp(x + static_cast<int>(std::lround(snx * shift)), 0, W - 1);
        const int ry = std::clamp(y + static_cast<int>(std::lround(sny * shift)), 0, H - 1);
        float airBgR = 0.0f, airBgG = 0.0f, airBgB = 0.0f;
        computeAirLightBackdrop(rx, ry, W, H, sunVis, lr, lg, lb, airBgR, airBgG, airBgB);

        const float optD = depth + 1.5f;
        const float transR = std::exp(-optics.sigmaR * optD);
        const float transG = std::exp(-optics.sigmaG * optD);
        const float transB = std::exp(-optics.sigmaB * optD);

        const float sunDiff = std::max(0.0f, snx * -0.48f + sny * -0.66f + snz * 0.58f);
        const float litR = ao * (0.28f + sunVis * (0.10f + 0.42f * sunDiff)) + lr * 1.05f;
        const float litG = ao * (0.28f + sunVis * (0.10f + 0.42f * sunDiff)) + lg * 1.05f;
        const float litB = ao * (0.28f + sunVis * (0.10f + 0.42f * sunDiff)) + lb * 1.05f;

        float specR = 0.0f, specG = 0.0f, specB = 0.0f, fresnel = 0.0f;
        computeSpecularFresnel(optics, r, g, b, snx, sny, snz, sunVis, ao, specR, specG, specB, fresnel);

        if (mat == MaterialId::Water || mat == MaterialId::SaltWater) {
            const bool isSalt = (mat == MaterialId::SaltWater);
            const float sr = (isSalt ? 66.0f : 48.0f) * std::exp((isSalt ? -0.058f : -0.070f) * depth);
            const float sg = (isSalt ? 176.0f : 154.0f) * std::exp((isSalt ? -0.020f : -0.024f) * depth);
            const float sb =
                (isSalt ? 236.0f : 244.0f) * std::max(0.46f, std::exp((isSalt ? -0.012f : -0.010f) * depth));
            const float brine = (isSalt && colorSeed > 234) ? 14.0f : 0.0f;

            r = airBgR * transR * 0.65f + (sr + seedDelta * 0.45f + brine) * litR * 0.85f;
            g = airBgG * transG * 0.65f + (sg + seedDelta * 0.50f + brine) * litG * 0.85f;
            b = airBgB * transB * 0.65f + (sb + seedDelta * 0.45f + brine) * litB * 0.85f;

            // Sunlit subsurface caustic shimmer near top surface
            if (depth > 0.0f && depth <= 8.5f) {
                const float c = std::max(0.0f, fastWave(fx * 0.16f + fy * 0.11f - tPhase) *
                                                   fastWave(fx * 0.10f - fy * 0.15f + tPhase * 0.7f));
                const float boost = c * (1.0f - depth * (1.0f / 9.0f)) * 32.0f * (0.25f + 0.75f * sunVis);
                r += boost * 0.45f;
                g += boost * 0.85f;
                b += boost;
            }
            if (onCrest) {
                const float spd = std::abs(vx) + std::abs(vy) + 0.35f * (std::abs(airVx) + std::abs(airVy));
                const float foam = std::clamp((spd - 0.32f) * 0.55f + (wave - 0.64f) * 0.48f, 0.0f, 1.0f);
                r += 24.0f + wave * 28.0f;
                g += 36.0f + wave * 36.0f;
                b += 42.0f + wave * 42.0f;
                r = r * (1.0f - foam * 0.72f) + 234.0f * (foam * 0.72f);
                g = g * (1.0f - foam * 0.72f) + 247.0f * (foam * 0.72f);
                b = b * (1.0f - foam * 0.72f) + 255.0f * (foam * 0.72f);
            }
        } else if (mat == MaterialId::Acid) {
            const float sr = 134.0f * std::exp(-0.052f * depth) + seedDelta * 0.6f;
            const float sg = 248.0f * std::max(0.55f, std::exp(-0.014f * depth)) + seedDelta * 0.6f;
            const float sb = 68.0f * std::exp(-0.075f * depth) + seedDelta * 0.4f;
            const float emLitR = litR + 0.25f;
            const float emLitG = litG + 0.45f;
            const float emLitB = litB + 0.12f;

            r = airBgR * transR * 0.55f + sr * emLitR * 0.82f;
            g = airBgG * transG * 0.55f + sg * emLitG * 0.82f;
            b = airBgB * transB * 0.55f + sb * emLitB * 0.82f;

            // Rising effervescent fizz bubbles
            const float fizzWave =
                fastWave(fx * 0.33f + seedNorm * 0.4f) * fastWave(fy * 0.31f + tPhase * 1.85f + seedNorm * 0.5f);
            const float fizz = std::max(0.0f, fizzWave - 0.45f) * 52.0f;
            r += fizz * 0.65f;
            g += fizz;
            b += fizz * 0.35f;

            if (onCrest) {
                const float spd = std::abs(vx) + std::abs(vy);
                const float foam = std::clamp(0.24f + (spd - 0.3f) * 0.45f + wave * 0.25f, 0.0f, 0.85f);
                r = r * (1.0f - foam) + 215.0f * foam;
                g = g * (1.0f - foam) + 255.0f * foam;
                b = b * (1.0f - foam) + 165.0f * foam;
            }
        } else if (mat == MaterialId::Oil) {
            const float att = std::max(0.36f, std::exp(-0.058f * depth));
            const float sr = 92.0f * att + seedDelta * 0.6f;
            const float sg = 68.0f * att + seedDelta * 0.5f;
            const float sb = 42.0f * att + seedDelta * 0.4f;

            r = airBgR * transR * 0.42f + sr * litR * 0.90f;
            g = airBgG * transG * 0.42f + sg * litG * 0.90f;
            b = airBgB * transB * 0.42f + sb * litB * 0.90f;

            // Thin-film hydrocarbon iridescence on crests & shallow slicks
            if (onCrest || depth <= 2.5f) {
                const float film = fx * 0.095f + fy * 0.045f - tPhase * 0.6f + seedNorm * 0.25f;
                const float irR = 0.5f + 0.5f * fastWave(film);
                const float irG = 0.5f + 0.5f * fastWave(film + 0.333f);
                const float irB = 0.5f + 0.5f * fastWave(film + 0.666f);
                const float w = (onCrest ? 24.0f : 12.0f) * (0.4f + 0.6f * sunVis);
                r += irR * w;
                g += irG * w;
                b += irB * w * 1.15f;
            }
        } else { // Nitro
            const float sr = 96.0f * std::exp(-0.038f * depth) + seedDelta * 0.6f;
            const float sg = 222.0f * std::max(0.55f, std::exp(-0.014f * depth)) + seedDelta * 0.6f;
            const float sb = 158.0f * std::exp(-0.024f * depth) + seedDelta * 0.5f;
            const float agitation = std::clamp((std::abs(vx) + std::abs(vy) - 0.30f) * 0.6f, 0.0f, 1.0f);

            r = airBgR * transR * 0.60f + sr * (litR + 0.14f) * 0.85f + agitation * 24.0f;
            g = airBgG * transG * 0.60f + sg * (litG + 0.22f) * 0.85f + agitation * 28.0f;
            b = airBgB * transB * 0.60f + sb * (litB + 0.16f) * 0.85f;
            if (onCrest) {
                r += 22.0f + wave * 18.0f;
                g += 28.0f + wave * 22.0f;
                b += 24.0f + wave * 20.0f;
            }
        }

        // Fresnel sky glint + crisp specular reflection on fluid surfaces
        r += specR + fresnel * (onCrest ? 65.0f : 22.0f);
        g += specG + fresnel * (onCrest ? 82.0f : 28.0f);
        b += specB + fresnel * (onCrest ? 105.0f : 36.0f);
        handledComposite = true;
        break;
    }
    case MaterialId::Lava: {
        // Cooled dark basalt slag crust plates floating over incandescent magma fissures
        bool onCrest = false;
        const float depth = computeLiquidDepth(matPtr, x, y, W, H, gMode, onCrest);
        const float w1 = fastWave(fx * 0.088f + fy * 0.056f - tPhase * 0.28f + seedNorm * 0.24f);
        const float w2 = fastWave(fx * 0.064f - fy * 0.094f + tPhase * 0.22f - seedNorm * 0.20f);
        const float vein = std::clamp(std::max(1.0f - std::abs(w1) * 2.5f, 1.0f - std::abs(w2) * 2.5f), 0.0f, 1.0f);
        const float coolSurface = (onCrest ? 0.62f : 0.22f) + std::clamp((1520.0f - tempK) / 520.0f, 0.0f, 0.65f) -
                                  std::min(0.32f, depth * 0.04f);
        const float crustMask = std::clamp(coolSurface * (1.0f - vein * 0.92f), 0.0f, 0.90f);

        const float crustR = (42.0f + seedDelta * 0.45f) * (0.65f + 0.35f * sunVis);
        const float crustG = (34.0f + seedDelta * 0.40f) * (0.65f + 0.35f * sunVis);
        const float crustB = (38.0f + seedDelta * 0.45f) * (0.65f + 0.35f * sunVis);

        const float magmaBoost = 0.72f + 0.55f * vein + std::min(0.30f, depth * 0.04f);
        const float magmaR = (235.0f + seedDelta * 0.8f) * magmaBoost;
        const float magmaG = (96.0f + seedDelta * 0.6f) * (magmaBoost + 0.22f * vein);
        const float magmaB = (22.0f + seedDelta * 0.3f) * magmaBoost;

        r = magmaR * (1.0f - crustMask) + crustR * crustMask;
        g = magmaG * (1.0f - crustMask) + crustG * crustMask;
        b = magmaB * (1.0f - crustMask) + crustB * crustMask;
        bumpX = (w1 - w2) * 0.25f * crustMask;
        bumpY = (w1 + w2) * 0.25f * crustMask;
        outBbScale = 1.0f - crustMask * 0.74f;
        break;
    }
    case MaterialId::MoltenMetal:
    case MaterialId::MoltenGlass: {
        bool onCrest = false;
        const float depth = computeLiquidDepth(matPtr, x, y, W, H, gMode, onCrest);
        const float w1 = fastWave(fx * 0.12f - fy * 0.09f + tPhase * 0.45f + seedNorm * 0.2f);
        const float w2 = fastWave(fx * 0.08f + fy * 0.13f - tPhase * 0.35f);
        const float vein = std::clamp(1.0f - std::abs(w1) * 2.2f, 0.0f, 1.0f);
        const float coolThresh = (mat == MaterialId::MoltenMetal) ? 1420.0f : 1680.0f;
        const float coolSurface = (onCrest ? 0.52f : 0.14f) + std::clamp((coolThresh - tempK) / 480.0f, 0.0f, 0.58f);
        const float slagMask = std::clamp(coolSurface * (1.0f - vein * 0.88f) * 0.75f, 0.0f, 0.72f);

        const float slagR = (mat == MaterialId::MoltenMetal) ? 72.0f : 96.0f;
        const float slagG = (mat == MaterialId::MoltenMetal) ? 58.0f : 62.0f;
        const float slagB = (mat == MaterialId::MoltenMetal) ? 52.0f : 42.0f;

        const float swirl = 0.88f + 0.18f * w2 + std::min(0.18f, depth * 0.02f);
        const float hotR = (r + seedDelta * 0.8f) * swirl;
        const float hotG = (g + seedDelta * 0.7f) * swirl;
        const float hotB = (b + seedDelta * 0.5f) * swirl;

        r = hotR * (1.0f - slagMask) + slagR * slagMask;
        g = hotG * (1.0f - slagMask) + slagG * slagMask;
        b = hotB * (1.0f - slagMask) + slagB * slagMask;
        outBbScale = 1.0f - slagMask * 0.60f;
        break;
    }
    case MaterialId::Fire: {
        // Turbulent rising flame tongues graded from incandescent yellow-white core to crimson tips
        float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f;
        computeBackdrop(x, y, W, H, bgR, bgG, bgB);
        const float lifeNorm = (life > 0.0f) ? std::clamp(life * (1.0f / 28.0f), 0.0f, 1.0f) : 0.45f;
        const float lick =
            0.5f + 0.5f * fastWave(fx * 0.24f - tPhase * 2.1f + seedNorm * 0.6f) * fastWave(fy * 0.19f + tPhase * 2.7f);
        const float core = 0.55f + 0.55f * lifeNorm + 0.22f * lick;
        const float fr = (r + seedDelta * 0.85f) * std::min(1.18f, core + 0.18f);
        const float fg = (g + seedDelta * 0.70f) * (0.42f + 0.80f * lifeNorm);
        const float fb = (b + seedDelta * 0.40f) * (0.30f + 0.85f * lifeNorm * lifeNorm);
        const float alpha = std::clamp(0.45f + 0.55f * lifeNorm * (0.70f + 0.40f * lick), 0.30f, 1.0f);
        r = bgR * (1.0f - alpha) + fr * alpha;
        g = bgG * (1.0f - alpha) + fg * alpha;
        b = bgB * (1.0f - alpha) + fb * alpha;
        handledComposite = true;
        break;
    }
    case MaterialId::Spark: {
        const float lifeNorm = (life > 0.0f) ? std::clamp(life * (1.0f / 18.0f), 0.0f, 1.0f) : 0.55f;
        const float spd = std::min(1.0f, (std::abs(vx) + std::abs(vy)) * 0.22f);
        const float hot = std::clamp(0.55f * lifeNorm + 0.45f * spd, 0.0f, 1.0f);
        r = 225.0f + hot * 45.0f + seedDelta * 0.5f;
        g = 140.0f + hot * 125.0f + seedDelta * 0.5f;
        b = 55.0f + hot * 175.0f;
        handledComposite = true;
        break;
    }
    case MaterialId::Plasma: {
        // Swirling electric violet-magenta-cyan filaments with blazing core
        const float arc = fastWave(fx * 0.28f + fy * 0.22f - tPhase * 3.1f + seedNorm * 0.5f) *
                          fastWave(fx * 0.19f - fy * 0.29f + tPhase * 2.4f);
        const float fil = std::clamp(std::abs(arc) * 1.45f, 0.0f, 1.0f);
        const float qBoost = 0.85f + 0.25f * std::clamp(std::abs(charge), 0.0f, 1.5f);
        r = (178.0f + fil * 85.0f + seedDelta * 0.6f) * qBoost;
        g = (96.0f + fil * 145.0f + seedDelta * 0.6f) * qBoost;
        b = (255.0f + fil * 65.0f) * qBoost;
        handledComposite = true;
        break;
    }
    case MaterialId::Smoke:
    case MaterialId::Steam:
    case MaterialId::Methane: {
        float airBgR = 0.0f, airBgG = 0.0f, airBgB = 0.0f;
        computeAirLightBackdrop(x, y, W, H, sunVis, lr, lg, lb, airBgR, airBgG, airBgB);

        const float cloud = 0.5f + 0.5f * fastWave(fx * 0.11f + seedNorm * 0.5f - tPhase * 0.45f) *
                                       fastWave(fy * 0.11f + tPhase * 0.35f);
        const float gasLitR = 0.52f + 0.36f * sunVis + lr * 0.85f;
        const float gasLitG = 0.52f + 0.36f * sunVis + lg * 0.85f;
        const float gasLitB = 0.56f + 0.36f * sunVis + lb * 0.85f;

        float alpha = 0.30f;
        if (mat == MaterialId::Smoke) {
            const float lifeFrac = (life > 0.0f) ? std::min(1.0f, life * (1.0f / 260.0f)) : 0.65f;
            alpha = std::clamp((0.20f + 0.52f * lifeFrac) * (0.65f + 0.45f * cloud), 0.16f, 0.78f);
        } else if (mat == MaterialId::Steam) {
            const float lifeFrac = (life > 0.0f) ? std::min(1.0f, life * (1.0f / 280.0f)) : 0.65f;
            alpha = std::clamp((0.16f + 0.44f * lifeFrac) * (0.65f + 0.45f * cloud), 0.14f, 0.64f);
        } else {
            alpha = std::clamp(0.24f + 0.16f * cloud, 0.18f, 0.42f);
        }

        const float cloudR = (r + (cloud - 0.5f) * 22.0f + seedDelta * 0.65f) * gasLitR;
        const float cloudG = (g + (cloud - 0.5f) * 22.0f + seedDelta * 0.65f) * gasLitG;
        const float cloudB = (b + (cloud - 0.5f) * 24.0f + seedDelta * 0.65f) * gasLitB;
        r = airBgR * (1.0f - alpha) + cloudR * alpha;
        g = airBgG * (1.0f - alpha) + cloudG * alpha;
        b = airBgB * (1.0f - alpha) + cloudB * alpha;
        handledComposite = true;
        break;
    }
    default:
        r += seedDelta;
        g += seedDelta;
        b += seedDelta;
        break;
    }

    if (!handledComposite) {
        // Normalize perturbed 2.5D surface normal
        float snx = 0.0f, sny = 0.0f, snz = 1.0f;
        normalizeNormal(nx, ny, nz, bumpX, bumpY, snx, sny, snz);

        // Subtle 2.5D bevel contrast on thin 1-px solid obstacles
        if (props.isSolidObstacle()) {
            if (topEdge) {
                r *= 1.10f;
                g *= 1.10f;
                b *= 1.10f;
            } else if (leftEdge) {
                r *= 1.05f;
                g *= 1.05f;
                b *= 1.05f;
            } else if (botEdge) {
                r *= 0.90f;
                g *= 0.90f;
                b *= 0.90f;
            }
        }

        const float emSum = props.emissiveR + props.emissiveG + props.emissiveB;
        const float selfLit = std::min(0.95f, emSum * 0.72f);
        const float sunDiff = std::max(0.0f, snx * -0.48f + sny * -0.66f + snz * 0.58f);
        const float baseLit = ao * (0.28f + sunVis * (0.08f + 0.44f * sunDiff)) + selfLit;

        const float mulR = std::clamp(lr * 1.05f + baseLit, 0.24f, 2.40f);
        const float mulG = std::clamp(lg * 1.05f + baseLit, 0.24f, 2.40f);
        const float mulB = std::clamp(lb * 1.05f + baseLit, 0.24f, 2.40f);

        float specR = 0.0f, specG = 0.0f, specB = 0.0f, fresnel = 0.0f;
        computeSpecularFresnel(optics, r, g, b, snx, sny, snz, sunVis, ao, specR, specG, specB, fresnel);

        r = r * mulR + specR;
        g = g * mulG + specG;
        b = b * mulB + specB;

        if (optics.roughness < 0.35f) {
            const float rimAmt = fresnel * (0.35f - optics.roughness) * 145.0f * (0.35f + 0.65f * sunVis);
            r += rimAmt * 0.85f;
            g += rimAmt * 0.92f;
            b += rimAmt * 1.10f;
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

    const uint8_t *__restrict matPtr = m_mat.data();
    const uint8_t *__restrict seedPtr = m_colorSeed.data();
    const float *__restrict tempPtr = m_tempK.data();
    const float *__restrict lifePtr = m_life.data();
    const float *__restrict pressPtr = m_pressure.data();
    const float *__restrict vxPtr = m_vx.data();
    const float *__restrict vyPtr = m_vy.data();
    const float *__restrict airVxPtr = m_airVx.data();
    const float *__restrict airVyPtr = m_airVy.data();
    const float *__restrict chargePtr = m_charge.data();
    const float *__restrict lightRPtr = m_lightR.data();
    const float *__restrict lightGPtr = m_lightG.data();
    const float *__restrict lightBPtr = m_lightB.data();
    uint32_t *__restrict fbPtr = m_framebuffer.data();

    static const float kLogMinT = std::log(180.0f);
    static const float kInvLogSpanT = 1.0f / (std::log(3200.0f) - kLogMinT);

    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                const MaterialId mat = fromIdx(matPtr[i]);
                const auto &props = getMaterial(mat);

                if (mode == ViewMode::Physical) {
                    const float lr = lightRPtr[i];
                    const float lg = lightGPtr[i];
                    const float lb = lightBPtr[i];

                    float r = 0.0f, g = 0.0f, b = 0.0f;
                    float bbScale = 1.0f;
                    float chargeScale = 1.0f;

                    if (mat == MaterialId::Air) {
                        const float sunVis = computeSunShadow(matPtr, x, y, W, H, false);
                        computeAirLightBackdrop(x, y, W, H, sunVis, lr, lg, lb, r, g, b);
                        const float shock = std::clamp((std::abs(pressPtr[i]) - 0.55f) * 0.18f, 0.0f, 0.40f);
                        r += shock * 165.0f;
                        g += shock * 205.0f;
                        b += shock * 255.0f;
                    } else {
                        shadePhysicalMaterial(mat, props, matPtr, x, y, W, H, seedPtr[i], tempPtr[i], lifePtr[i],
                                              chargePtr[i], vxPtr[i], vyPtr[i], airVxPtr[i], airVyPtr[i], lr, lg, lb,
                                              gMode, tPhase, r, g, b, bbScale, chargeScale);
                    }

                    // Physical blackbody thermal glow for heated matter & hot air
                    float bbR = 0.0f, bbG = 0.0f, bbB = 0.0f;
                    blackbodyGlow(tempPtr[i], bbR, bbG, bbB);
                    r += bbR * (210.0f * bbScale);
                    g += bbG * (210.0f * bbScale);
                    b += bbB * (210.0f * bbScale);

                    // Continuous electrical potential & corona highlight
                    const float q = std::abs(chargePtr[i]);
                    if (q > 0.04f) {
                        const float qGlow = std::min(1.2f, (q - 0.04f) * 0.50f) * chargeScale;
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
                            sr += 46.0f;
                            sg += 52.0f;
                            sb += 64.0f;
                        } else if (botRightEdge) {
                            sr += 22.0f;
                            sg += 26.0f;
                            sb += 34.0f;
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
