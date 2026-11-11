#include "sim/SimWorld.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace sim {

namespace {

constexpr auto buildThermalCondLUT() noexcept {
    std::array<std::array<float, kMaterialCount>, kMaterialCount> lut{};
    for (size_t a = 0; a < kMaterialCount; ++a) {
        const float ca = kMaterialTable[a].heatCapacity;
        const float ka = kMaterialTable[a].thermalConductivity;
        for (size_t b = 0; b < kMaterialCount; ++b) {
            const float cb = kMaterialTable[b].heatCapacity;
            const float kb = kMaterialTable[b].thermalConductivity;
            const float kEff = (2.0f * ka * kb) / (ka + kb + 1e-6f);
            const float maxCond = 0.20f * (ca < cb ? ca : cb);
            const float cond = 0.45f * kEff;
            lut[a][b] = (cond < maxCond) ? cond : maxCond;
        }
    }
    return lut;
}

constexpr auto buildHeatCapLUT() noexcept {
    std::array<float, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        lut[i] = kMaterialTable[i].heatCapacity;
    }
    return lut;
}

constexpr auto buildElecCondLUT() noexcept {
    std::array<float, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        lut[i] = kMaterialTable[i].electricalConductivity;
    }
    return lut;
}

constexpr auto buildElecCouplingLUT() noexcept {
    std::array<std::array<float, kMaterialCount>, kMaterialCount> lut{};
    for (size_t a = 0; a < kMaterialCount; ++a) {
        const float ecA = kMaterialTable[a].electricalConductivity;
        if (ecA <= 0.005f) continue;
        for (size_t b = 0; b < kMaterialCount; ++b) {
            const float ecB = kMaterialTable[b].electricalConductivity;
            if (ecB <= 0.005f) continue;
            lut[a][b] = (ecA < ecB) ? ecA : ecB;
        }
    }
    return lut;
}

constexpr auto buildJouleEdgeLUT() noexcept {
    std::array<std::array<float, kMaterialCount>, kMaterialCount> lut{};
    const auto gLut = buildElecCouplingLUT();
    for (size_t a = 0; a < kMaterialCount; ++a) {
        for (size_t b = 0; b < kMaterialCount; ++b) {
            const float gEff = gLut[a][b];
            if (gEff > 0.0f) {
                const float resistance = 1.0f - 0.70f * gEff;
                lut[a][b] = 65.0f * resistance * gEff;
            }
        }
    }
    return lut;
}

constexpr auto buildLightTransLUT() noexcept {
    std::array<float, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        lut[i] = kMaterialTable[i].lightTransmittance;
    }
    return lut;
}

inline constexpr auto kThermalCondLUT = buildThermalCondLUT();
inline constexpr auto kHeatCapLUT = buildHeatCapLUT();
inline constexpr auto kElecCondLUT = buildElecCondLUT();
inline constexpr auto kElecCouplingLUT = buildElecCouplingLUT();
inline constexpr auto kJouleEdgeLUT = buildJouleEdgeLUT();
inline constexpr auto kLightTransLUT = buildLightTransLUT();

inline uint8_t safeMatIdx(uint8_t raw) noexcept {
    return (raw < kMaterialCount) ? raw : 0u;
}

} // namespace

void SimWorld::stepThermoElectric() {
    const int W = m_width;
    const int H = m_height;
    const float ambT = m_ambientTempK;
    const bool openBounds = (m_boundaryMode == BoundaryMode::Open);

    const uint8_t* __restrict mat = m_mat.data();
    const uint8_t lavaIdx = toIdx(MaterialId::Lava);
    const uint8_t plasmaIdx = toIdx(MaterialId::Plasma);
    const uint8_t batteryIdx = toIdx(MaterialId::Battery);

    // 1. Symmetric energy-conserving Fourier thermal conduction (2 sub-steps for crisp metal conductivity)
    for (int sub = 0; sub < 2; ++sub) {
        const float* __restrict tempIn = m_tempK.data();
        float* __restrict tempOut = m_tempTmp.data();
        const bool lastSub = (sub == 1);

        m_pool.parallelFor(0, H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int row = y * W;
                if (y > 0 && y < H - 1 && !openBounds) {
                    // Fast interior + closed-boundary path: zero y-bounds checks
                    for (int x = 0; x < W; ++x) {
                        const int i = row + x;
                        const uint8_t mA = safeMatIdx(mat[i]);
                        const float Ta = tempIn[i];
                        const float ca = kHeatCapLUT[mA];
                        const float* __restrict kRow = kThermalCondLUT[mA].data();

                        float netFlux = kRow[safeMatIdx(mat[i - W])] * (tempIn[i - W] - Ta) +
                                        kRow[safeMatIdx(mat[i + W])] * (tempIn[i + W] - Ta);
                        if (x > 0) netFlux += kRow[safeMatIdx(mat[i - 1])] * (tempIn[i - 1] - Ta);
                        if (x < W - 1) netFlux += kRow[safeMatIdx(mat[i + 1])] * (tempIn[i + 1] - Ta);

                        float newT = Ta + netFlux / ca;
                        if (lastSub && newT > 750.0f && mA != lavaIdx && mA != plasmaIdx) {
                            const float excess = newT - 750.0f;
                            newT -= std::min(excess, 0.0022f * excess);
                        }
                        tempOut[i] = std::isfinite(newT) ? std::clamp(newT, 1.0f, 8000.0f) : Ta;
                    }
                } else {
                    for (int x = 0; x < W; ++x) {
                        const int i = row + x;
                        const uint8_t mA = safeMatIdx(mat[i]);
                        const float Ta = tempIn[i];
                        const float ca = kHeatCapLUT[mA];
                        const float* __restrict kRow = kThermalCondLUT[mA].data();

                        float netFlux = 0.0f;
                        if (x > 0) netFlux += kRow[safeMatIdx(mat[i - 1])] * (tempIn[i - 1] - Ta);
                        if (x < W - 1) netFlux += kRow[safeMatIdx(mat[i + 1])] * (tempIn[i + 1] - Ta);
                        if (y > 0) netFlux += kRow[safeMatIdx(mat[i - W])] * (tempIn[i - W] - Ta);
                        if (y < H - 1) netFlux += kRow[safeMatIdx(mat[i + W])] * (tempIn[i + W] - Ta);

                        float newT = Ta + netFlux / ca;
                        if (lastSub) {
                            if (newT > 750.0f && mA != lavaIdx && mA != plasmaIdx) {
                                const float excess = newT - 750.0f;
                                newT -= std::min(excess, 0.0022f * excess);
                            }
                            if (openBounds && (x == 0 || x == W - 1 || y == 0 || y == H - 1)) {
                                newT += 0.12f * (ambT - newT);
                            }
                        }
                        tempOut[i] = std::isfinite(newT) ? std::clamp(newT, 1.0f, 8000.0f) : Ta;
                    }
                }
            }
        });
        m_tempK.swap(m_tempTmp);
    }

    // 2. Electrical charge conduction & Joule heating (Sparse Row-Skipped Red-Black SOR)
    constexpr int kElectricIters = 14;
    constexpr float kAlpha = 35.0f;
    constexpr float kSorOmega = 1.45f;

    float* __restrict charge = m_charge.data();
    float* __restrict chargeTmp = m_chargeTmp.data();
    float* __restrict tempK = m_tempK.data();

    uint8_t stackRowFlags[2048];
    std::vector<uint8_t> heapRowFlags;
    uint8_t* rowFlags = stackRowFlags;
    if (H > 2048) {
        heapRowFlags.resize(static_cast<size_t>(H));
        rowFlags = heapRowFlags.data();
    }

    // Initialize fixed sources, snapshot RHS q^n into m_chargeTmp, and record per-row conductor/charge flags
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            uint8_t flags = 0u;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                const uint8_t mA = safeMatIdx(mat[i]);
                float q = charge[i];
                if (mA == batteryIdx) {
                    q = 4.0f;
                    charge[i] = 4.0f;
                } else if (mA == plasmaIdx) {
                    if (q < 1.8f) {
                        q = 1.8f;
                        charge[i] = 1.8f;
                    }
                } else if (!std::isfinite(q)) {
                    q = 0.0f;
                    charge[i] = 0.0f;
                }
                chargeTmp[i] = q;
                if (kElecCondLUT[mA] > 0.005f) flags |= 1u;
                if (q != 0.0f) flags |= 2u;
            }
            rowFlags[y] = flags;
        }
    });

    int minCondY = H;
    int maxCondY = -1;
    int minActY = H;
    int maxActY = -1;
    for (int y = 0; y < H; ++y) {
        const uint8_t f = rowFlags[y];
        if (f & 1u) {
            if (y < minCondY) minCondY = y;
            maxCondY = y;
        }
        if (f & 2u) {
            if (y < minActY) minActY = y;
            maxActY = y;
        }
    }

    // Fast exit: no active electrical charge anywhere in domain
    if (minActY > maxActY) {
        return;
    }

    if (minCondY <= maxCondY) {
        std::atomic<bool> anyMoved{false};
        for (int iter = 0; iter < kElectricIters; ++iter) {
            const int span = 2 * (iter + 1);
            const int loY = std::max(minCondY, minActY - span);
            const int hiY = std::min(maxCondY, maxActY + span);
            if (loY > hiY) continue;

            anyMoved.store(false, std::memory_order_relaxed);
            for (int color = 0; color < 2; ++color) {
                m_pool.parallelFor(loY, hiY + 1, [&](int y0, int y1) {
                    float localMaxDelta = 0.0f;
                    for (int y = y0; y < y1; ++y) {
                        if ((rowFlags[y] & 1u) == 0u) continue;
                        const int row = y * W;
                        const int startX = ((y + color) & 1);
                        const bool interiorY = (y > 0 && y < H - 1);

                        for (int x = startX; x < W; x += 2) {
                            const int i = row + x;
                            const uint8_t mA = safeMatIdx(mat[i]);
                            if (mA == batteryIdx) continue;
                            if (kElecCondLUT[mA] <= 0.005f) continue;

                            const float* __restrict gRow = kElecCouplingLUT[mA].data();
                            float sumG = 0.0f;
                            float sumGQ = 0.0f;

                            if (interiorY && x > 0 && x < W - 1) {
                                const float gL = gRow[safeMatIdx(mat[i - 1])];
                                const float gR = gRow[safeMatIdx(mat[i + 1])];
                                const float gU = gRow[safeMatIdx(mat[i - W])];
                                const float gD = gRow[safeMatIdx(mat[i + W])];
                                sumG = gL + gR + gU + gD;
                                sumGQ = gL * charge[i - 1] + gR * charge[i + 1] +
                                        gU * charge[i - W] + gD * charge[i + W];
                            } else {
                                if (x > 0) {
                                    const float g = gRow[safeMatIdx(mat[i - 1])];
                                    sumG += g;
                                    sumGQ += g * charge[i - 1];
                                }
                                if (x < W - 1) {
                                    const float g = gRow[safeMatIdx(mat[i + 1])];
                                    sumG += g;
                                    sumGQ += g * charge[i + 1];
                                }
                                if (y > 0) {
                                    const float g = gRow[safeMatIdx(mat[i - W])];
                                    sumG += g;
                                    sumGQ += g * charge[i - W];
                                }
                                if (y < H - 1) {
                                    const float g = gRow[safeMatIdx(mat[i + W])];
                                    sumG += g;
                                    sumGQ += g * charge[i + W];
                                }
                            }

                            const float qOld = charge[i];
                            const float qGS = (chargeTmp[i] + kAlpha * sumGQ) / (1.0f + kAlpha * sumG);
                            const float qNext = std::clamp(qOld + kSorOmega * (qGS - qOld), -5.0f, 5.0f);
                            charge[i] = qNext;
                            const float diff = std::abs(qNext - qOld);
                            if (diff > localMaxDelta) localMaxDelta = diff;
                        }
                    }
                    if (localMaxDelta >= 2e-4f) {
                        anyMoved.store(true, std::memory_order_relaxed);
                    }
                });
            }
            if (iter >= 2 && !anyMoved.load(std::memory_order_relaxed)) {
                break;
            }
        }
    }

    // Apply Joule heating (I^2 * R) and gentle charge decay over rows that can hold non-zero charge
    const int maxSpan = 2 * kElectricIters;
    const int jLoY = (minCondY <= maxCondY)
                         ? std::min(minActY, std::max(minCondY, minActY - maxSpan))
                         : minActY;
    const int jHiY = (minCondY <= maxCondY)
                         ? std::max(maxActY, std::min(maxCondY, maxActY + maxSpan))
                         : maxActY;

    m_pool.parallelFor(jLoY, jHiY + 1, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            if (rowFlags[y] == 0u) continue;
            const int row = y * W;
            const bool interiorY = (y > 0 && y < H - 1);

            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                const uint8_t mA = safeMatIdx(mat[i]);
                const float ecA = kElecCondLUT[mA];
                if (ecA <= 0.005f) {
                    const float decayed = charge[i] * 0.65f;
                    charge[i] = (std::abs(decayed) >= 1e-6f) ? decayed : 0.0f;
                    continue;
                }
                if (mA == batteryIdx) {
                    charge[i] = 4.0f;
                    continue;
                }

                const float qA = charge[i];
                const float* __restrict jRow = kJouleEdgeLUT[mA].data();
                float jouleHeat = 0.0f;

                if (interiorY && x > 0 && x < W - 1) {
                    const float dqL = charge[i - 1] - qA;
                    const float dqR = charge[i + 1] - qA;
                    const float dqU = charge[i - W] - qA;
                    const float dqD = charge[i + W] - qA;
                    jouleHeat = jRow[safeMatIdx(mat[i - 1])] * dqL * dqL +
                                jRow[safeMatIdx(mat[i + 1])] * dqR * dqR +
                                jRow[safeMatIdx(mat[i - W])] * dqU * dqU +
                                jRow[safeMatIdx(mat[i + W])] * dqD * dqD;
                } else {
                    if (x > 0) {
                        const float dq = charge[i - 1] - qA;
                        jouleHeat += jRow[safeMatIdx(mat[i - 1])] * dq * dq;
                    }
                    if (x < W - 1) {
                        const float dq = charge[i + 1] - qA;
                        jouleHeat += jRow[safeMatIdx(mat[i + 1])] * dq * dq;
                    }
                    if (y > 0) {
                        const float dq = charge[i - W] - qA;
                        jouleHeat += jRow[safeMatIdx(mat[i - W])] * dq * dq;
                    }
                    if (y < H - 1) {
                        const float dq = charge[i + W] - qA;
                        jouleHeat += jRow[safeMatIdx(mat[i + W])] * dq * dq;
                    }
                }

                const float dqSelf = qA - chargeTmp[i];
                jouleHeat += 18.0f * (1.0f - 0.65f * ecA) * dqSelf * dqSelf;

                if (jouleHeat > 0.0005f) {
                    const float nextT = tempK[i] + jouleHeat / kHeatCapLUT[mA];
                    tempK[i] = std::isfinite(nextT) ? std::clamp(nextT, 1.0f, 8000.0f) : tempK[i];
                }

                const float decay = (ecA > 0.25f) ? 0.99992f : 0.994f;
                float qDecayed = qA * decay;
                if (mA == plasmaIdx) {
                    qDecayed = std::max(qDecayed, 1.8f);
                } else if (std::abs(qDecayed) < 1e-6f) {
                    qDecayed = 0.0f;
                }
                charge[i] = qDecayed;
            }
        }
    });
}

void SimWorld::stepLighting() {
    const int W = m_width;
    const int H = m_height;
    const uint8_t* __restrict mat = m_mat.data();
    const float* __restrict tempK = m_tempK.data();
    const float* __restrict charge = m_charge.data();

    // Seed emissive + ambient light sources at every pixel
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        float* __restrict lightR = m_lightR.data();
        float* __restrict lightG = m_lightG.data();
        float* __restrict lightB = m_lightB.data();

        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                const MaterialId m = fromIdx(mat[i]);
                const auto& props = getMaterial(m);

                float bbR = 0.0f, bbG = 0.0f, bbB = 0.0f;
                blackbodyGlow(tempK[i], bbR, bbG, bbB);

                // Electrical corona glow
                const float qGlow = std::min(1.0f, std::abs(charge[i]) * 0.45f);

                // Soft ambient base illumination (stronger in open air, subtle inside walls)
                const float ambFactor = props.isSolidObstacle() ? 0.08f : 0.24f;
                const float ambR = 0.78f * ambFactor;
                const float ambG = 0.84f * ambFactor;
                const float ambB = 1.00f * ambFactor;

                const float emR = props.emissiveR + bbR * 0.85f + qGlow * 0.35f;
                const float emG = props.emissiveG + bbG * 0.85f + qGlow * 0.65f;
                const float emB = props.emissiveB + bbB * 0.85f + qGlow * 1.05f;

                // Blend with previous frame slightly for smooth temporal glow stability
                const float r = std::max(ambR, emR) * 0.65f + lightR[i] * 0.35f;
                const float g = std::max(ambG, emG) * 0.65f + lightG[i] * 0.35f;
                const float b = std::max(ambB, emB) * 0.65f + lightB[i] * 0.35f;

                lightR[i] = std::isfinite(r) ? std::clamp(r, 0.0f, 2.5f) : ambR;
                lightG[i] = std::isfinite(g) ? std::clamp(g, 0.0f, 2.5f) : ambG;
                lightB[i] = std::isfinite(b) ? std::clamp(b, 0.0f, 2.5f) : ambB;
            }
        }
    });

    // Isotropic 8-neighbor weighted diffusion (1.0 axial, 0.7071 diagonal) for circular halos
    constexpr int kLightIters = 5;
    constexpr float kDiagW = 0.70710678f;
    constexpr float kInvFullW = 1.0f / (4.0f + 4.0f * kDiagW);

    for (int iter = 0; iter < kLightIters; ++iter) {
        const float* __restrict inR = m_lightR.data();
        const float* __restrict inG = m_lightG.data();
        const float* __restrict inB = m_lightB.data();
        float* __restrict outR = m_lightTmpR.data();
        float* __restrict outG = m_lightTmpG.data();
        float* __restrict outB = m_lightTmpB.data();

        m_pool.parallelFor(0, H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int row = y * W;
                if (y > 0 && y < H - 1) {
                    for (int x = 1; x < W - 1; ++x) {
                        const int i = row + x;
                        const float tr = kLightTransLUT[safeMatIdx(mat[i])];
                        const float trDiag = 0.91f * tr * std::sqrt(tr);

                        const float cR = inR[i];
                        const float cG = inG[i];
                        const float cB = inB[i];

                        // 4 axial neighbors (weight = 1.0)
                        const int iL = i - 1;
                        const int iR = i + 1;
                        const int iU = i - W;
                        const int iD = i + W;

                        // 4 diagonal neighbors (weight = 0.7071)
                        const int iUL = iU - 1;
                        const int iUR = iU + 1;
                        const int iDL = iD - 1;
                        const int iDR = iD + 1;

                        const float axMaxR = std::max({inR[iL], inR[iR], inR[iU], inR[iD]}) * tr;
                        const float axMaxG = std::max({inG[iL], inG[iR], inG[iU], inG[iD]}) * tr;
                        const float axMaxB = std::max({inB[iL], inB[iR], inB[iU], inB[iD]}) * tr;

                        const float dgMaxR = std::max({inR[iUL], inR[iUR], inR[iDL], inR[iDR]}) * trDiag;
                        const float dgMaxG = std::max({inG[iUL], inG[iUR], inG[iDL], inG[iDR]}) * trDiag;
                        const float dgMaxB = std::max({inB[iUL], inB[iUR], inB[iDL], inB[iDR]}) * trDiag;

                        const float maxR = std::max({cR, axMaxR, dgMaxR});
                        const float maxG = std::max({cG, axMaxG, dgMaxG});
                        const float maxB = std::max({cB, axMaxB, dgMaxB});

                        const float sumR = (inR[iL] + inR[iR] + inR[iU] + inR[iD]) +
                                           kDiagW * (inR[iUL] + inR[iUR] + inR[iDL] + inR[iDR]);
                        const float sumG = (inG[iL] + inG[iR] + inG[iU] + inG[iD]) +
                                           kDiagW * (inG[iUL] + inG[iUR] + inG[iDL] + inG[iDR]);
                        const float sumB = (inB[iL] + inB[iR] + inB[iU] + inB[iD]) +
                                           kDiagW * (inB[iUL] + inB[iUR] + inB[iDL] + inB[iDR]);

                        outR[i] = std::min(2.5f, std::max(cR, 0.50f * maxR + 0.50f * (sumR * kInvFullW) * tr));
                        outG[i] = std::min(2.5f, std::max(cG, 0.50f * maxG + 0.50f * (sumG * kInvFullW) * tr));
                        outB[i] = std::min(2.5f, std::max(cB, 0.50f * maxB + 0.50f * (sumB * kInvFullW) * tr));
                    }

                    // Left and right edge pixels of interior rows
                    for (int bx : {0, W - 1}) {
                        const int i = row + bx;
                        const float tr = kLightTransLUT[safeMatIdx(mat[i])];
                        const float trDiag = 0.91f * tr * std::sqrt(tr);

                        float maxR = inR[i], maxG = inG[i], maxB = inB[i];
                        float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
                        float wSum = 0.0f;

                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (dx == 0 && dy == 0) continue;
                                const int nx = bx + dx;
                                if (nx < 0 || nx >= W) continue;
                                const int j = (y + dy) * W + nx;
                                const bool diag = (dx != 0 && dy != 0);
                                const float w = diag ? kDiagW : 1.0f;
                                const float att = diag ? trDiag : tr;
                                sumR += inR[j] * w;
                                sumG += inG[j] * w;
                                sumB += inB[j] * w;
                                wSum += w;
                                maxR = std::max(maxR, inR[j] * att);
                                maxG = std::max(maxG, inG[j] * att);
                                maxB = std::max(maxB, inB[j] * att);
                            }
                        }

                        const float invW = 1.0f / wSum;
                        outR[i] = std::min(2.5f, std::max(inR[i], 0.50f * maxR + 0.50f * (sumR * invW) * tr));
                        outG[i] = std::min(2.5f, std::max(inG[i], 0.50f * maxG + 0.50f * (sumG * invW) * tr));
                        outB[i] = std::min(2.5f, std::max(inB[i], 0.50f * maxB + 0.50f * (sumB * invW) * tr));
                    }
                } else {
                    // Top and bottom border rows
                    for (int x = 0; x < W; ++x) {
                        const int i = row + x;
                        const float tr = kLightTransLUT[safeMatIdx(mat[i])];
                        const float trDiag = 0.91f * tr * std::sqrt(tr);

                        float maxR = inR[i], maxG = inG[i], maxB = inB[i];
                        float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
                        float wSum = 0.0f;

                        for (int dy = -1; dy <= 1; ++dy) {
                            const int ny = y + dy;
                            if (ny < 0 || ny >= H) continue;
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (dx == 0 && dy == 0) continue;
                                const int nx = x + dx;
                                if (nx < 0 || nx >= W) continue;
                                const int j = ny * W + nx;
                                const bool diag = (dx != 0 && dy != 0);
                                const float w = diag ? kDiagW : 1.0f;
                                const float att = diag ? trDiag : tr;
                                sumR += inR[j] * w;
                                sumG += inG[j] * w;
                                sumB += inB[j] * w;
                                wSum += w;
                                maxR = std::max(maxR, inR[j] * att);
                                maxG = std::max(maxG, inG[j] * att);
                                maxB = std::max(maxB, inB[j] * att);
                            }
                        }

                        if (wSum > 0.0f) {
                            const float invW = 1.0f / wSum;
                            outR[i] = std::min(2.5f, std::max(inR[i], 0.50f * maxR + 0.50f * (sumR * invW) * tr));
                            outG[i] = std::min(2.5f, std::max(inG[i], 0.50f * maxG + 0.50f * (sumG * invW) * tr));
                            outB[i] = std::min(2.5f, std::max(inB[i], 0.50f * maxB + 0.50f * (sumB * invW) * tr));
                        } else {
                            outR[i] = inR[i];
                            outG[i] = inG[i];
                            outB[i] = inB[i];
                        }
                    }
                }
            }
        });
        m_lightR.swap(m_lightTmpR);
        m_lightG.swap(m_lightTmpG);
        m_lightB.swap(m_lightTmpB);
    }
}

} // namespace sim
