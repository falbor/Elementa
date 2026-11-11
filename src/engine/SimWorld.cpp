#include "sim/SimWorld.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>

namespace sim {

namespace {
using Clock = std::chrono::steady_clock;

double msBetween(Clock::time_point a, Clock::time_point b) noexcept {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
} // namespace

SimWorld::SimWorld(int width, int height, unsigned threads, uint32_t seed)
    : m_width(std::max(16, width)), m_height(std::max(16, height)), m_count(m_width * m_height), m_pool(threads),
      m_rngState(seed ? seed : 0x5EED1234u) {

    const size_t n = static_cast<size_t>(m_count);
    m_mat.assign(n, toIdx(MaterialId::Air));
    m_vx.assign(n, 0.0f);
    m_vy.assign(n, 0.0f);
    m_fx.assign(n, 0.0f);
    m_fy.assign(n, 0.0f);
    m_life.assign(n, 0.0f);
    m_charge.assign(n, 0.0f);
    m_colorSeed.assign(n, 0);
    m_movedTick.assign(n, 0);
    m_solidMask.assign(n, 0);

    m_airVx.assign(n, 0.0f);
    m_airVy.assign(n, 0.0f);
    m_airVxTmp.assign(n, 0.0f);
    m_airVyTmp.assign(n, 0.0f);
    m_pressure.assign(n, 0.0f);
    m_div.assign(n, 0.0f);
    m_blastDiv.assign(n, 0.0f);

    m_tempK.assign(n, m_ambientTempK);
    m_tempTmp.assign(n, m_ambientTempK);
    m_chargeTmp.assign(n, 0.0f);

    m_lightR.assign(n, 0.0f);
    m_lightG.assign(n, 0.0f);
    m_lightB.assign(n, 0.0f);
    m_lightTmpR.assign(n, 0.0f);
    m_lightTmpG.assign(n, 0.0f);
    m_lightTmpB.assign(n, 0.0f);

    m_framebuffer.assign(n, 0xFF000000u);

    clear(MaterialId::Air);
}

uint32_t SimWorld::nextRand() noexcept {
    uint32_t x = m_rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_rngState = x;
    return x;
}

uint32_t SimWorld::hashCoord(int x, int y, uint64_t tick) const noexcept {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(tick) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

void SimWorld::clear(MaterialId fill) {
    const auto &props = getMaterial(fill);
    const uint8_t matIdx = toIdx(fill);
    for (int i = 0; i < m_count; ++i) {
        m_mat[i] = matIdx;
        m_vx[i] = 0.0f;
        m_vy[i] = 0.0f;
        m_fx[i] = 0.0f;
        m_fy[i] = 0.0f;
        m_life[i] = (fill == MaterialId::Fire)     ? 28.0f
                    : (fill == MaterialId::Spark)  ? 14.0f
                    : (fill == MaterialId::Plasma) ? 45.0f
                    : (fill == MaterialId::Acid)   ? 100.0f
                                                   : 0.0f;
        m_charge[i] = (fill == MaterialId::Spark || fill == MaterialId::Plasma) ? 1.0f : 0.0f;
        m_colorSeed[i] = static_cast<uint8_t>(nextRand() & 0xFFu);
        m_movedTick[i] = 0;

        m_airVx[i] = 0.0f;
        m_airVy[i] = 0.0f;
        m_pressure[i] = 0.0f;
        m_div[i] = 0.0f;
        m_blastDiv[i] = 0.0f;

        m_tempK[i] = (fill == MaterialId::Air) ? m_ambientTempK : props.defaultTempK;
        m_lightR[i] = 0.14f;
        m_lightG[i] = 0.15f;
        m_lightB[i] = 0.18f;
    }
    m_stats = SimStats{};
    m_stats.minTempK = m_ambientTempK;
    m_stats.maxTempK = m_ambientTempK;
}

void SimWorld::setCell(int x, int y, MaterialId mat, bool resetVelocity) {
    if (!inBounds(x, y)) return;
    const int i = index(x, y);
    const auto &props = getMaterial(mat);
    m_mat[i] = toIdx(mat);
    m_tempK[i] = (mat == MaterialId::Air) ? m_ambientTempK : props.defaultTempK;
    m_colorSeed[i] = static_cast<uint8_t>(nextRand() & 0xFFu);
    m_fx[i] = 0.0f;
    m_fy[i] = 0.0f;

    if (resetVelocity) {
        m_vx[i] = 0.0f;
        m_vy[i] = 0.0f;
    }

    if (mat == MaterialId::Fire) {
        m_life[i] = 18.0f + static_cast<float>(nextRand() % 28);
        m_vy[i] = -0.35f;
    } else if (mat == MaterialId::Spark) {
        m_life[i] = 10.0f + static_cast<float>(nextRand() % 14);
        m_charge[i] = 1.0f;
        const float angle = static_cast<float>(nextRand() % 360) * 0.01745329f;
        const float spd = 1.4f + static_cast<float>(nextRand() % 100) * 0.025f;
        m_vx[i] = std::cos(angle) * spd;
        m_vy[i] = std::sin(angle) * spd;
    } else if (mat == MaterialId::Plasma) {
        m_life[i] = 35.0f + static_cast<float>(nextRand() % 35);
        m_charge[i] = 1.2f;
    } else if (mat == MaterialId::Acid) {
        m_life[i] = 100.0f;
        m_charge[i] = 0.0f;
    } else if (mat == MaterialId::Smoke || mat == MaterialId::Steam) {
        m_life[i] = 280.0f + static_cast<float>(nextRand() % 140);
        m_charge[i] = 0.0f;
    } else {
        m_life[i] = 0.0f;
        m_charge[i] = 0.0f;
    }
}

void SimWorld::setCellTemp(int x, int y, float tempK) {
    if (!inBounds(x, y)) return;
    m_tempK[index(x, y)] = std::clamp(tempK, 1.0f, 8000.0f);
}

void SimWorld::setCellVelocity(int x, int y, float vx, float vy) {
    if (!inBounds(x, y)) return;
    const int i = index(x, y);
    m_vx[i] = std::clamp(vx, -12.0f, 12.0f);
    m_vy[i] = std::clamp(vy, -12.0f, 12.0f);
}

void SimWorld::setCellCharge(int x, int y, float charge) {
    if (!inBounds(x, y)) return;
    m_charge[index(x, y)] = std::clamp(charge, -5.0f, 5.0f);
}

void SimWorld::convertCell(int idx, MaterialId newMat, bool keepTemp) noexcept {
    const auto &props = getMaterial(newMat);
    m_mat[idx] = toIdx(newMat);
    if (!keepTemp) {
        m_tempK[idx] = props.defaultTempK;
    }
    m_fx[idx] = 0.0f;
    m_fy[idx] = 0.0f;
    if (newMat == MaterialId::Fire) {
        m_life[idx] = 18.0f + static_cast<float>(nextRand() % 24);
    } else if (newMat == MaterialId::Smoke || newMat == MaterialId::Steam) {
        m_life[idx] = 280.0f + static_cast<float>(nextRand() % 140);
    } else if (newMat == MaterialId::Spark) {
        m_life[idx] = 10.0f + static_cast<float>(nextRand() % 12);
        m_charge[idx] = 1.0f;
    } else if (newMat == MaterialId::Acid) {
        m_life[idx] = 100.0f;
    } else {
        m_life[idx] = 0.0f;
    }
}

void SimWorld::paintCircle(int cx, int cy, int radius, MaterialId mat, bool overwriteSolid) {
    const int r = std::max(0, radius);
    const int r2 = r * r;
    const auto &targetProps = getMaterial(mat);
    const bool paintingSolidOrErase = (mat == MaterialId::Air) || targetProps.isSolidObstacle();

    for (int dy = -r; dy <= r; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= m_height) continue;
        for (int dx = -r; dx <= r; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= m_width) continue;
            if (dx * dx + dy * dy > r2) continue;

            const int i = index(x, y);
            const MaterialId existing = fromIdx(m_mat[i]);
            if (!overwriteSolid && !paintingSolidOrErase && getMaterial(existing).isSolidObstacle()) {
                continue;
            }
            // Add slight organic scatter for powders/gases on large brushes so streams mix naturally
            if (r >= 3 && (targetProps.phase == PhaseState::Powder || targetProps.phase == PhaseState::Energy)) {
                if ((nextRand() & 3u) == 0u && existing == mat) continue;
            }
            setCell(x, y, mat);
        }
    }
}

void SimWorld::paintLine(int x0, int y0, int x1, int y1, int radius, MaterialId mat, bool overwriteSolid) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int x = x0;
    int y = y0;

    while (true) {
        paintCircle(x, y, radius, mat, overwriteSolid);
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

void SimWorld::applyToolCircle(ToolMode tool, int cx, int cy, int radius, float dxStroke, float dyStroke,
                               MaterialId paintMat) {
    if (tool == ToolMode::Material) {
        paintCircle(cx, cy, radius, paintMat, false);
        return;
    }
    if (tool == ToolMode::Erase) {
        paintCircle(cx, cy, radius, MaterialId::Air, true);
        return;
    }

    const int r = std::max(1, radius);
    const int r2 = r * r;
    for (int dy = -r; dy <= r; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= m_height) continue;
        for (int dx = -r; dx <= r; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= m_width) continue;
            const int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;

            const int i = index(x, y);
            const float falloff = 1.0f - std::sqrt(static_cast<float>(d2)) / static_cast<float>(r + 1);
            const MaterialProps &props = getMaterial(fromIdx(m_mat[i]));

            switch (tool) {
            case ToolMode::Wind: {
                float fx = dxStroke * 0.45f;
                float fy = dyStroke * 0.45f;
                if (std::abs(fx) + std::abs(fy) < 0.05f) {
                    fx = static_cast<float>(dx) * 0.18f;
                    fy = static_cast<float>(dy) * 0.18f - 0.55f;
                }
                if (!props.isSolidObstacle()) {
                    m_airVx[i] = std::clamp(m_airVx[i] + fx * falloff, -12.0f, 12.0f);
                    m_airVy[i] = std::clamp(m_airVy[i] + fy * falloff, -12.0f, 12.0f);
                    if (props.isMovable()) {
                        m_vx[i] = std::clamp(m_vx[i] + fx * falloff * 0.7f, -12.0f, 12.0f);
                        m_vy[i] = std::clamp(m_vy[i] + fy * falloff * 0.7f, -12.0f, 12.0f);
                    }
                }
                break;
            }
            case ToolMode::Heat:
                m_tempK[i] = std::clamp(m_tempK[i] + 180.0f * falloff, 1.0f, 6500.0f);
                break;
            case ToolMode::Cryo:
                m_tempK[i] = std::clamp(m_tempK[i] - 120.0f * falloff, 40.0f, 6500.0f);
                break;
            case ToolMode::SparkTool:
                m_charge[i] = std::clamp(m_charge[i] + 1.4f * falloff, -5.0f, 5.0f);
                m_tempK[i] = std::clamp(m_tempK[i] + 90.0f * falloff, 1.0f, 6500.0f);
                if (fromIdx(m_mat[i]) == MaterialId::Air && (nextRand() % 5u) == 0u) {
                    setCell(x, y, MaterialId::Spark, false);
                    m_charge[i] = 1.5f;
                }
                break;
            case ToolMode::Attractor:
            case ToolMode::Repulsor: {
                const float dist = std::sqrt(static_cast<float>(d2)) + 0.5f;
                const float dirSign = (tool == ToolMode::Attractor) ? -1.0f : 1.0f;
                const float strength = dirSign * 2.2f * falloff / std::max(1.0f, dist * 0.25f);
                const float nx = static_cast<float>(dx) / dist;
                const float ny = static_cast<float>(dy) / dist;
                if (!props.isSolidObstacle()) {
                    m_airVx[i] = std::clamp(m_airVx[i] + nx * strength, -12.0f, 12.0f);
                    m_airVy[i] = std::clamp(m_airVy[i] + ny * strength, -12.0f, 12.0f);
                    if (props.isMovable()) {
                        m_vx[i] = std::clamp(m_vx[i] + nx * strength * 0.85f, -12.0f, 12.0f);
                        m_vy[i] = std::clamp(m_vy[i] + ny * strength * 0.85f, -12.0f, 12.0f);
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }
}

void SimWorld::applyToolLine(ToolMode tool, int x0, int y0, int x1, int y1, int radius, MaterialId paintMat) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int x = x0;
    int y = y0;
    const float strokeDx = static_cast<float>(x1 - x0);
    const float strokeDy = static_cast<float>(y1 - y0);

    while (true) {
        applyToolCircle(tool, x, y, radius, strokeDx, strokeDy, paintMat);
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

void SimWorld::detonate(int cx, int cy, int radius, float blastStrength, float peakTempK) {
    const int r = std::max(2, radius);
    const int r2 = r * r;
    const int rInner2 = r2 / 3;
    const int rShock = r * 2;
    const int rShock2 = rShock * rShock;

    const int x0 = std::max(0, cx - rShock);
    const int x1 = std::min(m_width - 1, cx + rShock);
    const int y0 = std::max(0, cy - rShock);
    const int y1 = std::min(m_height - 1, cy + rShock);
    if (x0 > x1 || y0 > y1) return;

    const int bw = x1 - x0 + 1;
    const int bh = y1 - y0 + 1;
    const size_t boxCount = static_cast<size_t>(bw) * static_cast<size_t>(bh);

    // Snapshot pre-detonation solid occlusion codes so blast raycasts are symmetric and scan-order independent:
    // 0 = open, 1 = unbreakable StaticSolid, 2 = inner Stone (falloff > 0.72), 3 = fragile wall inside r^2/3, 4 =
    // fragile wall outside r^2/3
    std::array<uint8_t, 4096> stackCodes{};
    std::vector<uint8_t> heapCodes;
    uint8_t *codes = stackCodes.data();
    if (boxCount > stackCodes.size()) {
        heapCodes.resize(boxCount);
        codes = heapCodes.data();
    }

    for (int y = y0; y <= y1; ++y) {
        const int dy = y - cy;
        const int rowOff = (y - y0) * bw;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - cx;
            const int d2 = dx * dx + dy * dy;
            uint8_t code = 0;
            if (d2 <= rShock2) {
                const MaterialId m = fromIdx(m_mat[index(x, y)]);
                if (getMaterial(m).isSolidObstacle()) {
                    if (d2 <= rInner2) {
                        if (m == MaterialId::Wood || m == MaterialId::Plant || m == MaterialId::Glass ||
                            m == MaterialId::Ice) {
                            code = 3;
                        } else if (m == MaterialId::Stone) {
                            const float dist = std::sqrt(static_cast<float>(d2));
                            const float falloff = 1.0f - dist / static_cast<float>(r + 1);
                            code = (falloff > 0.72f) ? 2 : 1;
                        } else {
                            code = 1;
                        }
                    } else {
                        code = (m == MaterialId::Wood || m == MaterialId::Plant || m == MaterialId::Glass ||
                                m == MaterialId::Ice)
                                   ? 4
                                   : 1;
                    }
                }
            }
            codes[rowOff + (x - x0)] = code;
        }
    }

    auto codeAt = [&](int gx, int gy) noexcept -> uint8_t {
        if (gx < x0 || gx > x1 || gy < y0 || gy > y1) return 1;
        return codes[(gy - y0) * bw + (gx - x0)];
    };

    auto codeTrans = [](uint8_t c) noexcept -> float {
        if (c == 0) return 1.0f;
        if (c == 3) return 0.45f;
        if (c == 2) return 0.25f;
        return 0.0f;
    };

    // Exact integer Amanatides-Woo grid raycast from (cx, cy) to (tx, ty) checking intermediate StaticSolid cells
    // and preventing diagonal corner tunneling across sealed 1-px diagonal barriers.
    auto computeBlastTransmission = [&](int tx, int ty) noexcept -> float {
        if (tx == cx && ty == cy) return 1.0f;
        const int dx = tx - cx;
        const int dy = ty - cy;
        const int sx = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
        const int sy = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
        const int adx = std::abs(dx);
        const int ady = std::abs(dy);

        int x = cx;
        int y = cy;
        int tMaxX = ady;
        int tMaxY = adx;
        const int tDeltaX = 2 * ady;
        const int tDeltaY = 2 * adx;
        float trans = 1.0f;

        while (x != tx || y != ty) {
            if (tMaxX < tMaxY) {
                x += sx;
                tMaxX += tDeltaX;
            } else if (tMaxY < tMaxX) {
                y += sy;
                tMaxY += tDeltaY;
            } else {
                const uint8_t cX = codeAt(x + sx, y);
                const uint8_t cY = codeAt(x, y + sy);
                if (cX != 0 && cY != 0) {
                    trans *= std::min(codeTrans(cX), codeTrans(cY));
                    if (trans < 0.01f) return 0.0f;
                }
                x += sx;
                y += sy;
                tMaxX += tDeltaX;
                tMaxY += tDeltaY;
            }

            if (x == tx && y == ty) {
                return trans;
            }

            const uint8_t c = codeAt(x, y);
            if (c != 0) {
                trans *= codeTrans(c);
                if (trans < 0.01f) return 0.0f;
            }
        }
        return trans;
    };

    for (int y = y0; y <= y1; ++y) {
        const int dy = y - cy;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - cx;
            const int d2 = dx * dx + dy * dy;
            if (d2 > rShock2) continue;

            const int i = index(x, y);
            const MaterialId existing = fromIdx(m_mat[i]);
            if (existing == MaterialId::Bedrock) continue;
            const auto &props = getMaterial(existing);
            if (d2 > r2 && !props.isMovable()) continue;

            const float trans = computeBlastTransmission(x, y);
            if (trans <= 0.0f) continue;

            const float dist = std::sqrt(static_cast<float>(d2));
            const float shockFalloff = 1.0f - dist / static_cast<float>(rShock + 1);

            if (dist > 0.1f && props.isMovable()) {
                const float nx = static_cast<float>(dx) / dist;
                const float ny = static_cast<float>(dy) / dist;
                const float invRho = 1.0f / std::max(0.45f, props.density);
                const float kick = blastStrength * trans * shockFalloff * 2.2f * invRho;
                m_vx[i] = std::clamp(m_vx[i] + nx * kick, -12.0f, 12.0f);
                m_vy[i] = std::clamp(m_vy[i] + ny * kick - 0.22f * shockFalloff * trans, -12.0f, 12.0f);
            }

            if (d2 > r2) continue;
            const float falloff = 1.0f - dist / static_cast<float>(r + 1);

            // Inject expansion divergence & heat inside core blast radius
            m_blastDiv[i] += blastStrength * falloff * trans;
            m_tempK[i] = std::max(m_tempK[i], kAmbientTempK + (peakTempK - kAmbientTempK) * falloff * trans);

            // Inner fireball breaks fragile walls (except bedrock/obsidian/metal)
            if (d2 <= rInner2 && trans >= 0.40f) {
                if (existing == MaterialId::Air || existing == MaterialId::Smoke || existing == MaterialId::Gunpowder ||
                    existing == MaterialId::Nitro || existing == MaterialId::Methane) {
                    setCell(x, y, (nextRand() % 4u == 0u) ? MaterialId::Spark : MaterialId::Fire, false);
                    m_tempK[i] = std::max(m_tempK[i], kAmbientTempK + (peakTempK - kAmbientTempK) * trans);
                } else if (existing == MaterialId::Wood || existing == MaterialId::Plant ||
                           existing == MaterialId::Glass || existing == MaterialId::Ice) {
                    setCell(x, y, MaterialId::Fire, false);
                    m_tempK[i] = std::max(m_tempK[i], kAmbientTempK + (peakTempK * 0.85f - kAmbientTempK) * trans);
                } else if (existing == MaterialId::Stone && falloff * trans > 0.72f) {
                    setCell(x, y, MaterialId::Sand, false);
                    m_tempK[i] = std::max(m_tempK[i], kAmbientTempK + (peakTempK * 0.75f - kAmbientTempK) * trans);
                }
            }
        }
    }
}

void SimWorld::step() {
    const auto t0 = Clock::now();
    if (m_tickParity == 255) {
        std::fill(m_movedTick.begin(), m_movedTick.end(), 0);
        m_tickParity = 1;
    } else {
        ++m_tickParity;
    }

    stepFluidField();
    const auto t1 = Clock::now();

    stepThermoElectric();
    const auto t2 = Clock::now();

    stepReactions();
    const auto t3 = Clock::now();

    stepParticles();
    const auto t4 = Clock::now();

    stepLighting();
    const auto t5 = Clock::now();

    computeStats();
    const auto t6 = Clock::now();

    ++m_stats.tick;
    m_stats.fluidMs = msBetween(t0, t1);
    m_stats.thermoMs = msBetween(t1, t2);
    m_stats.reactionMs = msBetween(t2, t3);
    m_stats.particleMs = msBetween(t3, t4);
    m_stats.lightMs = msBetween(t4, t5);
    m_stats.totalStepMs = msBetween(t0, t6);
}

void SimWorld::computeStats() {
    uint32_t active = 0;
    double thermal = 0.0;
    double kinetic = 0.0;
    float minT = 1e9f;
    float maxT = -1e9f;
    float maxP = 0.0f;

    for (int i = 0; i < m_count; ++i) {
        const MaterialId m = fromIdx(m_mat[i]);
        const auto &props = getMaterial(m);
        const float T = m_tempK[i];
        thermal += static_cast<double>(props.heatCapacity) * static_cast<double>(T);

        const float av2 = m_airVx[i] * m_airVx[i] + m_airVy[i] * m_airVy[i];
        kinetic += 0.5 * 0.02 * static_cast<double>(av2);

        if (m != MaterialId::Air) {
            ++active;
            if (props.isMovable()) {
                const float pv2 = m_vx[i] * m_vx[i] + m_vy[i] * m_vy[i];
                kinetic += 0.5 * static_cast<double>(props.density) * static_cast<double>(pv2);
            }
        }
        if (T < minT) minT = T;
        if (T > maxT) maxT = T;
        const float ap = std::abs(m_pressure[i]);
        if (ap > maxP) maxP = ap;
    }

    m_stats.activeParticles = active;
    m_stats.totalThermalEnergy = thermal;
    m_stats.totalKineticEnergy = kinetic;
    m_stats.minTempK = minT;
    m_stats.maxTempK = maxT;
    m_stats.maxPressure = maxP;
}

PixelInfo SimWorld::inspect(int x, int y) const noexcept {
    PixelInfo info;
    if (!inBounds(x, y)) return info;
    const int i = index(x, y);
    info.x = x;
    info.y = y;
    info.material = fromIdx(m_mat[i]);
    info.tempK = m_tempK[i];
    info.pressure = m_pressure[i];
    info.vx = m_vx[i];
    info.vy = m_vy[i];
    info.airVx = m_airVx[i];
    info.airVy = m_airVy[i];
    info.charge = m_charge[i];
    info.lightR = m_lightR[i];
    info.lightG = m_lightG[i];
    info.lightB = m_lightB[i];
    info.life = m_life[i];
    return info;
}

uint32_t SimWorld::countMaterial(MaterialId mat) const noexcept {
    const uint8_t target = toIdx(mat);
    uint32_t count = 0;
    for (int i = 0; i < m_count; ++i) {
        if (m_mat[i] == target) ++count;
    }
    return count;
}

double SimWorld::computeTotalThermalEnergy() const noexcept {
    double sum = 0.0;
    for (int i = 0; i < m_count; ++i) {
        sum += static_cast<double>(getMaterial(fromIdx(m_mat[i])).heatCapacity) * static_cast<double>(m_tempK[i]);
    }
    return sum;
}

bool SimWorld::savePPM(const std::string &path, ViewMode mode) {
    render(mode);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << m_width << " " << m_height << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(m_count) * 3);
    for (int i = 0; i < m_count; ++i) {
        const uint32_t c = m_framebuffer[i];
        rgb[i * 3 + 0] = static_cast<uint8_t>((c >> 16) & 0xFFu);
        rgb[i * 3 + 1] = static_cast<uint8_t>((c >> 8) & 0xFFu);
        rgb[i * 3 + 2] = static_cast<uint8_t>(c & 0xFFu);
    }
    out.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}

void SimWorld::loadPreset(PresetId preset) {
    clear(MaterialId::Air);
    m_gravityMode = GravityMode::Earth;
    m_boundaryMode = BoundaryMode::Closed;

    const int W = m_width;
    const int H = m_height;
    const int floorH = std::max(3, H / 25);
    const int floorY = H - floorH;

    auto fillRect = [&](int x0, int y0, int x1, int y1, MaterialId mat) {
        const int lx = std::clamp(x0, 0, W - 1);
        const int rx = std::clamp(x1, 0, W - 1);
        const int ty = std::clamp(y0, 0, H - 1);
        const int by = std::clamp(y1, 0, H - 1);
        if (lx > rx || ty > by) return;
        for (int y = ty; y <= by; ++y) {
            for (int x = lx; x <= rx; ++x) {
                setCell(x, y, mat);
            }
        }
    };

    switch (preset) {
    case PresetId::Laboratory: {
        // Bedrock floor
        fillRect(0, floorY, W - 1, H - 1, MaterialId::Bedrock);

        const int wall = std::max(2, std::min(W, H) / 36);

        // Left: Open V-funnel hourglass pouring Sand & Salt into Water/Oil tank
        const int funnelCx = W * 18 / 100;
        const int funnelTopY = H * 14 / 100;
        const int funnelBotY = H * 40 / 100;
        const int funnelHalfTop = std::max(5, W * 10 / 100);
        const int throatHalf = std::max(2, W * 2 / 100);
        paintLine(funnelCx - funnelHalfTop, funnelTopY, funnelCx - throatHalf, funnelBotY, 1, MaterialId::Stone);
        paintLine(funnelCx + funnelHalfTop, funnelTopY, funnelCx + throatHalf, funnelBotY, 1, MaterialId::Stone);
        fillRect(funnelCx - funnelHalfTop / 2, H * 8 / 100, funnelCx + funnelHalfTop / 2, H * 20 / 100,
                 MaterialId::Sand);
        fillRect(funnelCx - funnelHalfTop / 3, H * 3 / 100, funnelCx + funnelHalfTop / 3, H * 7 / 100,
                 MaterialId::Salt);

        // Water & Oil tank strictly below hourglass
        const int tankL = std::max(2, W * 5 / 100);
        const int tankR = W * 32 / 100;
        const int tankTop = H * 48 / 100;
        const int oilTop = H * 56 / 100;
        const int waterTop = H * 68 / 100;
        fillRect(tankL, tankTop, tankL + wall - 1, floorY - 1, MaterialId::Glass);
        fillRect(tankR - wall + 1, tankTop, tankR, floorY - 1, MaterialId::Glass);
        fillRect(tankL + wall, waterTop, tankR - wall, floorY - 1, MaterialId::Water);
        fillRect(tankL + wall, oilTop, tankR - wall, waterTop - 1, MaterialId::Oil);

        // Center: Geothermal magma chamber + metal heat bridge heating an ice/water boiler
        const int magL = W * 38 / 100;
        const int magR = W * 62 / 100;
        const int magTop = H * 64 / 100;
        fillRect(magL, magTop, magR, floorY - 1, MaterialId::Stone);
        fillRect(magL + wall, magTop + wall, magR - wall, floorY - 1 - wall, MaterialId::Lava);

        const int boilL = W * 40 / 100;
        const int boilR = W * 60 / 100;
        const int boilTop = H * 10 / 100;
        const int boilBot = H * 38 / 100;
        fillRect(boilL, boilTop, boilR, boilBot, MaterialId::Glass);
        fillRect(boilL + wall, boilTop + wall, boilR - wall, boilBot - wall, MaterialId::Ice);

        // Copper thermal bridge conducting magma heat up through the boiler floor into the Ice
        fillRect(W * 48 / 100, boilBot - wall + 1, W * 52 / 100, magTop + wall + 2, MaterialId::Metal);

        // Right: Acid bath dissolving metal + elevated Wood platform with Gunpowder fuse
        const int acidL = W * 68 / 100;
        const int acidR = W * 92 / 100;
        const int acidTop = H * 70 / 100;
        fillRect(acidL, acidTop, acidR, floorY - 1, MaterialId::Stone);
        fillRect(acidL + wall, acidTop + wall, acidR - wall, floorY - 1 - wall, MaterialId::Acid);
        fillRect(W * 74 / 100, H * 62 / 100, W * 86 / 100, H * 76 / 100, MaterialId::Metal);

        const int shelfY = H * 32 / 100;
        const int shelfThick = std::max(2, H / 30);
        const int fuseEnd = W * 87 / 100;
        fillRect(W * 66 / 100, shelfY, W * 92 / 100, shelfY + shelfThick - 1, MaterialId::Wood);
        fillRect(W * 68 / 100, shelfY - shelfThick, fuseEnd, shelfY - 1, MaterialId::Gunpowder);
        paintCircle(fuseEnd, shelfY - std::max(2, shelfThick / 2 + 1), std::max(2, H / 34), MaterialId::Fire, true);
        break;
    }

    case PresetId::Volcano: {
        fillRect(0, floorY, W - 1, H - 1, MaterialId::Bedrock);

        // Volcanic mountain cone scaled cleanly to any grid size >= 64x64
        const int cx = W / 2;
        const int baseHalf = W * 32 / 100;
        const int peakY = H * 28 / 100;
        const int ventTopHalf = std::max(3, W * 4 / 100);
        const int coneTopHalf = ventTopHalf + std::max(3, W * 4 / 100);
        const int ventBotHalf = std::max(4, W * 6 / 100);
        const int lavaTopY = peakY + std::max(4, H * 6 / 100);
        const int minWall = std::max(3, W / 24);

        for (int y = peakY; y < floorY; ++y) {
            const float t = static_cast<float>(y - peakY) / static_cast<float>(std::max(1, floorY - 1 - peakY));
            const int halfW =
                static_cast<int>(static_cast<float>(coneTopHalf) + t * static_cast<float>(baseHalf - coneTopHalf));
            int ventW =
                static_cast<int>(static_cast<float>(ventTopHalf) + t * static_cast<float>(ventBotHalf - ventTopHalf));
            if (t > 0.65f) {
                ventW = std::min(halfW - minWall,
                                 ventW + static_cast<int>((t - 0.65f) * static_cast<float>(baseHalf) * 0.65f));
            }
            for (int x = cx - halfW; x <= cx + halfW; ++x) {
                if (x < 1 || x >= W - 1) continue;
                if (std::abs(x - cx) <= ventW) {
                    if (y >= lavaTopY) setCell(x, y, MaterialId::Lava);
                } else {
                    setCell(x, y, (t > 0.65f) ? MaterialId::Obsidian : MaterialId::Stone);
                }
            }
        }

        // Snow cap on outer flanks
        const int snowThick = std::max(2, H / 28);
        for (int x = cx - baseHalf; x <= cx + baseHalf; ++x) {
            if (x < 1 || x >= W - 1 || std::abs(x - cx) <= ventTopHalf + 2) continue;
            for (int y = peakY; y < H * 60 / 100; ++y) {
                if (fromIdx(m_mat[index(x, y)]) == MaterialId::Stone) {
                    fillRect(x, y - snowThick, x, y - 1, MaterialId::Snow);
                    break;
                }
            }
        }

        // Flanking lakes cleanly outside mountain base
        const int lakeTopY = H * 64 / 100;
        const int leftLakeEnd = std::max(3, cx - baseHalf - 1);
        const int rightLakeStart = std::min(W - 4, cx + baseHalf + 1);
        fillRect(2, lakeTopY, leftLakeEnd, floorY - 1, MaterialId::Water);
        fillRect(rightLakeStart, lakeTopY, W - 3, floorY - 1, MaterialId::Water);

        // Tag central bedrock vent floor as a periodic geothermal magma feeder (m_life = 1000)
        const int feederHalf = std::max(2, ventBotHalf - 1);
        for (int x = cx - feederHalf; x <= cx + feederHalf; ++x) {
            if (!inBounds(x, floorY)) continue;
            const int bi = index(x, floorY);
            m_life[bi] = 1000.0f;
            m_tempK[bi] = 2100.0f;
        }
        paintCircle(cx, floorY - std::max(4, H / 12), std::max(2, std::min(W, H) / 28), MaterialId::Plasma, true);
        break;
    }

    case PresetId::Hydrodynamics: {
        fillRect(0, floorY, W - 1, H - 1, MaterialId::Bedrock);

        // Communicating U-tube vessels made of Glass
        const int wall = std::max(2, std::min(W, H) / 30);
        const int leftX = W * 18 / 100;
        const int rightX = W * 82 / 100;
        const int topY = H * 24 / 100;
        const int botY = std::min(floorY - 1, H * 86 / 100);
        const int armW = std::max(6, W * 16 / 100);
        const int chanH = std::max(6, H * 14 / 100);
        const int divBot = botY - wall - chanH;

        // Outer shell + inner divider
        fillRect(leftX, topY, leftX + wall - 1, botY, MaterialId::Glass);
        fillRect(rightX - wall + 1, topY, rightX, botY, MaterialId::Glass);
        fillRect(leftX, botY - wall + 1, rightX, botY, MaterialId::Glass);
        fillRect(leftX + wall + armW, topY, rightX - wall - armW, divBot, MaterialId::Glass);

        // Bottom bridge Water + taller Water column & Oil cap in left arm (zero overlap)
        fillRect(leftX + wall, divBot + 1, rightX - wall, botY - wall, MaterialId::Water);
        const int oilTopY = topY + (botY - topY) * 14 / 100;
        const int waterColTopY = topY + (botY - topY) * 32 / 100;
        fillRect(leftX + wall, waterColTopY, leftX + wall + armW - 1, divBot, MaterialId::Water);
        fillRect(leftX + wall, oilTopY, leftX + wall + armW - 1, waterColTopY - 1, MaterialId::Oil);

        // Salt & Water drops centered cleanly above each arm
        const int dropR = std::max(2, std::min(armW / 3, 8));
        const int dropY = std::max(dropR + 2, topY - dropR - 2);
        paintCircle(rightX - wall - armW / 2, dropY, dropR, MaterialId::Salt, true);
        paintCircle(leftX + wall + armW / 2, dropY, dropR, MaterialId::Water, true);
        break;
    }

    case PresetId::Explosives: {
        fillRect(0, floorY, W - 1, H - 1, MaterialId::Bedrock);

        // Obsidian cannon barrel pointing right
        const int barrelL = W * 14 / 100;
        const int barrelR = W * 50 / 100;
        const int breechThick = std::max(2, W * 4 / 100);
        const int wallThick = std::max(2, H * 4 / 100);
        const int boreTop = H * 56 / 100;
        const int boreBot = std::max(boreTop + 5, H * 70 / 100);
        const int boreMidY = (boreTop + boreBot) / 2;

        fillRect(barrelL, boreTop - wallThick, barrelR, boreTop - 1, MaterialId::Obsidian);
        fillRect(barrelL, boreBot + 1, barrelR, boreBot + wallThick, MaterialId::Obsidian);
        fillRect(barrelL, boreTop - wallThick, barrelL + breechThick - 1, boreBot + wallThick, MaterialId::Obsidian);

        // Gunpowder charge + Nitro booster directly behind compact muzzle Sand slug
        const int powderStart = barrelL + breechThick;
        const int slugThick = std::clamp(W * 4 / 100, 3, 6);
        const int nitroThick = std::clamp(W * 3 / 100, 2, 5);
        const int slugEnd = barrelR - 2;
        const int slugStart = slugEnd - slugThick + 1;
        const int nitroEnd = slugStart - 1;
        const int nitroStart = nitroEnd - nitroThick + 1;
        const int powderEnd = nitroStart - 1;
        const int nitroInset = (boreBot - boreTop >= 8) ? 1 : 0;

        fillRect(powderStart, boreTop, powderEnd, boreBot, MaterialId::Gunpowder);
        fillRect(nitroStart, boreTop + nitroInset, nitroEnd, boreBot - nitroInset, MaterialId::Nitro);
        fillRect(slugStart, boreTop, slugEnd, boreBot, MaterialId::Sand);

        // Target Wood wall & sealed Glass tank filled with Methane
        const int targetTop = std::min(boreTop - wallThick - 2, H * 46 / 100);
        const int woodL = W * 60 / 100;
        const int woodThick = std::clamp(W / 64, 2, 3);
        fillRect(woodL, targetTop, woodL + woodThick - 1, floorY - 1, MaterialId::Wood);

        const int tankL = woodL + woodThick + std::max(2, W * 3 / 100);
        const int tankR = std::min(W - 3, W * 84 / 100);
        const int glassWall = 2;
        fillRect(tankL, targetTop, tankR, floorY - 1, MaterialId::Glass);
        fillRect(tankL + glassWall, targetTop + glassWall, tankR - glassWall, floorY - 1 - glassWall,
                 MaterialId::Methane);

        // Battery electrical igniter feeding copper wire straight into Gunpowder
        const int batL = std::max(2, W * 5 / 100);
        const int batR = std::max(batL + 2, barrelL - std::max(2, W * 3 / 100));
        fillRect(batL, boreMidY - 2, batR, boreMidY + 2, MaterialId::Battery);
        fillRect(batR + 1, boreMidY, powderStart, std::min(boreBot, boreMidY + 1), MaterialId::Metal);
        break;
    }

    case PresetId::ElectricGrid: {
        fillRect(0, floorY, W - 1, H - 1, MaterialId::Bedrock);

        // High-current-density SaltWater electrolyte trough
        const int wall = std::max(2, std::min(W, H) / 32);
        const int troughL = W * 38 / 100;
        const int troughR = W * 62 / 100;
        const int troughTop = H * 54 / 100;
        const int troughBot = std::min(floorY - 2, H * 70 / 100);
        fillRect(troughL, troughTop, troughR, troughBot, MaterialId::Glass);
        fillRect(troughL + wall, troughTop, troughR - wall, troughTop + wall - 1, MaterialId::Air);
        fillRect(troughL + wall, troughTop + wall, troughR - wall, troughBot - wall, MaterialId::SaltWater);

        // 4V Battery -> copper rails -> SaltWater bridge -> Ice & Gunpowder load
        const int railTop = H * 28 / 100;
        const int railBot = railTop + std::max(2, H / 34);
        const int batL = W * 22 / 100;
        const int batR = W * 28 / 100;
        const int leftElecX = W * 46 / 100;
        const int rightElecX = W * 54 / 100;
        const int elecW = std::max(2, W / 55);
        const int elecDipY = troughTop + wall + std::max(2, (troughBot - wall - (troughTop + wall)) / 2);

        fillRect(batL, std::max(2, railTop - 2), batR, railBot + 2, MaterialId::Battery);
        fillRect(batR + 1, railTop, leftElecX + elecW - 1, railBot, MaterialId::Metal);
        fillRect(leftElecX, railTop, leftElecX + elecW - 1, elecDipY, MaterialId::Metal);

        fillRect(rightElecX, railTop, rightElecX + elecW - 1, elecDipY, MaterialId::Metal);
        fillRect(rightElecX, railTop, W * 76 / 100, railBot, MaterialId::Metal);

        // Place Ice cube & Gunpowder fuse strictly above the metal rail without cutting it
        const int iceH = std::max(4, H * 8 / 100);
        const int powH = std::max(2, H / 35);
        fillRect(W * 66 / 100, std::max(2, railTop - iceH), W * 74 / 100, railTop - 1, MaterialId::Ice);
        fillRect(W * 58 / 100, std::max(2, railTop - powH), W * 62 / 100, railTop - 1, MaterialId::Gunpowder);
        break;
    }

    case PresetId::ZeroGOrb: {
        m_gravityMode = GravityMode::RadialCenter;
        // Concentric orbiting rings scaled cleanly to any grid size >= 64x64
        const int cx = W / 2;
        const int cy = H / 2;
        const float maxR = static_cast<float>(std::min(W, H)) * 0.44f;
        const int lavaR = std::max(3, static_cast<int>(maxR * 0.22f));
        const int plasmaR = std::max(1, static_cast<int>(maxR * 0.09f));
        paintCircle(cx, cy, lavaR, MaterialId::Lava, true);
        paintCircle(cx, cy, plasmaR, MaterialId::Plasma, true);

        const int loopR = static_cast<int>(std::ceil(maxR)) + 1;
        for (int dy = -loopR; dy <= loopR; ++dy) {
            for (int dx = -loopR; dx <= loopR; ++dx) {
                const float r = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                const int x = cx + dx;
                const int y = cy + dy;
                if (!inBounds(x, y)) continue;
                if (r > maxR * 0.36f && r < maxR * 0.54f) {
                    setCell(x, y, MaterialId::Water, false);
                } else if (r >= maxR * 0.58f && r < maxR * 0.74f) {
                    setCell(x, y, MaterialId::Oil, false);
                } else if (r >= maxR * 0.78f && r < maxR * 0.94f) {
                    setCell(x, y, MaterialId::Sand, false);
                } else {
                    continue;
                }
                // Tangential Keplerian orbital velocity matching radial g = 0.20
                const float tx = -static_cast<float>(dy) / (r + 1e-3f);
                const float ty = static_cast<float>(dx) / (r + 1e-3f);
                const float orbitalV = std::min(3.5f, std::sqrt(0.20f * r) * 0.95f);
                setCellVelocity(x, y, tx * orbitalV, ty * orbitalV);
            }
        }
        break;
    }
    }
}

} // namespace sim
