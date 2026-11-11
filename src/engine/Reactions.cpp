#include "sim/SimWorld.hpp"

#include <algorithm>
#include <cmath>

namespace sim {

void SimWorld::stepReactions() {
    const int W = m_width;
    const int H = m_height;
    const bool leftToRight = (m_stats.tick & 1u) == 0u;

    // m_solidMask is unused after stepFluidField(); reuse it as a per-tick reaction guard
    // so neighbor mutations never double-update in the same pass.
    std::fill(m_solidMask.begin(), m_solidMask.end(), 0);

    constexpr int dx4[4] = {-1, 1, 0, 0};
    constexpr int dy4[4] = {0, 0, -1, 1};

    for (int y = 0; y < H; ++y) {
        const int xStart = leftToRight ? 0 : (W - 1);
        const int xEnd = leftToRight ? W : -1;
        const int xStep = leftToRight ? 1 : -1;

        for (int x = xStart; x != xEnd; x += xStep) {
            const int i = y * W + x;
            if (m_solidMask[i]) continue;

            const MaterialId mat = fromIdx(m_mat[i]);
            if (mat == MaterialId::Air) continue;

            const uint32_t rnd = hashCoord(x, y, m_stats.tick);

            // --- 0. Volcanic Geothermal Vent Feeder (tagged Bedrock with m_life >= 500) ---
            if (mat == MaterialId::Bedrock) {
                if (m_life[i] < 500.0f || y <= 2) continue;

                const uint64_t cycle = m_stats.tick % 96ull;
                const bool erupting = (cycle >= 6ull && cycle < 26ull);
                const bool simmering = (cycle >= 74ull);

                // Keep magma chamber base superheated and unclogged without bypassing solid barriers
                for (int dy = 1; dy <= 3; ++dy) {
                    const int ny = y - dy;
                    if (!inBounds(x, ny)) break;
                    const int ni = ny * W + x;
                    const MaterialId nm = fromIdx(m_mat[ni]);
                    if (nm == MaterialId::Bedrock) break;
                    if (nm == MaterialId::Air || nm == MaterialId::Smoke || nm == MaterialId::Steam ||
                        nm == MaterialId::Fire ||
                        (erupting && (nm == MaterialId::Stone || nm == MaterialId::Obsidian))) {
                        if (dy == 1 || erupting) {
                            convertCell(ni, MaterialId::Lava, false);
                            m_tempK[ni] = erupting ? 2050.0f : 1680.0f;
                            m_vy[ni] = erupting ? -3.4f : -0.4f;
                            m_solidMask[ni] = 1;
                        }
                    } else if (nm == MaterialId::Lava) {
                        m_tempK[ni] = std::max(m_tempK[ni], erupting ? 2050.0f : 1650.0f);
                    } else if (getMaterial(nm).isSolidObstacle()) {
                        break;
                    }
                }

                const int baseUp = (y - 1) * W + x;
                const MaterialId baseMat = fromIdx(m_mat[baseUp]);
                if (erupting) {
                    if (!getMaterial(baseMat).isSolidObstacle()) {
                        m_blastDiv[baseUp] += 1.05f;
                    }
                    const int minY = std::max(1, H * 16 / 100);
                    for (int ny = y - 1; ny >= minY; --ny) {
                        const int ni = ny * W + x;
                        const MaterialId nm = fromIdx(m_mat[ni]);
                        const int dy = y - ny;
                        if (nm == MaterialId::Bedrock ||
                            (dy > 4 && (nm == MaterialId::Stone || nm == MaterialId::Obsidian)) ||
                            (getMaterial(nm).isSolidObstacle() && nm != MaterialId::Stone &&
                             nm != MaterialId::Obsidian)) {
                            break;
                        }
                        if (nm == MaterialId::Lava) {
                            const bool upperVent = (ny < H * 56 / 100);
                            const float pushY = upperVent ? 0.85f : 0.55f;
                            const float spreadX = upperVent ? 0.34f : 0.14f;
                            m_vy[ni] = std::clamp(m_vy[ni] - pushY, -8.5f, 8.5f);
                            m_vx[ni] = std::clamp(
                                m_vx[ni] +
                                    static_cast<float>(static_cast<int>((rnd + static_cast<uint32_t>(dy)) % 3u) - 1) *
                                        spreadX,
                                -4.5f, 4.5f);
                            m_airVy[ni] = std::clamp(m_airVy[ni] - 0.65f, -12.0f, 12.0f);
                            m_blastDiv[ni] += upperVent ? 0.32f : 0.18f;
                            m_tempK[ni] = std::max(m_tempK[ni], 1820.0f);

                            // Emit glowing sparks/fire at top of rising magma column
                            if (ny > 1 && fromIdx(m_mat[ni - W]) == MaterialId::Air && !m_solidMask[ni - W] &&
                                ((rnd + static_cast<uint32_t>(dy)) % 7u) == 0u) {
                                const int spIdx = ni - W;
                                convertCell(spIdx, ((rnd & 1u) ? MaterialId::Spark : MaterialId::Fire), false);
                                m_vx[spIdx] = static_cast<float>(static_cast<int>((rnd >> 3) % 5u) - 2) * 0.65f;
                                m_vy[spIdx] = -4.6f;
                                m_tempK[spIdx] = 2100.0f;
                                m_solidMask[spIdx] = 1;
                            }
                        } else if (nm == MaterialId::Air || nm == MaterialId::Fire || nm == MaterialId::Smoke ||
                                   nm == MaterialId::Steam) {
                            m_airVy[ni] = std::clamp(m_airVy[ni] - 0.55f, -12.0f, 12.0f);
                        }
                    }
                } else if (simmering && ((rnd % 6u) == 0u) && !getMaterial(baseMat).isSolidObstacle()) {
                    m_blastDiv[baseUp] += 0.14f;
                }
                continue;
            }

            const auto &props = getMaterial(mat);
            const float T = m_tempK[i];

            // --- 1. Energy & Gas Lifecycle ---
            if (mat == MaterialId::Fire) {
                m_life[i] -= 1.0f;
                m_tempK[i] = std::max(T, 1080.0f);
                m_airVy[i] = std::clamp(m_airVy[i] - 0.07f, -12.0f, 12.0f);

                bool extinguished = false;
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[k];
                    const int ny = y + dy4[k];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    const MaterialId nm = fromIdx(m_mat[ni]);
                    if (nm == MaterialId::Water || nm == MaterialId::SaltWater) {
                        m_tempK[ni] += 55.0f;
                        convertCell(i, (rnd & 1u) ? MaterialId::Steam : MaterialId::Smoke, true);
                        extinguished = true;
                        break;
                    }
                    if (nm == MaterialId::Ice || nm == MaterialId::Snow) {
                        m_tempK[ni] += 50.0f;
                        convertCell(i, MaterialId::Steam, true);
                        extinguished = true;
                        break;
                    }
                    // Heat nearby air/gases to drive convective thermal plumes
                    if (nm == MaterialId::Air || nm == MaterialId::Smoke || nm == MaterialId::Steam) {
                        const float heatBoost = (ny < y) ? 42.0f : 24.0f;
                        m_tempK[ni] = std::min(1550.0f, m_tempK[ni] + heatBoost);
                        m_airVy[ni] = std::clamp(m_airVy[ni] - 0.045f, -12.0f, 12.0f);
                    }
                }
                if (extinguished) continue;

                // Occasional rising embers (Sparks) and trailing Smoke wisps
                if (y > 0) {
                    const int upIdx = (y - 1) * W + x;
                    if (fromIdx(m_mat[upIdx]) == MaterialId::Air && !m_solidMask[upIdx]) {
                        if (m_life[i] > 8.0f && (rnd % 110u) == 0u) {
                            convertCell(upIdx, MaterialId::Spark, false);
                            m_vx[upIdx] = static_cast<float>(static_cast<int>((rnd >> 4) % 3u) - 1) * 0.6f;
                            m_vy[upIdx] = -1.35f - static_cast<float>(rnd % 3u) * 0.25f;
                            m_life[upIdx] = 9.0f + static_cast<float>(rnd % 7u);
                            m_solidMask[upIdx] = 1;
                        } else if (m_life[i] < 14.0f && (rnd % 32u) == 0u) {
                            convertCell(upIdx, MaterialId::Smoke, true);
                            m_tempK[upIdx] = std::max(m_tempK[upIdx], 480.0f);
                            m_vy[upIdx] = -0.65f;
                            m_solidMask[upIdx] = 1;
                        }
                    }
                }

                if (m_life[i] <= 0.0f) {
                    const bool toSmoke = (rnd % 2u) == 0u;
                    convertCell(i, toSmoke ? MaterialId::Smoke : MaterialId::Air, true);
                    if (toSmoke) m_tempK[i] = std::max(m_tempK[i], 460.0f);
                }
                continue;
            }

            if (mat == MaterialId::Spark) {
                m_life[i] -= 1.0f;
                m_charge[i] = std::max(m_charge[i], 0.9f);
                bool quenched = false;
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[k];
                    const int ny = y + dy4[k];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    const MaterialId nm = fromIdx(m_mat[ni]);
                    if (nm == MaterialId::Water || nm == MaterialId::SaltWater) {
                        m_tempK[ni] += 40.0f;
                        convertCell(i, MaterialId::Steam, true);
                        quenched = true;
                        break;
                    }
                    if (nm == MaterialId::Air) {
                        m_tempK[ni] = std::min(1400.0f, m_tempK[ni] + 18.0f);
                    }
                }
                if (quenched) continue;
                if (m_life[i] <= 0.0f) {
                    convertCell(i, MaterialId::Air, true);
                }
                continue;
            }

            if (mat == MaterialId::Plasma) {
                m_life[i] -= 1.0f;
                m_tempK[i] = std::max(T, 3400.0f);
                m_charge[i] = 1.4f;
                m_blastDiv[i] += 0.08f;
                if (m_life[i] <= 0.0f) {
                    convertCell(i, ((rnd & 1u) ? MaterialId::Fire : MaterialId::Spark), true);
                }
                continue;
            }

            if (mat == MaterialId::Smoke) {
                m_life[i] -= 1.0f;
                if (m_life[i] <= 0.0f) {
                    convertCell(i, MaterialId::Air, true);
                }
                continue;
            }

            if (mat == MaterialId::Steam) {
                m_life[i] -= 0.4f;
                if (T < props.condenseTempK && (rnd % 20u) == 0u) {
                    convertCell(i, MaterialId::Water, true);
                    m_tempK[i] = std::min(365.0f, T + 18.0f); // Latent heat of condensation
                } else if (m_life[i] <= 0.0f && T < 345.0f) {
                    convertCell(i, MaterialId::Water, true);
                }
                continue;
            }

            // --- 2. Phase Transitions (Melting / Boiling / Supercooled Freezing) ---
            if (mat == MaterialId::Ice || mat == MaterialId::Snow) {
                if (T > props.meltTempK) {
                    convertCell(i, MaterialId::Water, true);
                    m_tempK[i] = std::max(273.25f, T - 12.0f); // Latent heat absorbed
                    continue;
                }
            } else if (mat == MaterialId::Water) {
                if (T >= props.boilTempK) {
                    convertCell(i, MaterialId::Steam, true);
                    m_tempK[i] = std::max(373.15f, T - 18.0f);
                    m_vy[i] = -0.9f;
                    m_blastDiv[i] += 0.25f;
                    continue;
                }
                if (T < props.freezeTempK) {
                    bool hasIceNucleus = false;
                    for (int k = 0; k < 4; ++k) {
                        const int nx = x + dx4[k];
                        const int ny = y + dy4[k];
                        if (!inBounds(nx, ny)) continue;
                        const int ni = ny * W + nx;
                        if (m_solidMask[ni]) continue; // Only nucleate from pre-existing crystals
                        const MaterialId nm = fromIdx(m_mat[ni]);
                        if (nm == MaterialId::Ice || nm == MaterialId::Snow) {
                            hasIceNucleus = true;
                            break;
                        }
                    }
                    if (hasIceNucleus && T < props.freezeTempK - 0.3f && (rnd % 3u) == 0u) {
                        convertCell(i, MaterialId::Ice, true);
                        m_tempK[i] = std::min(273.15f, T + 10.0f);
                        m_solidMask[i] = 1;
                        continue;
                    }
                    if (T < props.freezeTempK - 6.0f) {
                        const uint32_t mod = (T < 258.0f) ? 4u : 10u;
                        if ((rnd % mod) == 0u) {
                            convertCell(i, MaterialId::Ice, true);
                            m_tempK[i] = std::min(273.15f, T + 12.0f);
                            m_solidMask[i] = 1;
                            continue;
                        }
                    }
                }
            } else if (mat == MaterialId::SaltWater) {
                if (T >= props.boilTempK) {
                    if ((rnd % 4u) == 0u) {
                        convertCell(i, MaterialId::Salt, true);
                    } else {
                        convertCell(i, MaterialId::Steam, true);
                        m_vy[i] = -0.95f;
                        m_blastDiv[i] += 0.22f;
                    }
                    continue;
                }
                if (T < props.freezeTempK - 2.0f && (rnd % 8u) == 0u) {
                    // Brine rejection: occasionally precipitate Salt crystals as SaltWater freezes
                    convertCell(i, ((rnd % 5u) == 0u) ? MaterialId::Salt : MaterialId::Ice, true);
                    m_solidMask[i] = 1;
                    continue;
                }
            } else if (mat == MaterialId::Sand) {
                if (T >= props.meltTempK) {
                    convertCell(i, MaterialId::MoltenGlass, true);
                    continue;
                }
            } else if (mat == MaterialId::MoltenGlass) {
                if (T < props.freezeTempK) {
                    convertCell(i, MaterialId::Glass, true);
                    continue;
                }
            } else if (mat == MaterialId::Stone) {
                if (T >= props.meltTempK) {
                    convertCell(i, MaterialId::Lava, true);
                    continue;
                }
            } else if (mat == MaterialId::Metal) {
                if (T >= props.meltTempK) {
                    convertCell(i, MaterialId::MoltenMetal, true);
                    continue;
                }
            } else if (mat == MaterialId::MoltenMetal) {
                if (T < props.freezeTempK) {
                    convertCell(i, MaterialId::Metal, true);
                    continue;
                }
            } else if (mat == MaterialId::Lava) {
                // Contact with Water/Ice rapidly quenches Lava -> Obsidian + Steam plume!
                bool quenched = false;
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[k];
                    const int ny = y + dy4[k];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    if (m_solidMask[ni]) continue;
                    const MaterialId nm = fromIdx(m_mat[ni]);
                    if (nm == MaterialId::Water || nm == MaterialId::SaltWater || nm == MaterialId::Ice ||
                        nm == MaterialId::Snow) {
                        const bool saltPrecip = (nm == MaterialId::SaltWater) && ((rnd % 7u) == 0u);
                        convertCell(ni, saltPrecip ? MaterialId::Salt : MaterialId::Steam, false);
                        if (!saltPrecip) {
                            m_tempK[ni] = 435.0f;
                            m_vy[ni] = -1.35f;
                            m_vx[ni] = static_cast<float>(static_cast<int>((rnd >> 3) % 3u) - 1) * 0.45f;
                            m_blastDiv[ni] += 0.38f;
                        }
                        m_solidMask[ni] = 1;
                        m_tempK[i] -= 210.0f;
                        quenched = true;
                    }
                }
                if (quenched && m_tempK[i] < 1300.0f) {
                    convertCell(i, MaterialId::Obsidian, true);
                    m_solidMask[i] = 1;
                    continue;
                }
                if (m_tempK[i] < props.freezeTempK) {
                    convertCell(i, (rnd & 3u) == 0u ? MaterialId::Obsidian : MaterialId::Stone, true);
                    continue;
                }
            } else if (mat == MaterialId::Acid) {
                if (T >= props.boilTempK) {
                    convertCell(i, MaterialId::Smoke, true);
                    continue;
                }
            }

            // --- 3. Combustion & Explosive Reactions ---
            if (props.ignitionTempK > 0.0f) {
                bool ignite = (T >= props.ignitionTempK) || (std::abs(m_charge[i]) > 0.72f);

                if (mat == MaterialId::Nitro && std::abs(m_pressure[i]) > 2.2f) {
                    ignite = true;
                }

                if (!ignite) {
                    for (int k = 0; k < 4; ++k) {
                        const int nx = x + dx4[k];
                        const int ny = y + dy4[k];
                        if (!inBounds(nx, ny)) continue;
                        const int ni = ny * W + nx;
                        const MaterialId nm = fromIdx(m_mat[ni]);
                        if (nm == MaterialId::Fire || nm == MaterialId::Spark || nm == MaterialId::Plasma ||
                            nm == MaterialId::Lava || std::abs(m_charge[ni]) > 0.35f) {
                            ignite = true;
                            break;
                        }
                    }
                }

                if (ignite) {
                    if (mat == MaterialId::Nitro) {
                        detonate(x, y, 10, 6.2f, 1680.0f);
                        continue;
                    }
                    if (mat == MaterialId::Gunpowder) {
                        m_blastDiv[i] += 3.2f;
                        m_tempK[i] = 1750.0f;
                        convertCell(i, ((rnd % 5u) == 0u) ? MaterialId::Spark : MaterialId::Fire, true);
                        m_vy[i] = -1.2f;
                        m_vx[i] += static_cast<float>(static_cast<int>(rnd % 5u) - 2) * 0.45f;
                        continue;
                    }
                    if (mat == MaterialId::Methane) {
                        m_blastDiv[i] += 1.5f;
                        m_tempK[i] = 1650.0f;
                        convertCell(i, MaterialId::Fire, true);
                        continue;
                    }
                    if (mat == MaterialId::Oil) {
                        m_tempK[i] = std::max(T, 1320.0f);
                        convertCell(i, MaterialId::Fire, true);
                        m_life[i] = 42.0f;
                        continue;
                    }
                    if (mat == MaterialId::Wood || mat == MaterialId::Plant) {
                        const uint32_t burnMod = (mat == MaterialId::Plant) ? 6u : 18u;
                        m_tempK[i] = std::max(T, 880.0f);
                        if ((rnd % burnMod) == 0u) {
                            convertCell(i, MaterialId::Fire, true);
                            m_life[i] = 32.0f;
                        } else {
                            // Emit flame/smoke into an adjacent Air pixel
                            for (int k = 0; k < 4; ++k) {
                                const int nx = x + dx4[(k + rnd) & 3];
                                const int ny = y + dy4[(k + rnd) & 3];
                                if (inBounds(nx, ny)) {
                                    const int ni = ny * W + nx;
                                    if (fromIdx(m_mat[ni]) == MaterialId::Air && !m_solidMask[ni]) {
                                        const bool emitSmoke = (ny < y) && ((rnd % 8u) == 0u);
                                        setCell(nx, ny, emitSmoke ? MaterialId::Smoke : MaterialId::Fire);
                                        m_tempK[ni] = emitSmoke ? 520.0f : 1180.0f;
                                        m_solidMask[ni] = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        continue;
                    }
                }
            }

            // --- 4. Acid Corrosion & Effervescent Gas Bubbles ---
            if (mat == MaterialId::Acid) {
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[(k + rnd) & 3];
                    const int ny = y + dy4[(k + rnd) & 3];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    if (m_solidMask[ni]) continue;
                    const MaterialId nm = fromIdx(m_mat[ni]);

                    uint32_t resist = 0;
                    if (nm == MaterialId::Plant || nm == MaterialId::Wood || nm == MaterialId::Ice)
                        resist = 4u;
                    else if (nm == MaterialId::Metal || nm == MaterialId::Stone || nm == MaterialId::Salt)
                        resist = 10u;
                    else if (nm == MaterialId::Sand)
                        resist = 22u;
                    else if (nm == MaterialId::Obsidian)
                        resist = 85u;
                    else
                        continue;

                    if ((rnd % resist) == 0u) {
                        MaterialId bubbleGas = MaterialId::Smoke;
                        if (nm == MaterialId::Ice) {
                            bubbleGas = MaterialId::Steam;
                        } else if (nm == MaterialId::Metal || nm == MaterialId::Plant || nm == MaterialId::Wood) {
                            bubbleGas = MaterialId::Methane;
                        } else {
                            bubbleGas = ((rnd & 1u) == 0u) ? MaterialId::Methane : MaterialId::Smoke;
                        }

                        convertCell(ni, bubbleGas, true);
                        m_vy[ni] = -0.95f;
                        m_vx[ni] = static_cast<float>(static_cast<int>((rnd >> 2) % 3u) - 1) * 0.3f;
                        m_blastDiv[ni] += 0.12f;
                        m_tempK[ni] += 70.0f;
                        m_tempK[i] += 45.0f;
                        m_solidMask[ni] = 1;

                        m_life[i] -= 16.0f;
                        if (m_life[i] <= 0.0f) {
                            convertCell(i, MaterialId::Smoke, true);
                        }
                        break;
                    }
                }
                continue;
            }

            // --- 5. Salt Dissolution & Freezing-Point Depression ---
            if (mat == MaterialId::Salt) {
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[(k + rnd) & 3];
                    const int ny = y + dy4[(k + rnd) & 3];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    if (m_solidMask[ni]) continue;
                    const MaterialId nm = fromIdx(m_mat[ni]);
                    if (nm == MaterialId::Water && (rnd % 5u) == 0u) {
                        convertCell(ni, MaterialId::SaltWater, true);
                        m_solidMask[ni] = 1;
                        convertCell(i, MaterialId::Air, true);
                        break;
                    }
                    if ((nm == MaterialId::Ice || nm == MaterialId::Snow) && T > 255.0f && (rnd % 16u) == 0u) {
                        convertCell(ni, MaterialId::SaltWater, true);
                        m_solidMask[ni] = 1;
                        convertCell(i, MaterialId::Air, true);
                        break;
                    }
                }
                continue;
            }

            // --- 6. Plant Capillary Hydration & Organic Growth ---
            if (mat == MaterialId::Plant) {
                if (T > 390.0f) {
                    m_life[i] = std::max(0.0f, m_life[i] - 0.6f);
                }

                int plantNeighbors = 0;
                int airX = -1, airY = -1, airIdx = -1;
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[(k + rnd) & 3];
                    const int ny = y + dy4[(k + rnd) & 3];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    const MaterialId nm = fromIdx(m_mat[ni]);
                    if (nm == MaterialId::Water) {
                        if (m_life[i] < 6.0f) {
                            m_life[i] = 10.0f;
                            // Sparingly draw water so small pools sustain lush growth
                            if ((rnd % 8u) == 0u && !m_solidMask[ni]) {
                                convertCell(ni, MaterialId::Air, true);
                                m_solidMask[ni] = 1;
                            }
                        }
                    } else if (nm == MaterialId::Plant) {
                        ++plantNeighbors;
                        if (m_life[ni] - m_life[i] >= 1.5f && !m_solidMask[ni]) {
                            m_life[ni] -= 1.5f;
                            m_life[i] += 1.5f;
                        }
                    } else if (nm == MaterialId::Air && ny <= y && !m_solidMask[ni]) {
                        if (airIdx < 0 || ny < airY) {
                            airX = nx;
                            airY = ny;
                            airIdx = ni;
                        }
                    }
                }

                if (m_life[i] >= 4.0f && plantNeighbors <= 2 && airIdx >= 0 && (rnd % 16u) == 0u) {
                    setCell(airX, airY, MaterialId::Plant);
                    m_life[airIdx] = std::max(1.0f, m_life[i] - 3.5f);
                    m_life[i] = std::max(0.0f, m_life[i] - 3.5f);
                    m_solidMask[airIdx] = 1;
                }
                continue;
            }

            // --- 7. Dielectric Air Breakdown (Electrical Spark Arcing) ---
            if (props.electricalConductivity > 0.5f && std::abs(m_charge[i]) > 0.85f && (rnd % 6u) == 0u) {
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx4[(k + rnd) & 3];
                    const int ny = y + dy4[(k + rnd) & 3];
                    if (!inBounds(nx, ny)) continue;
                    const int ni = ny * W + nx;
                    if (fromIdx(m_mat[ni]) == MaterialId::Air && !m_solidMask[ni]) {
                        // Arc toward nearby lower-potential conductor or explosive gas
                        const int nx2 = nx + dx4[(k + rnd) & 3];
                        const int ny2 = ny + dy4[(k + rnd) & 3];
                        if (inBounds(nx2, ny2)) {
                            const int ni2 = ny2 * W + nx2;
                            const MaterialId nm2 = fromIdx(m_mat[ni2]);
                            if ((getMaterial(nm2).electricalConductivity > 0.2f &&
                                 std::abs(m_charge[i] - m_charge[ni2]) > 0.4f) ||
                                nm2 == MaterialId::Gunpowder || nm2 == MaterialId::Methane) {
                                setCell(nx, ny, MaterialId::Spark);
                                m_charge[ni] = m_charge[i] * 0.7f;
                                m_solidMask[ni] = 1;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace sim
