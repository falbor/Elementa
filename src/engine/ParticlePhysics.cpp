#include "sim/SimWorld.hpp"

#include <algorithm>
#include <cmath>

namespace sim {

void SimWorld::computeGravityAt(int x, int y, float& gx, float& gy) const noexcept {
    switch (m_gravityMode) {
    case GravityMode::Earth:
        gx = 0.0f;
        gy = 0.24f;
        break;
    case GravityMode::ZeroG:
        gx = 0.0f;
        gy = 0.0f;
        break;
    case GravityMode::Inverted:
        gx = 0.0f;
        gy = -0.24f;
        break;
    case GravityMode::RadialCenter: {
        const float cx = static_cast<float>(m_width) * 0.5f;
        const float cy = static_cast<float>(m_height) * 0.5f;
        const float dx = cx - static_cast<float>(x);
        const float dy = cy - static_cast<float>(y);
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > 1.5f) {
            gx = (dx / dist) * 0.20f;
            gy = (dy / dist) * 0.20f;
        } else {
            gx = 0.0f;
            gy = 0.0f;
        }
        break;
    }
    }
}

bool SimWorld::canDisplace(MaterialId mover, MaterialId target) const noexcept {
    if (target == MaterialId::Air) return true;
    const auto& pMover = getMaterial(mover);
    const auto& pTarget = getMaterial(target);
    if (pTarget.isSolidObstacle()) return false;

    if (pMover.phase == PhaseState::Powder) {
        return pTarget.phase == PhaseState::Liquid || pTarget.isGasOrEnergy();
    }
    if (pMover.phase == PhaseState::Liquid) {
        if (pTarget.isGasOrEnergy()) return true;
        if (pTarget.phase == PhaseState::Liquid) {
            return pMover.density > pTarget.density * 1.03f;
        }
        return false;
    }
    if (pMover.phase == PhaseState::EmptyGas && mover != MaterialId::Air) {
        // Rising gas bubbles pass upward through liquids or lighter energy
        return pTarget.phase == PhaseState::Liquid || target == MaterialId::Fire;
    }
    if (pMover.phase == PhaseState::Energy) {
        return pTarget.phase == PhaseState::EmptyGas;
    }
    return false;
}

void SimWorld::moveParticle(int srcIdx, int dstIdx) noexcept {
    m_mat[dstIdx] = m_mat[srcIdx];
    m_vx[dstIdx] = m_vx[srcIdx];
    m_vy[dstIdx] = m_vy[srcIdx];
    m_fx[dstIdx] = m_fx[srcIdx];
    m_fy[dstIdx] = m_fy[srcIdx];
    m_life[dstIdx] = m_life[srcIdx];
    m_charge[dstIdx] = m_charge[srcIdx];
    m_colorSeed[dstIdx] = m_colorSeed[srcIdx];
    m_movedTick[dstIdx] = m_tickParity;

    // Exact thermal energy conservation: swap temperature of particle with displaced Air cell
    std::swap(m_tempK[srcIdx], m_tempK[dstIdx]);

    m_mat[srcIdx] = toIdx(MaterialId::Air);
    m_vx[srcIdx] = 0.0f;
    m_vy[srcIdx] = 0.0f;
    m_fx[srcIdx] = 0.0f;
    m_fy[srcIdx] = 0.0f;
    m_life[srcIdx] = 0.0f;
    m_charge[srcIdx] = 0.0f;
    m_colorSeed[srcIdx] = 0;
    m_movedTick[srcIdx] = 0;
}

void SimWorld::swapParticles(int aIdx, int bIdx) noexcept {
    std::swap(m_mat[aIdx], m_mat[bIdx]);
    std::swap(m_vx[aIdx], m_vx[bIdx]);
    std::swap(m_vy[aIdx], m_vy[bIdx]);
    std::swap(m_fx[aIdx], m_fx[bIdx]);
    std::swap(m_fy[aIdx], m_fy[bIdx]);
    std::swap(m_life[aIdx], m_life[bIdx]);
    std::swap(m_charge[aIdx], m_charge[bIdx]);
    std::swap(m_colorSeed[aIdx], m_colorSeed[bIdx]);
    std::swap(m_tempK[aIdx], m_tempK[bIdx]);
    m_movedTick[aIdx] = m_tickParity;
    m_movedTick[bIdx] = m_tickParity;
}

void SimWorld::updateParticleAt(int x, int y, uint8_t tickParity) {
    const int W = m_width;
    const int H = m_height;
    int curX = x;
    int curY = y;
    int curIdx = curY * W + curX;

    if (m_movedTick[curIdx] == tickParity) return;

    const MaterialId mat = fromIdx(m_mat[curIdx]);
    const auto& props = getMaterial(mat);
    if (!props.isMovable()) return;
    m_movedTick[curIdx] = tickParity;

    const bool closed = (m_boundaryMode == BoundaryMode::Closed);
    const uint32_t rnd = hashCoord(curX, curY, m_stats.tick);

    // 1. Compute forces: gravity/buoyancy + aerodynamic drag from Eulerian air field
    float gx = 0.0f, gy = 0.0f;
    computeGravityAt(curX, curY, gx, gy);

    float ax = 0.0f, ay = 0.0f;
    if (props.phase == PhaseState::Powder) {
        const float gScale = (mat == MaterialId::Snow) ? 0.35f : 1.0f;
        ax = gx * gScale;
        ay = gy * gScale;
    } else if (props.phase == PhaseState::Liquid) {
        ax = gx;
        ay = gy;
    } else if (props.phase == PhaseState::EmptyGas) {
        // Gas buoyancy relative to air temperature
        const float dT = m_tempK[curIdx] - m_ambientTempK;
        float buoyScale = -0.30f;
        if (mat == MaterialId::Smoke) {
            buoyScale = (dT > 15.0f) ? -0.35f : 0.04f;
        } else if (mat == MaterialId::Steam) {
            buoyScale = -0.45f;
        } else if (mat == MaterialId::Methane) {
            buoyScale = -0.38f;
        }
        ax = gx * buoyScale;
        ay = gy * buoyScale;
        // Gentle Brownian dispersion
        ax += static_cast<float>(static_cast<int>(rnd % 3u) - 1) * 0.05f;
    } else if (props.phase == PhaseState::Energy) {
        if (mat == MaterialId::Fire) {
            ax = -gx * 0.45f + static_cast<float>(static_cast<int>(rnd % 3u) - 1) * 0.09f;
            ay = -gy * 0.45f;
        } else if (mat == MaterialId::Spark) {
            ax = gx * 0.55f;
            ay = gy * 0.55f;
        } else if (mat == MaterialId::Plasma) {
            ax = -gx * 0.10f + static_cast<float>(static_cast<int>(rnd % 5u) - 2) * 0.10f;
            ay = -gy * 0.10f + static_cast<float>(static_cast<int>((rnd >> 4) % 5u) - 2) * 0.10f;
        }
    }

    float vx = m_vx[curIdx] + ax;
    float vy = m_vy[curIdx] + ay;

    // Pressure-gradient force (-grad P / rho) from shockwaves & explosive expansion
    if (curX > 0 && curX < W - 1 && curY > 0 && curY < H - 1) {
        const float dpx = 0.5f * (m_pressure[curIdx + 1] - m_pressure[curIdx - 1]);
        const float dpy = 0.5f * (m_pressure[curIdx + W] - m_pressure[curIdx - W]);
        if (std::abs(dpx) + std::abs(dpy) > 0.30f) {
            const float invRho = 1.0f / std::max(0.45f, props.density);
            vx -= dpx * invRho * 0.45f;
            vy -= dpy * invRho * 0.45f;
        }
    }

    // Aerodynamic drag coupling with Eulerian air velocity
    const float drag = props.dragCoeff;
    vx += drag * (m_airVx[curIdx] - vx);
    vy += drag * (m_airVy[curIdx] - vy);

    // Terminal velocity clamping
    const float maxSpd = (mat == MaterialId::Snow) ? 1.6f :
                         (props.phase == PhaseState::EmptyGas) ? 3.5f : 10.5f;
    vx = std::clamp(vx, -maxSpd, maxSpd);
    vy = std::clamp(vy, -maxSpd, maxSpd);

    m_vx[curIdx] = vx;
    m_vy[curIdx] = vy;

    // 2. Sub-pixel fractional position accumulation & DDA raycast stepping
    float fx = m_fx[curIdx] + vx;
    float fy = m_fy[curIdx] + vy;
    const int targetDx = static_cast<int>(std::trunc(fx));
    const int targetDy = static_cast<int>(std::trunc(fy));
    m_fx[curIdx] = fx;
    m_fy[curIdx] = fy;

    auto isSolidOrWall = [&](int tx, int ty) noexcept -> bool {
        if (tx < 0 || tx >= W || ty < 0 || ty >= H) return closed;
        if (closed && (tx == 0 || tx == W - 1 || ty == 0 || ty == H - 1)) return true;
        return getMaterial(fromIdx(m_mat[ty * W + tx])).isSolidObstacle();
    };

    auto tryStepInto = [&](int nx, int ny, bool isBallistic) -> int {
        // Returns: 1 = moved/swapped, 0 = blocked, -1 = exited open boundary
        if (nx < 0 || nx >= W || ny < 0 || ny >= H) {
            if (!closed) {
                convertCell(curIdx, MaterialId::Air, true);
                return -1;
            }
            return 0;
        }
        if (closed && (nx == 0 || nx == W - 1 || ny == 0 || ny == H - 1)) {
            return 0;
        }

        const int stepDx = nx - curX;
        const int stepDy = ny - curY;

        // Prevent diagonal corner-cutting through sealed 1-pixel solid walls
        if (stepDx != 0 && stepDy != 0 &&
            isSolidOrWall(curX + stepDx, curY) && isSolidOrWall(curX, curY + stepDy)) {
            return 0;
        }

        const int nextIdx = ny * W + nx;
        const MaterialId targetMat = fromIdx(m_mat[nextIdx]);
        if (targetMat == MaterialId::Air) {
            moveParticle(curIdx, nextIdx);
            if (isBallistic) {
                m_fx[nextIdx] -= static_cast<float>(stepDx);
                m_fy[nextIdx] -= static_cast<float>(stepDy);
            } else {
                if (stepDx != 0) m_fx[nextIdx] = 0.0f;
                if (stepDy != 0) m_fy[nextIdx] = 0.0f;
            }
            curX = nx;
            curY = ny;
            curIdx = nextIdx;
            return 1;
        }
        if (canDisplace(mat, targetMat)) {
            swapParticles(curIdx, nextIdx);
            if (isBallistic) {
                m_fx[nextIdx] -= static_cast<float>(stepDx);
                m_fy[nextIdx] -= static_cast<float>(stepDy);
            } else {
                if (stepDx != 0) m_fx[nextIdx] = 0.0f;
                if (stepDy != 0) m_fy[nextIdx] = 0.0f;
            }
            // Viscous damping when displacing heavier/liquid media
            m_vx[nextIdx] *= 0.86f;
            m_vy[nextIdx] *= 0.84f;
            curX = nx;
            curY = ny;
            curIdx = nextIdx;
            return 1;
        }

        // High-energy ballistic granular projectiles smash fragile solids (Plant, Ice, Glass, Wood)
        if (isBallistic && props.phase == PhaseState::Powder) {
            float breakThresh = 0.0f;
            float speedKeep = 0.88f;
            if (targetMat == MaterialId::Plant) { breakThresh = 22.0f; speedKeep = 0.92f; }
            else if (targetMat == MaterialId::Ice) { breakThresh = 31.0f; speedKeep = 0.90f; }
            else if (targetMat == MaterialId::Glass) { breakThresh = 31.0f; speedKeep = 0.90f; }
            else if (targetMat == MaterialId::Wood) { breakThresh = 32.0f; speedKeep = 0.88f; }

            if (breakThresh > 0.0f) {
                const float speedSq = m_vx[curIdx] * m_vx[curIdx] + m_vy[curIdx] * m_vy[curIdx];
                const float kinetic = props.density * speedSq;
                if (kinetic > breakThresh) {
                    const int prevIdx = curIdx;
                    const float pvx = m_vx[curIdx];
                    const float pvy = m_vy[curIdx];
                    convertCell(nextIdx, MaterialId::Air, true);
                    if (kinetic > 45.0f) {
                        m_tempK[nextIdx] = std::max(m_tempK[nextIdx], 560.0f);
                    }
                    moveParticle(curIdx, nextIdx);
                    m_fx[nextIdx] -= static_cast<float>(stepDx);
                    m_fy[nextIdx] -= static_cast<float>(stepDy);
                    m_vx[nextIdx] *= speedKeep;
                    m_vy[nextIdx] *= speedKeep;

                    // Emit incandescent impact spark in the vacated cell without stalling the projectile
                    if ((targetMat == MaterialId::Glass || targetMat == MaterialId::Wood) &&
                        kinetic > 38.0f && (rnd & 3u) == 0u) {
                        convertCell(prevIdx, MaterialId::Spark, false);
                        m_vx[prevIdx] = std::clamp(pvx * 1.02f, -10.5f, 10.5f);
                        m_vy[prevIdx] = std::clamp(pvy + ((rnd & 8u) ? 0.45f : -0.45f), -10.5f, 10.5f);
                        m_tempK[prevIdx] = 1750.0f;
                        m_movedTick[prevIdx] = tickParity;
                    }
                    curX = nx;
                    curY = ny;
                    curIdx = nextIdx;
                    return 1;
                }
            }
        }
        return 0;
    };

    bool movedBallistic = false;
    bool blocked = false;
    if (targetDx != 0 || targetDy != 0) {
        const int nSteps = std::min(16, std::max(std::abs(targetDx), std::abs(targetDy)));
        float accX = 0.0f;
        float accY = 0.0f;
        const float stepX = static_cast<float>(targetDx) / static_cast<float>(nSteps);
        const float stepY = static_cast<float>(targetDy) / static_cast<float>(nSteps);

        int prevRx = 0;
        int prevRy = 0;
        for (int s = 1; s <= nSteps; ++s) {
            accX += stepX;
            accY += stepY;
            const int rx = static_cast<int>(std::round(accX));
            const int ry = static_cast<int>(std::round(accY));
            const int ddx = rx - prevRx;
            const int ddy = ry - prevRy;
            prevRx = rx;
            prevRy = ry;
            if (ddx == 0 && ddy == 0) continue;

            const int res = tryStepInto(curX + ddx, curY + ddy, true);
            if (res == -1) return; // Particle left open boundary
            if (res == 1) {
                movedBallistic = true;
                continue;
            }

            // Blocked: if diagonal move blocked, try vertical-only then horizontal-only before stopping
            bool slid = false;
            if (ddx != 0 && ddy != 0) {
                const int ryOnly = tryStepInto(curX, curY + ddy, true);
                if (ryOnly == -1) return;
                if (ryOnly == 1) {
                    m_vx[curIdx] *= 0.25f;
                    m_fx[curIdx] = 0.0f;
                    movedBallistic = true;
                    slid = true;
                } else {
                    const int rxOnly = tryStepInto(curX + ddx, curY, true);
                    if (rxOnly == -1) return;
                    if (rxOnly == 1) {
                        m_vy[curIdx] *= 0.25f;
                        m_fy[curIdx] = 0.0f;
                        movedBallistic = true;
                        slid = true;
                    }
                }
            }
            if (!slid) {
                // Nitro shock sensitivity on violent impact
                const float speedSq = m_vx[curIdx] * m_vx[curIdx] + m_vy[curIdx] * m_vy[curIdx];
                if (mat == MaterialId::Nitro && speedSq > 11.5f) {
                    detonate(curX, curY, 10, 4.4f, 2500.0f);
                    return;
                }

                // Check two-body collision with another movable particle vs rigid wall bounce
                const int hitX = curX + ddx;
                const int hitY = curY + ddy;
                bool hitMovable = false;
                if (inBounds(hitX, hitY) && !(closed && (hitX == 0 || hitX == W - 1 || hitY == 0 || hitY == H - 1))) {
                    const int hitIdx = hitY * W + hitX;
                    const auto& hitProps = getMaterial(fromIdx(m_mat[hitIdx]));
                    if (hitProps.isMovable()) {
                        hitMovable = true;
                        const float norm = (ddx != 0 && ddy != 0) ? 0.70710678f : 1.0f;
                        const float nx = static_cast<float>(ddx) * norm;
                        const float ny = static_cast<float>(ddy) * norm;
                        const float v1n = m_vx[curIdx] * nx + m_vy[curIdx] * ny;
                        const float v2n = m_vx[hitIdx] * nx + m_vy[hitIdx] * ny;
                        const float vRel = v1n - v2n;
                        if (vRel > 0.65f) {
                            // Two-body impulse preserves forward slug coherence without destabilizing resting piles
                            const float d1 = std::max(0.1f, props.density);
                            const float d2 = std::max(0.1f, hitProps.density);
                            const float eColl = (props.phase == PhaseState::Powder && hitProps.phase == PhaseState::Powder)
                                                    ? ((vRel > 1.5f) ? 0.90f : 0.45f)
                                                    : 0.30f;
                            const float invSum = 1.0f / (d1 + d2);
                            const float dv1 = -(1.0f + eColl) * vRel * (d2 * invSum);
                            const float dv2 = +(1.0f + eColl) * vRel * (d1 * invSum);
                            m_vx[curIdx] = std::clamp(m_vx[curIdx] + dv1 * nx, -10.0f, 10.0f);
                            m_vy[curIdx] = std::clamp(m_vy[curIdx] + dv1 * ny, -10.0f, 10.0f);
                            m_vx[hitIdx] = std::clamp(m_vx[hitIdx] + dv2 * nx, -10.0f, 10.0f);
                            m_vy[hitIdx] = std::clamp(m_vy[hitIdx] + dv2 * ny, -10.0f, 10.0f);
                            if (props.phase == PhaseState::Liquid && vRel > 1.35f) {
                                const float splash = std::min(1.6f, vRel * 0.35f);
                                m_vx[curIdx] = std::clamp(m_vx[curIdx] + ((rnd & 1u) ? splash : -splash), -10.0f, 10.0f);
                            }
                        } else {
                            // Low-speed resting contact: damp approach without pumping resting neighbors
                            if (ddx != 0) m_vx[curIdx] *= 0.25f;
                            if (ddy != 0) m_vy[curIdx] *= 0.25f;
                        }
                        if (ddx != 0) m_fx[curIdx] = 0.0f;
                        if (ddy != 0) m_fy[curIdx] = 0.0f;
                    }
                }

                if (!hitMovable) {
                    const float rest = props.restitution;
                    if (props.phase == PhaseState::Liquid && std::abs(m_vy[curIdx]) > 1.35f) {
                        const float kick = std::abs(m_vy[curIdx]) * 0.40f;
                        m_vx[curIdx] += ((rnd & 1u) ? kick : -kick);
                    } else if (props.phase == PhaseState::Powder && std::abs(m_vy[curIdx]) > 1.8f) {
                        m_vx[curIdx] += ((rnd & 1u) ? 0.35f : -0.35f);
                    }
                    if (ddx != 0) {
                        m_vx[curIdx] = -m_vx[curIdx] * rest;
                        if (std::abs(m_vx[curIdx]) < 0.15f) m_vx[curIdx] = 0.0f;
                        m_fx[curIdx] = 0.0f;
                    }
                    if (ddy != 0) {
                        m_vy[curIdx] = -m_vy[curIdx] * rest;
                        if (std::abs(m_vy[curIdx]) < 0.15f) m_vy[curIdx] = 0.0f;
                        m_fy[curIdx] = 0.0f;
                    }
                }
                blocked = true;
                break;
            }
        }
    }

    // 3. Resting / Blocked Granular Angle-of-Repose & Liquid Hydrostatic Dispersion
    if (movedBallistic && !blocked) return;
    if (m_gravityMode == GravityMode::ZeroG) return;

    if (m_gravityMode == GravityMode::RadialCenter) {
        const int rx = (gx > 0.04f) ? 1 : ((gx < -0.04f) ? -1 : 0);
        const int ry = (gy > 0.04f) ? 1 : ((gy < -0.04f) ? -1 : 0);
        if (props.phase == PhaseState::Powder || props.phase == PhaseState::Liquid) {
            if ((rx != 0 || ry != 0) && tryStepInto(curX + rx, curY + ry, false) != 0) return;
            if (rx != 0 && tryStepInto(curX + rx, curY, false) != 0) return;
            if (ry != 0 && tryStepInto(curX, curY + ry, false) != 0) return;
            if (rx != 0 && m_vx[curIdx] * static_cast<float>(rx) > 0.0f) {
                m_vx[curIdx] = 0.0f;
                m_fx[curIdx] = 0.0f;
            }
            if (ry != 0 && m_vy[curIdx] * static_cast<float>(ry) > 0.0f) {
                m_vy[curIdx] = 0.0f;
                m_fy[curIdx] = 0.0f;
            }
        } else if (props.phase == PhaseState::EmptyGas || props.phase == PhaseState::Energy) {
            if (mat != MaterialId::Spark && (rx != 0 || ry != 0) && tryStepInto(curX - rx, curY - ry, false) != 0) return;
        }
        return;
    }

    const int gDirY = (gy >= 0.0f) ? 1 : -1;
    const int firstSide = ((rnd ^ static_cast<uint32_t>(m_stats.tick)) & 1u) ? 1 : -1;

    if (props.phase == PhaseState::Powder) {
        if (mat == MaterialId::Snow && (rnd & 1u) == 0u) {
            m_vy[curIdx] *= 0.5f;
            return;
        }

        // Try straight gravity direction first if fractional accumulator hadn't triggered a step yet
        if (tryStepInto(curX, curY + gDirY, false) != 0) return;

        // Supported below: clear resting downward velocity to keep resting piles stable
        if (m_vy[curIdx] * static_cast<float>(gDirY) > 0.0f) {
            m_vy[curIdx] = 0.0f;
            m_fy[curIdx] = 0.0f;
        }

        // Angle of repose diagonal slide
        if (tryStepInto(curX + firstSide, curY + gDirY, false) != 0) {
            m_vx[curIdx] = static_cast<float>(firstSide) * 0.30f;
            return;
        }
        if (tryStepInto(curX - firstSide, curY + gDirY, false) != 0) {
            m_vx[curIdx] = static_cast<float>(-firstSide) * 0.30f;
            return;
        }
        m_vx[curIdx] *= 0.45f;
        if (std::abs(m_vx[curIdx]) < 0.08f) {
            m_vx[curIdx] = 0.0f;
            m_fx[curIdx] = 0.0f;
        }
        return;
    }

    if (props.phase == PhaseState::Liquid) {
        // Vertical drop
        if (tryStepInto(curX, curY + gDirY, false) != 0) return;

        // Supported below: zero resting downward creep so pools stay calm without phantom splashes
        if (m_vy[curIdx] * static_cast<float>(gDirY) > 0.0f) {
            m_vy[curIdx] = 0.0f;
            m_fy[curIdx] = 0.0f;
        }

        // Pressure-driven upward hydrostatic head equalization (e.g. U-tube communicating vessels)
        const int belowY = curY + gDirY;
        const int aboveY = curY - gDirY;
        if (belowY >= 0 && belowY < H && aboveY >= 0 && aboveY < H) {
            if (m_airVy[curIdx] * static_cast<float>(gDirY) < -0.18f &&
                getMaterial(fromIdx(m_mat[belowY * W + curX])).phase == PhaseState::Liquid &&
                fromIdx(m_mat[aboveY * W + curX]) == MaterialId::Air) {
                if (tryStepInto(curX, aboveY, false) != 0) return;
            }
        }

        // Diagonal slide
        const int prefDir = (std::abs(m_airVx[curIdx]) > 0.14f)
                                ? (m_airVx[curIdx] > 0.0f ? 1 : -1)
                                : ((std::abs(m_vx[curIdx]) > 0.12f)
                                       ? (m_vx[curIdx] > 0.0f ? 1 : -1)
                                       : firstSide);
        if (tryStepInto(curX + prefDir, curY + gDirY, false) != 0) {
            m_vx[curIdx] = static_cast<float>(prefDir) * 0.45f;
            return;
        }
        if (tryStepInto(curX - prefDir, curY + gDirY, false) != 0) {
            m_vx[curIdx] = static_cast<float>(-prefDir) * 0.45f;
            return;
        }

        // Hydrostatic horizontal dispersion: seek nearest drop-off first, else level evenly without jitter
        const int maxSpread = std::max(1, props.dispersion);
        struct SpreadProbe {
            int dir = 0;
            int farthestX = 0;
            int dropDist = 999;
            bool exitOpen = false;
        };
        auto probeDir = [&](int dir) -> SpreadProbe {
            SpreadProbe p;
            p.dir = dir;
            p.farthestX = curX;
            const int dropY = curY + gDirY;
            for (int d = 1; d <= maxSpread; ++d) {
                const int tx = curX + dir * d;
                if (tx < 0 || tx >= W) {
                    if (!closed) {
                        p.exitOpen = true;
                        p.dropDist = d;
                    }
                    break;
                }
                if (closed && (tx == 0 || tx == W - 1)) break;

                const MaterialId tm = fromIdx(m_mat[curY * W + tx]);
                if (!canDisplace(mat, tm)) break;

                p.farthestX = tx;
                if (dropY >= 0 && dropY < H && canDisplace(mat, fromIdx(m_mat[dropY * W + tx]))) {
                    p.dropDist = d;
                    break;
                }
            }
            return p;
        };

        const SpreadProbe pA = probeDir(prefDir);
        const SpreadProbe pB = probeDir(-prefDir);

        auto executeProbe = [&](const SpreadProbe& p, float flowSpeed) -> bool {
            if (p.exitOpen) {
                convertCell(curIdx, MaterialId::Air, true);
                return true;
            }
            if (p.farthestX != curX) {
                const int res = tryStepInto(p.farthestX, curY, false);
                if (res != 0) {
                    if (res == 1) m_vx[curIdx] = static_cast<float>(p.dir) * flowSpeed;
                    return true;
                }
            }
            return false;
        };

        // If at least one direction leads to a downward drop, flow directly toward the closer drop
        if (pA.dropDist <= maxSpread || pB.dropDist <= maxSpread) {
            const SpreadProbe& first = (pA.dropDist <= pB.dropDist) ? pA : pB;
            const SpreadProbe& second = (pA.dropDist <= pB.dropDist) ? pB : pA;
            if (executeProbe(first, 0.45f)) return;
            if (second.dropDist <= maxSpread && executeProbe(second, 0.45f)) return;
        } else {
            // Flat surface: spread if driven by fluid/momentum or adjacent liquid pressure, otherwise rest calmly
            auto shouldSpreadFlat = [&](int dir) -> bool {
                if (std::abs(m_airVx[curIdx]) > 0.16f || std::abs(m_vx[curIdx]) > 0.22f) return true;
                const int backX = curX - dir;
                if (inBounds(backX, curY) && getMaterial(fromIdx(m_mat[curY * W + backX])).phase == PhaseState::Liquid) {
                    return true;
                }
                if (aboveY >= 0 && aboveY < H && getMaterial(fromIdx(m_mat[aboveY * W + curX])).phase == PhaseState::Liquid) {
                    return true;
                }
                return false;
            };
            if (pA.farthestX != curX && shouldSpreadFlat(pA.dir) && executeProbe(pA, 0.16f)) return;
            if (pB.farthestX != curX && shouldSpreadFlat(pB.dir) && executeProbe(pB, 0.16f)) return;
        }

        m_vx[curIdx] *= 0.40f;
        if (std::abs(m_vx[curIdx]) < 0.08f) {
            m_vx[curIdx] = 0.0f;
            m_fx[curIdx] = 0.0f;
        }
        return;
    }

    if (props.phase == PhaseState::EmptyGas || props.phase == PhaseState::Energy) {
        if (mat == MaterialId::Spark) return;
        const bool coolSmoke = (mat == MaterialId::Smoke && m_tempK[curIdx] <= m_ambientTempK + 15.0f);
        if (!coolSmoke) {
            const int riseDirY = (gy >= 0.0f) ? -1 : 1;
            if (tryStepInto(curX, curY + riseDirY, false) != 0) return;
            if (tryStepInto(curX + firstSide, curY + riseDirY, false) != 0) return;
            if (tryStepInto(curX - firstSide, curY + riseDirY, false) != 0) return;
        }
        if (tryStepInto(curX + firstSide, curY, false) != 0) return;
        tryStepInto(curX - firstSide, curY, false);
    }
    m_movedTick[curIdx] = tickParity;
}

void SimWorld::stepParticles() {
    const int W = m_width;
    const int H = m_height;
    const uint8_t parity = m_tickParity;
    const bool invertScan = (m_gravityMode == GravityMode::Inverted);

    auto scanGasesAndEnergy = [&](int y) {
        const bool leftToRight = (((y + static_cast<int>(m_stats.tick)) & 1) == 0);
        const int xStart = leftToRight ? 0 : (W - 1);
        const int xEnd = leftToRight ? W : -1;
        const int xStep = leftToRight ? 1 : -1;
        for (int x = xStart; x != xEnd; x += xStep) {
            const int i = y * W + x;
            if (m_movedTick[i] == parity) continue;
            const MaterialId m = fromIdx(m_mat[i]);
            if (m == MaterialId::Air) continue;
            const auto phase = getMaterial(m).phase;
            if (phase == PhaseState::EmptyGas || phase == PhaseState::Energy) {
                updateParticleAt(x, y, parity);
            }
        }
    };

    if (!invertScan) {
        // Pass 1: Bottom-up for falling powders/liquids (cohesive columns)
        for (int y = H - 1; y >= 0; --y) {
            const bool leftToRight = (((y + static_cast<int>(m_stats.tick)) & 1) == 0);
            const int xStart = leftToRight ? 0 : (W - 1);
            const int xEnd = leftToRight ? W : -1;
            const int xStep = leftToRight ? 1 : -1;
            for (int x = xStart; x != xEnd; x += xStep) {
                const int i = y * W + x;
                if (m_movedTick[i] == parity) continue;
                const auto phase = getMaterial(fromIdx(m_mat[i])).phase;
                if (phase == PhaseState::Powder || phase == PhaseState::Liquid ||
                    fromIdx(m_mat[i]) == MaterialId::Spark) {
                    updateParticleAt(x, y, parity);
                }
            }
        }
        // Pass 2: Top-down for rising gases, fire, and plasma
        for (int y = 0; y < H; ++y) {
            scanGasesAndEnergy(y);
        }
    } else {
        // Inverted gravity: Top-down for powders/liquids first, then bottom-up
        for (int y = 0; y < H; ++y) {
            const bool leftToRight = (((y + static_cast<int>(m_stats.tick)) & 1) == 0);
            const int xStart = leftToRight ? 0 : (W - 1);
            const int xEnd = leftToRight ? W : -1;
            const int xStep = leftToRight ? 1 : -1;
            for (int x = xStart; x != xEnd; x += xStep) {
                const int i = y * W + x;
                if (m_movedTick[i] == parity) continue;
                const auto phase = getMaterial(fromIdx(m_mat[i])).phase;
                if (phase == PhaseState::Powder || phase == PhaseState::Liquid ||
                    fromIdx(m_mat[i]) == MaterialId::Spark) {
                    updateParticleAt(x, y, parity);
                }
            }
        }
        for (int y = H - 1; y >= 0; --y) {
            scanGasesAndEnergy(y);
        }
    }
}

} // namespace sim
