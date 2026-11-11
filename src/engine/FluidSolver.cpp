#include "sim/SimWorld.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim {

namespace {

constexpr std::array<uint8_t, kMaterialCount> buildPhaseLut() noexcept {
    std::array<uint8_t, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        lut[i] = static_cast<uint8_t>(kMaterialTable[i].phase);
    }
    return lut;
}

constexpr std::array<float, kMaterialCount> buildLiquidHydroBiasLut() noexcept {
    std::array<float, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        if (kMaterialTable[i].phase == PhaseState::Liquid) {
            const float rho = kMaterialTable[i].density;
            lut[i] = 0.14f * (rho < 3.0f ? rho : 3.0f);
        }
    }
    return lut;
}

inline constexpr auto kPhaseLut = buildPhaseLut();
inline constexpr auto kLiquidHydroBias = buildLiquidHydroBiasLut();
inline constexpr uint8_t kPhaseEmptyGas = static_cast<uint8_t>(PhaseState::EmptyGas);
inline constexpr uint8_t kPhaseLiquid = static_cast<uint8_t>(PhaseState::Liquid);
inline constexpr uint8_t kPhaseEnergy = static_cast<uint8_t>(PhaseState::Energy);

inline float clampVel(float v) noexcept {
    return std::isfinite(v) ? std::clamp(v, -12.0f, 12.0f) : 0.0f;
}

// Solid-aware 2D DDA backtrace in cell-center coordinates: marches along (dx, dy) from (x, y)
// without crossing or tunneling across 1-px axial or sealed diagonal solid obstacles.
inline void traceOpenFluid(int x, int y, float dx, float dy, int W, int H, const uint8_t *__restrict mask, float &outX,
                           float &outY, int &outCellX, int &outCellY) noexcept {
    const float maxXf = static_cast<float>(W - 1);
    const float maxYf = static_cast<float>(H - 1);
    const float fx0 = static_cast<float>(x);
    const float fy0 = static_cast<float>(y);
    const float ex = std::clamp(fx0 + dx, 0.0f, maxXf);
    const float ey = std::clamp(fy0 + dy, 0.0f, maxYf);
    const float rdx = ex - fx0;
    const float rdy = ey - fy0;

    if (std::abs(rdx) < 0.499f && std::abs(rdy) < 0.499f) {
        outX = ex;
        outY = ey;
        outCellX = x;
        outCellY = y;
        return;
    }

    int cx = x;
    int cy = y;
    const int sx = (rdx > 1e-6f) ? 1 : ((rdx < -1e-6f) ? -1 : 0);
    const int sy = (rdy > 1e-6f) ? 1 : ((rdy < -1e-6f) ? -1 : 0);
    const float tDeltaX = (sx != 0) ? (1.0f / std::abs(rdx)) : 1e9f;
    const float tDeltaY = (sy != 0) ? (1.0f / std::abs(rdy)) : 1e9f;
    float tMaxX = (sx != 0) ? (0.5f * tDeltaX) : 1e9f;
    float tMaxY = (sy != 0) ? (0.5f * tDeltaY) : 1e9f;
    bool hitX = (sx == 0);
    bool hitY = (sy == 0);

    for (int step = 0; step < 32 && (tMaxX <= 1.0f || tMaxY <= 1.0f); ++step) {
        if (!hitX && !hitY && std::abs(tMaxX - tMaxY) < 1e-5f) {
            const int nx = cx + sx;
            const int ny = cy + sy;
            const bool xOpen = (nx >= 0 && nx < W && mask[cy * W + nx] == 0);
            const bool yOpen = (ny >= 0 && ny < H && mask[ny * W + cx] == 0);
            if (xOpen && yOpen && mask[ny * W + nx] == 0) {
                cx = nx;
                cy = ny;
                tMaxX += tDeltaX;
                tMaxY += tDeltaY;
            } else if (xOpen && !yOpen) {
                cx = nx;
                tMaxX += tDeltaX;
                hitY = true;
                tMaxY = 1e9f;
            } else if (yOpen && !xOpen) {
                cy = ny;
                tMaxY += tDeltaY;
                hitX = true;
                tMaxX = 1e9f;
            } else {
                hitX = true;
                hitY = true;
                break;
            }
        } else if (tMaxX <= tMaxY) {
            const int nx = cx + sx;
            if (nx >= 0 && nx < W && mask[cy * W + nx] == 0) {
                cx = nx;
                tMaxX += tDeltaX;
            } else {
                hitX = true;
                tMaxX = 1e9f;
            }
        } else {
            const int ny = cy + sy;
            if (ny >= 0 && ny < H && mask[ny * W + cx] == 0) {
                cy = ny;
                tMaxY += tDeltaY;
            } else {
                hitY = true;
                tMaxY = 1e9f;
            }
        }
    }

    outCellX = cx;
    outCellY = cy;
    const float cxf = static_cast<float>(cx);
    const float cyf = static_cast<float>(cy);
    outX = hitX ? cxf : std::clamp(ex, std::max(0.0f, cxf - 0.499f), std::min(maxXf, cxf + 0.499f));
    outY = hitY ? cyf : std::clamp(ey, std::max(0.0f, cyf - 0.499f), std::min(maxYf, cyf + 0.499f));
}

} // namespace

void SimWorld::stepFluidField() {
    const int W = m_width;
    const int H = m_height;
    const bool closed = (m_boundaryMode == BoundaryMode::Closed);
    const float ambT = m_ambientTempK;
    const GravityMode gMode = m_gravityMode;
    const float cx = static_cast<float>(W) * 0.5f;
    const float cy = static_cast<float>(H) * 0.5f;
    const uint8_t airIdx = toIdx(MaterialId::Air);

    uint8_t *__restrict mask = m_solidMask.data();
    const uint8_t *__restrict mat = m_mat.data();
    const float *__restrict pVx = m_vx.data();
    const float *__restrict pVy = m_vy.data();
    float *__restrict airVx = m_airVx.data();
    float *__restrict airVy = m_airVy.data();
    float *__restrict airVxTmp = m_airVxTmp.data();
    float *__restrict airVyTmp = m_airVyTmp.data();
    float *__restrict press = m_pressure.data();
    float *__restrict div = m_div.data();
    float *__restrict blast = m_blastDiv.data();
    float *__restrict temp = m_tempK.data();
    float *__restrict tempTmp = m_tempTmp.data();

    // 1. Build solid obstacle mask, apply thermal buoyancy & particle drag coupling
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const bool borderY = closed && (y == 0 || y == H - 1);
            const int row = y * W;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                const MaterialId mid = fromIdx(mat[i]);
                const auto &props = getMaterial(mid);
                const bool blocked = borderY || (closed && (x == 0 || x == W - 1)) || props.isSolidObstacle();
                mask[i] = blocked ? 1u : 0u;
                if (blocked) {
                    airVx[i] = 0.0f;
                    airVy[i] = 0.0f;
                    continue;
                }

                float vx = airVx[i];
                float vy = airVy[i];

                if (props.isGasOrEnergy()) {
                    const float dT = std::clamp(temp[i] - ambT, -260.0f, 3500.0f);
                    const float buoy = 0.00038f * dT;
                    if (gMode == GravityMode::Earth) {
                        vy -= buoy;
                    } else if (gMode == GravityMode::Inverted) {
                        vy += buoy;
                    } else if (gMode == GravityMode::RadialCenter) {
                        const float dx = static_cast<float>(x) - cx;
                        const float dy = static_cast<float>(y) - cy;
                        const float dist = std::sqrt(dx * dx + dy * dy) + 1.0f;
                        vx += (dx / dist) * buoy;
                        vy += (dy / dist) * buoy;
                    }
                    if (mat[i] != airIdx) {
                        vx = vx * 0.92f + pVx[i] * 0.08f;
                        vy = vy * 0.92f + pVy[i] * 0.08f;
                    }
                } else if (props.phase == PhaseState::Liquid) {
                    vx = vx * 0.95f + pVx[i] * 0.05f;
                    vy = vy * 0.95f + pVy[i] * 0.05f;
                    const float hydroG = kLiquidHydroBias[toIdx(mid)];
                    if (gMode == GravityMode::Earth) {
                        vy += hydroG;
                    } else if (gMode == GravityMode::Inverted) {
                        vy -= hydroG;
                    } else if (gMode == GravityMode::RadialCenter) {
                        const float dx = cx - static_cast<float>(x);
                        const float dy = cy - static_cast<float>(y);
                        const float dist = std::sqrt(dx * dx + dy * dy);
                        if (dist > 1.5f) {
                            vx += (dx / dist) * hydroG;
                            vy += (dy / dist) * hydroG;
                        }
                    }
                } else if (props.isMovable()) {
                    vx = vx * 0.88f + pVx[i] * 0.12f;
                    vy = vy * 0.88f + pVy[i] * 0.12f;
                }

                airVx[i] = clampVel(vx);
                airVy[i] = clampVel(vy);
            }
        }
    });

    // 2. Solid-aware Semi-Lagrangian advection (+ bounded thermal advection in pure Air)
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                if (mask[i]) {
                    airVxTmp[i] = 0.0f;
                    airVyTmp[i] = 0.0f;
                    tempTmp[i] = temp[i];
                    continue;
                }

                const float vxC = clampVel(airVx[i]);
                const float vyC = clampVel(airVy[i]);
                // 2nd-order midpoint (RK2) backtrace with exact solid-barrier ray clamping
                float mx = 0.0f, my = 0.0f;
                int mCellX = x, mCellY = y;
                traceOpenFluid(x, y, -vxC * 0.45f, -vyC * 0.45f, W, H, mask, mx, my, mCellX, mCellY);
                const int mIdx = mCellY * W + mCellX;
                const float traceVx = 0.5f * (vxC + clampVel(airVx[mIdx]));
                const float traceVy = 0.5f * (vyC + clampVel(airVy[mIdx]));

                float sx = 0.0f, sy = 0.0f;
                int endX = x, endY = y;
                traceOpenFluid(x, y, -traceVx * 0.90f, -traceVy * 0.90f, W, H, mask, sx, sy, endX, endY);

                const int ix = std::min(static_cast<int>(sx), W - 2);
                const int iy = std::min(static_cast<int>(sy), H - 2);
                const float tx = sx - static_cast<float>(ix);
                const float ty = sy - static_cast<float>(iy);

                const int i00 = iy * W + ix;
                const int i10 = i00 + 1;
                const int i01 = i00 + W;
                const int i11 = i01 + 1;

                const float w00 = (1.0f - tx) * (1.0f - ty);
                const float w10 = tx * (1.0f - ty);
                const float w01 = (1.0f - tx) * ty;
                const float w11 = tx * ty;

                const uint8_t mAny = mask[i00] | mask[i10] | mask[i01] | mask[i11];
                if (mAny == 0) {
                    airVxTmp[i] = clampVel(airVx[i00] * w00 + airVx[i10] * w10 + airVx[i01] * w01 + airVx[i11] * w11);
                    airVyTmp[i] = clampVel(airVy[i00] * w00 + airVy[i10] * w10 + airVy[i01] * w01 + airVy[i11] * w11);
                } else {
                    // Free-slip wall interpolation: exclude solid cells and the opposite side of sealed 1-px diagonal
                    // barriers
                    bool ok00 = (mask[i00] == 0);
                    bool ok10 = (mask[i10] == 0);
                    bool ok01 = (mask[i01] == 0);
                    bool ok11 = (mask[i11] == 0);
                    if (mask[i00] && mask[i11]) {
                        if (endX == ix + 1 && endY == iy)
                            ok01 = false;
                        else if (endX == ix && endY == iy + 1)
                            ok10 = false;
                        else {
                            ok10 = false;
                            ok01 = false;
                        }
                    }
                    if (mask[i10] && mask[i01]) {
                        if (endX == ix && endY == iy)
                            ok11 = false;
                        else if (endX == ix + 1 && endY == iy + 1)
                            ok00 = false;
                        else {
                            ok00 = false;
                            ok11 = false;
                        }
                    }
                    float wSum = 0.0f;
                    float sumU = 0.0f;
                    float sumV = 0.0f;
                    if (ok00) {
                        wSum += w00;
                        sumU += airVx[i00] * w00;
                        sumV += airVy[i00] * w00;
                    }
                    if (ok10) {
                        wSum += w10;
                        sumU += airVx[i10] * w10;
                        sumV += airVy[i10] * w10;
                    }
                    if (ok01) {
                        wSum += w01;
                        sumU += airVx[i01] * w01;
                        sumV += airVy[i01] * w01;
                    }
                    if (ok11) {
                        wSum += w11;
                        sumU += airVx[i11] * w11;
                        sumV += airVy[i11] * w11;
                    }
                    if (wSum > 1e-4f) {
                        const float invW = 1.0f / wSum;
                        airVxTmp[i] = clampVel(sumU * invW);
                        airVyTmp[i] = clampVel(sumV * invW);
                    } else {
                        airVxTmp[i] = vxC;
                        airVyTmp[i] = vyC;
                    }
                }

                if (mAny == 0 && mat[i] == airIdx && mat[i00] == airIdx && mat[i10] == airIdx && mat[i01] == airIdx &&
                    mat[i11] == airIdx) {
                    const float tAdv = temp[i00] * w00 + temp[i10] * w10 + temp[i01] * w01 + temp[i11] * w11;
                    const float minT = std::min({temp[i00], temp[i10], temp[i01], temp[i11]});
                    const float maxT = std::max({temp[i00], temp[i10], temp[i01], temp[i11]});
                    tempTmp[i] = std::isfinite(tAdv) ? std::clamp(tAdv, minT, maxT) : temp[i];
                } else {
                    tempTmp[i] = temp[i];
                }
            }
        }
    });

    m_airVx.swap(m_airVxTmp);
    m_airVy.swap(m_airVyTmp);
    m_tempK.swap(m_tempTmp);
    airVx = m_airVx.data();
    airVy = m_airVy.data();
    temp = m_tempK.data();

    // 3. Vorticity confinement (curl into m_div scratch; zero boundary borders to avoid stale curl reads)
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            if (y == 0 || y == H - 1) {
                std::fill(div + row, div + row + W, 0.0f);
                continue;
            }
            div[row] = 0.0f;
            div[row + W - 1] = 0.0f;
            for (int x = 1; x < W - 1; ++x) {
                const int i = row + x;
                if (mask[i]) {
                    div[i] = 0.0f;
                    continue;
                }
                const float vR = mask[i + 1] ? airVy[i] : airVy[i + 1];
                const float vL = mask[i - 1] ? airVy[i] : airVy[i - 1];
                const float uD = mask[i + W] ? airVx[i] : airVx[i + W];
                const float uU = mask[i - W] ? airVx[i] : airVx[i - W];
                div[i] = 0.5f * ((vR - vL) - (uD - uU));
            }
        }
    });

    m_pool.parallelFor(1, H - 1, [&](int y0, int y1) {
        constexpr float kVorticityEps = 0.11f;
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            for (int x = 1; x < W - 1; ++x) {
                const int i = row + x;
                if (mask[i]) continue;
                const uint8_t mC = mat[i];
                if (mC < kMaterialCount && kPhaseLut[mC] == kPhaseLiquid) continue;

                const float wR = mask[i + 1] ? std::abs(div[i]) : std::abs(div[i + 1]);
                const float wL = mask[i - 1] ? std::abs(div[i]) : std::abs(div[i - 1]);
                const float wD = mask[i + W] ? std::abs(div[i]) : std::abs(div[i + W]);
                const float wU = mask[i - W] ? std::abs(div[i]) : std::abs(div[i - W]);
                const float dwDx = 0.5f * (wR - wL);
                const float dwDy = 0.5f * (wD - wU);
                const float len = std::sqrt(dwDx * dwDx + dwDy * dwDy);
                if (len > 1e-4f) {
                    const float invLen = kVorticityEps / len;
                    const float omega = div[i];
                    airVx[i] = clampVel(airVx[i] + (dwDy * invLen) * omega);
                    airVy[i] = clampVel(airVy[i] - (dwDx * invLen) * omega);
                }
            }
        }
    });

    // 4. Divergence calculation & material-aware pressure warm-start decay
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                if (mask[i]) {
                    div[i] = 0.0f;
                    press[i] = 0.0f;
                    blast[i] = 0.0f;
                    continue;
                }

                const uint8_t mIdx = (mat[i] < kMaterialCount) ? mat[i] : 0;
                const uint8_t phC = kPhaseLut[mIdx];
                const bool isLiqC = (phC == kPhaseLiquid);
                const bool isGasC = (phC == kPhaseEmptyGas) || (phC == kPhaseEnergy);

                const float pWarm = press[i] * (isLiqC ? 0.9999f : 0.62f);
                press[i] = std::isfinite(pWarm) ? pWarm : 0.0f;

                auto readU = [&](int j) noexcept -> float {
                    if (mask[j]) return 0.0f;
                    if (isGasC) {
                        const uint8_t mj = (mat[j] < kMaterialCount) ? mat[j] : 0;
                        if (kPhaseLut[mj] == kPhaseLiquid) return pVx[j];
                    }
                    return airVx[j];
                };
                auto readV = [&](int j) noexcept -> float {
                    if (mask[j]) return 0.0f;
                    if (isGasC) {
                        const uint8_t mj = (mat[j] < kMaterialCount) ? mat[j] : 0;
                        if (kPhaseLut[mj] == kPhaseLiquid) return pVy[j];
                    }
                    return airVy[j];
                };

                const float uL = (x > 0) ? readU(i - 1) : (closed ? 0.0f : airVx[i]);
                const float uR = (x < W - 1) ? readU(i + 1) : (closed ? 0.0f : airVx[i]);
                const float vU = (y > 0) ? readV(i - W) : (closed ? 0.0f : airVy[i]);
                const float vD = (y < H - 1) ? readV(i + W) : (closed ? 0.0f : airVy[i]);

                float d = 0.5f * ((uR - uL) + (vD - vU));

                // Cancel artificial internal hydrostatic excess at Liquid/Gas free surface
                if (isLiqC && y > 0 && y < H - 1) {
                    const uint8_t mUp = (mat[i - W] < kMaterialCount) ? mat[i - W] : 0;
                    const uint8_t mDn = (mat[i + W] < kMaterialCount) ? mat[i + W] : 0;
                    if (gMode == GravityMode::Earth) {
                        if (!mask[i - W] && kPhaseLut[mUp] == kPhaseEmptyGas && kPhaseLut[mDn] == kPhaseLiquid) {
                            d -= 0.5f * std::max(0.0f, airVy[i + W] - pVy[i + W]);
                        }
                    } else if (gMode == GravityMode::Inverted) {
                        if (!mask[i + W] && kPhaseLut[mDn] == kPhaseEmptyGas && kPhaseLut[mUp] == kPhaseLiquid) {
                            d -= 0.5f * std::max(0.0f, pVy[i - W] - airVy[i - W]);
                        }
                    }
                }

                const float bVal = std::isfinite(blast[i]) ? blast[i] : 0.0f;
                div[i] = std::isfinite(d) ? (d - bVal) : -bVal;
                blast[i] = 0.0f;
            }
        }
    });

    // 5. Red-Black SOR Pressure Poisson solver
    constexpr int kPressureIters = 10;
    constexpr float kSorOmega = 1.15f;
    for (int iter = 0; iter < kPressureIters; ++iter) {
        for (int color = 0; color < 2; ++color) {
            m_pool.parallelFor(0, H, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    const int row = y * W;
                    if (y > 0 && y < H - 1) {
                        int x = 1 + (((y + 1 + color) & 1));
                        for (; x < W - 1; x += 2) {
                            const int i = row + x;
                            if (mask[i]) continue;
                            if ((mask[i - 1] | mask[i + 1] | mask[i - W] | mask[i + W]) == 0) {
                                const float pGS =
                                    0.25f * (press[i - 1] + press[i + 1] + press[i - W] + press[i + W] - div[i]);
                                press[i] = std::clamp(press[i] + kSorOmega * (pGS - press[i]), -60.0f, 60.0f);
                            } else {
                                const int oL = 1 ^ mask[i - 1];
                                const int oR = 1 ^ mask[i + 1];
                                const int oU = 1 ^ mask[i - W];
                                const int oD = 1 ^ mask[i + W];
                                const int openN = oL + oR + oU + oD;
                                if (openN > 0) {
                                    const float sumP =
                                        static_cast<float>(oL) * press[i - 1] + static_cast<float>(oR) * press[i + 1] +
                                        static_cast<float>(oU) * press[i - W] + static_cast<float>(oD) * press[i + W];
                                    const float pGS = (sumP - div[i]) / static_cast<float>(openN);
                                    press[i] = std::clamp(press[i] + kSorOmega * (pGS - press[i]), -60.0f, 60.0f);
                                }
                            }
                        }
                        if (!closed) {
                            for (int bx : {0, W - 1}) {
                                if (((bx + y) & 1) != color) continue;
                                const int i = row + bx;
                                if (mask[i]) continue;
                                float sumP = 0.0f;
                                int openN = 1; // outside P = 0
                                if (bx == 0 && !mask[i + 1]) {
                                    sumP += press[i + 1];
                                    ++openN;
                                }
                                if (bx == W - 1 && !mask[i - 1]) {
                                    sumP += press[i - 1];
                                    ++openN;
                                }
                                if (!mask[i - W]) {
                                    sumP += press[i - W];
                                    ++openN;
                                }
                                if (!mask[i + W]) {
                                    sumP += press[i + W];
                                    ++openN;
                                }
                                const float pGS = (sumP - div[i]) / static_cast<float>(openN);
                                press[i] = std::clamp(press[i] + kSorOmega * (pGS - press[i]), -60.0f, 60.0f);
                            }
                        }
                    } else if (!closed) {
                        const int startX = ((y + color) & 1);
                        for (int x = startX; x < W; x += 2) {
                            const int i = row + x;
                            if (mask[i]) continue;
                            float sumP = 0.0f;
                            int openN = 0;
                            if (x > 0) {
                                if (!mask[i - 1]) {
                                    sumP += press[i - 1];
                                    ++openN;
                                }
                            } else
                                ++openN;
                            if (x < W - 1) {
                                if (!mask[i + 1]) {
                                    sumP += press[i + 1];
                                    ++openN;
                                }
                            } else
                                ++openN;
                            if (y > 0) {
                                if (!mask[i - W]) {
                                    sumP += press[i - W];
                                    ++openN;
                                }
                            } else
                                ++openN;
                            if (y < H - 1) {
                                if (!mask[i + W]) {
                                    sumP += press[i + W];
                                    ++openN;
                                }
                            } else
                                ++openN;
                            if (openN > 0) {
                                const float pGS = (sumP - div[i]) / static_cast<float>(openN);
                                press[i] = std::clamp(press[i] + kSorOmega * (pGS - press[i]), -60.0f, 60.0f);
                            }
                        }
                    }
                }
            });
        }
    }

    // 6. Subtract pressure gradient & apply subtle viscous damping
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int row = y * W;
            const bool interiorY = (y > 0 && y < H - 1);
            for (int x = 0; x < W; ++x) {
                const int i = row + x;
                if (mask[i]) {
                    airVx[i] = 0.0f;
                    airVy[i] = 0.0f;
                    continue;
                }

                const uint8_t mIdx = (mat[i] < kMaterialCount) ? mat[i] : 0;
                const float damp = (kPhaseLut[mIdx] == kPhaseLiquid) ? 0.992f : 0.995f;

                float gradX = 0.0f;
                float gradY = 0.0f;
                if (interiorY && x > 0 && x < W - 1 && (mask[i - 1] | mask[i + 1] | mask[i - W] | mask[i + W]) == 0) {
                    gradX = 0.5f * (press[i + 1] - press[i - 1]);
                    gradY = 0.5f * (press[i + W] - press[i - W]);
                } else {
                    const float pC = press[i];
                    const float pL = (x > 0) ? (mask[i - 1] ? pC : press[i - 1]) : (closed ? pC : 0.0f);
                    const float pR = (x < W - 1) ? (mask[i + 1] ? pC : press[i + 1]) : (closed ? pC : 0.0f);
                    const float pU = (y > 0) ? (mask[i - W] ? pC : press[i - W]) : (closed ? pC : 0.0f);
                    const float pD = (y < H - 1) ? (mask[i + W] ? pC : press[i + W]) : (closed ? pC : 0.0f);
                    gradX = 0.5f * (pR - pL);
                    gradY = 0.5f * (pD - pU);
                }

                airVx[i] = clampVel((airVx[i] - gradX) * damp);
                airVy[i] = clampVel((airVy[i] - gradY) * damp);
            }
        }
    });
}

} // namespace sim
