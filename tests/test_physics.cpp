#include "sim/Material.hpp"
#include "sim/SimWorld.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int gPassed = 0;
int gFailed = 0;

void check(bool cond, const std::string &expr, const char *file, int line) {
    if (!cond) {
        std::ostringstream oss;
        oss << file << ":" << line << " Assertion failed: " << expr;
        throw std::runtime_error(oss.str());
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

void runTest(const char *name, void (*fn)()) {
    try {
        fn();
        std::cout << "[PASS] " << name << "\n";
        ++gPassed;
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] " << name << " -> " << e.what() << "\n";
        ++gFailed;
    }
}

void testMaterialConstants() {
    const auto &mats = sim::getAllMaterials();
    for (size_t i = 0; i < mats.size(); ++i) {
        const auto &m = mats[i];
        CHECK(static_cast<size_t>(m.id) == i);
        CHECK(m.heatCapacity > 0.0f);
        CHECK(m.density > 0.0f);
        CHECK(m.thermalConductivity > 0.0f && m.thermalConductivity <= 1.0f);
        CHECK(!m.nameIt.empty());
    }
    CHECK(sim::getMaterial(sim::MaterialId::Sand).density > sim::getMaterial(sim::MaterialId::Water).density);
    CHECK(sim::getMaterial(sim::MaterialId::Water).density > sim::getMaterial(sim::MaterialId::Oil).density);
    CHECK(sim::getMaterial(sim::MaterialId::Lava).density > sim::getMaterial(sim::MaterialId::Water).density);
}

void testGravityAndNoTunneling() {
    sim::SimWorld world(64, 64, 1);
    world.setBoundaryMode(sim::BoundaryMode::Closed);
    world.setGravityMode(sim::GravityMode::Earth);

    // Build a 1-pixel thick Stone shelf at y = 40
    for (int x = 1; x < 63; ++x) {
        world.setCell(x, 40, sim::MaterialId::Stone);
    }

    // Launch fast Sand at y = 10 with high downward velocity vy = 9.0 px/tick
    world.setCell(32, 10, sim::MaterialId::Sand);
    world.setCellVelocity(32, 10, 0.0f, 9.0f);

    for (int t = 0; t < 30; ++t) {
        world.step();
    }

    // Verify sand did NOT tunnel past y = 40
    bool foundAbove = false;
    bool foundBelow = false;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (world.inspect(x, y).material == sim::MaterialId::Sand) {
                if (y < 40) foundAbove = true;
                if (y > 40) foundBelow = true;
            }
        }
    }
    CHECK(foundAbove);
    CHECK(!foundBelow);
}

void testDensityDisplacement() {
    sim::SimWorld world(48, 48, 1);
    // Build a glass cup
    for (int x = 16; x <= 32; ++x)
        world.setCell(x, 42, sim::MaterialId::Glass);
    for (int y = 20; y <= 42; ++y) {
        world.setCell(16, y, sim::MaterialId::Glass);
        world.setCell(32, y, sim::MaterialId::Glass);
    }

    // Put Oil on the bottom and Water above it, plus Sand on top
    for (int y = 35; y <= 40; ++y)
        for (int x = 18; x <= 30; ++x)
            world.setCell(x, y, sim::MaterialId::Oil);

    for (int y = 27; y <= 34; ++y)
        for (int x = 18; x <= 30; ++x)
            world.setCell(x, y, sim::MaterialId::Water);

    for (int x = 22; x <= 26; ++x)
        world.setCell(x, 22, sim::MaterialId::Sand);

    for (int t = 0; t < 180; ++t)
        world.step();

    double avgYSand = 0.0, avgYWater = 0.0, avgYOil = 0.0;
    int cSand = 0, cWater = 0, cOil = 0;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const auto m = world.inspect(x, y).material;
            if (m == sim::MaterialId::Sand) {
                avgYSand += y;
                ++cSand;
            } else if (m == sim::MaterialId::Water) {
                avgYWater += y;
                ++cWater;
            } else if (m == sim::MaterialId::Oil) {
                avgYOil += y;
                ++cOil;
            }
        }
    }
    CHECK(cSand == 5);
    CHECK(cWater > 50);
    CHECK(cOil > 50);
    avgYSand /= cSand;
    avgYWater /= cWater;
    avgYOil /= cOil;

    // Sand sinks deepest (largest Y), Water sits middle, Oil floats highest (smallest Y)
    CHECK(avgYSand > avgYWater);
    CHECK(avgYWater > avgYOil);
}

void testCommunicatingVessels() {
    sim::SimWorld world(64, 48, 1);
    // Build U-tube out of Glass: left arm x=[10..22], right arm x=[42..54], bottom channel y=[34..42]
    for (int x = 10; x <= 54; ++x)
        world.setCell(x, 42, sim::MaterialId::Glass);
    for (int y = 10; y <= 42; ++y) {
        world.setCell(10, y, sim::MaterialId::Glass);
        world.setCell(54, y, sim::MaterialId::Glass);
    }
    for (int x = 22; x <= 42; ++x) {
        for (int y = 10; y <= 33; ++y) {
            world.setCell(x, y, sim::MaterialId::Glass);
        }
    }
    // Pour water strictly into the left arm
    for (int y = 14; y <= 41; ++y) {
        for (int x = 11; x <= 21; ++x) {
            world.setCell(x, y, sim::MaterialId::Water);
        }
    }

    for (int t = 0; t < 260; ++t)
        world.step();

    int leftWater = 0, rightWater = 0;
    for (int y = 10; y <= 41; ++y) {
        for (int x = 11; x <= 21; ++x) {
            if (world.inspect(x, y).material == sim::MaterialId::Water) ++leftWater;
        }
        for (int x = 43; x <= 53; ++x) {
            if (world.inspect(x, y).material == sim::MaterialId::Water) ++rightWater;
        }
    }
    CHECK(rightWater > 50);
    CHECK(std::abs(leftWater - rightWater) <= 25);
}

void testThermalConservationAndConductivity() {
    // Closed box with Metal bridge (no reactions below 700K, no radiative cooling below 750K)
    sim::SimWorld world(48, 32, 1);
    world.setBoundaryMode(sim::BoundaryMode::Closed);
    world.setGravityMode(sim::GravityMode::ZeroG);

    for (int y = 12; y <= 18; ++y) {
        for (int x = 8; x <= 39; ++x) {
            world.setCell(x, y, sim::MaterialId::Metal);
            world.setCellTemp(x, y, (x < 24) ? 550.0f : 300.0f);
        }
    }

    const double initialEnergy = world.computeTotalThermalEnergy();
    for (int t = 0; t < 100; ++t) {
        world.step();
    }
    const double finalEnergy = world.computeTotalThermalEnergy();

    // Check energy conservation (< 0.1% drift)
    const double relErr = std::abs(finalEnergy - initialEnergy) / initialEnergy;
    CHECK(relErr < 1e-3);

    // Check heat diffused from hot left into cooler right end of metal rod
    CHECK(world.inspect(30, 15).tempK > 330.0f);
    CHECK(world.inspect(10, 15).tempK < 540.0f);

    // Compare Metal vs Wood thermal conductivity over 60 ticks
    sim::SimWorld woodWorld(48, 32, 1);
    woodWorld.setGravityMode(sim::GravityMode::ZeroG);
    for (int y = 12; y <= 18; ++y) {
        for (int x = 8; x <= 39; ++x) {
            woodWorld.setCell(x, y, sim::MaterialId::Wood);
            woodWorld.setCellTemp(x, y, (x < 24) ? 500.0f : 300.0f);
        }
    }
    sim::SimWorld metalWorld(48, 32, 1);
    metalWorld.setGravityMode(sim::GravityMode::ZeroG);
    for (int y = 12; y <= 18; ++y) {
        for (int x = 8; x <= 39; ++x) {
            metalWorld.setCell(x, y, sim::MaterialId::Metal);
            metalWorld.setCellTemp(x, y, (x < 24) ? 500.0f : 300.0f);
        }
    }
    for (int t = 0; t < 60; ++t) {
        woodWorld.step();
        metalWorld.step();
    }
    CHECK(metalWorld.inspect(28, 15).tempK > woodWorld.inspect(28, 15).tempK);
}

void testPhaseTransitions() {
    sim::SimWorld world(48, 48, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);

    // 1. Ice -> Water -> Steam
    world.setCell(12, 12, sim::MaterialId::Ice);
    world.setCellTemp(12, 12, 310.0f);
    world.step();
    CHECK(world.countMaterial(sim::MaterialId::Water) >= 1);

    // Boiling water
    world.clear();
    world.setCell(24, 24, sim::MaterialId::Water);
    world.setCellTemp(24, 24, 420.0f);
    world.step();
    CHECK(world.countMaterial(sim::MaterialId::Steam) >= 1);

    // Condensation
    world.clear();
    world.setCell(24, 24, sim::MaterialId::Steam);
    world.setCellTemp(24, 24, 300.0f);
    for (int t = 0; t < 120; ++t)
        world.step();
    CHECK(world.countMaterial(sim::MaterialId::Water) >= 1);

    // Salt dissolution in Water
    world.clear();
    world.setCell(20, 20, sim::MaterialId::Water);
    world.setCell(21, 20, sim::MaterialId::Salt);
    for (int t = 0; t < 40; ++t)
        world.step();
    CHECK(world.countMaterial(sim::MaterialId::SaltWater) >= 1);

    // Sand -> MoltenGlass -> Glass
    world.clear();
    world.setCell(20, 20, sim::MaterialId::Sand);
    world.setCellTemp(20, 20, 2200.0f);
    world.step();
    CHECK(world.countMaterial(sim::MaterialId::MoltenGlass) == 1);
    // Cool it below freeze point
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            if (world.inspect(x, y).material == sim::MaterialId::MoltenGlass) world.setCellTemp(x, y, 1000.0f);
    world.step();
    CHECK(world.countMaterial(sim::MaterialId::Glass) == 1);
}

void testBlastPressureAndMomentum() {
    sim::SimWorld world(64, 64, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);
    world.setCell(20, 32, sim::MaterialId::Sand);
    world.setCell(44, 32, sim::MaterialId::Sand);

    world.detonate(32, 32, 8, 6.0f, 2200.0f);
    for (int t = 0; t < 8; ++t)
        world.step();

    // Explosion creates positive pressure & pushes left sand further left and right sand further right
    CHECK(world.stats().maxPressure > 0.1f);
    bool leftPushed = false, rightPushed = false;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (world.inspect(x, y).material == sim::MaterialId::Sand) {
                if (x < 20) leftPushed = true;
                if (x > 44) rightPushed = true;
            }
        }
    }
    CHECK(leftPushed || rightPushed || world.inspect(20, 32).airVx < -0.05f);
    CHECK(world.inspect(40, 32).airVx > 0.05f);
    CHECK(world.inspect(24, 32).airVx < -0.05f);
}

void testElectricityAndJouleHeating() {
    sim::SimWorld world(48, 32, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);

    // Copper wire from x=10..30
    for (int x = 10; x <= 30; ++x) {
        world.setCell(x, 16, sim::MaterialId::Metal);
    }
    // Wood insulator on bottom row
    for (int x = 10; x <= 30; ++x) {
        world.setCell(x, 24, sim::MaterialId::Wood);
    }

    // Apply strong voltage pulse on left ends
    world.setCellCharge(10, 16, 4.0f);
    world.setCellCharge(10, 24, 4.0f);
    for (int t = 0; t < 35; ++t) {
        if (t < 15) {
            world.setCellCharge(10, 16, 4.0f);
            world.setCellCharge(10, 24, 4.0f);
        }
        world.step();
    }

    // Metal propagated charge & heated up; Wood did not propagate to x=20
    CHECK(world.inspect(20, 16).charge > 0.05f);
    CHECK(world.inspect(12, 16).tempK > sim::kAmbientTempK + 0.1f);
    CHECK(world.inspect(20, 24).charge < 0.001f);

    // Battery driving a long 55-cell wire to ignite Gunpowder at far end
    sim::SimWorld circuit(80, 32, 1);
    circuit.setGravityMode(sim::GravityMode::ZeroG);
    circuit.setCell(5, 16, sim::MaterialId::Battery);
    for (int x = 6; x <= 60; ++x)
        circuit.setCell(x, 16, sim::MaterialId::Metal);
    circuit.setCell(61, 16, sim::MaterialId::Gunpowder);
    for (int t = 0; t < 80; ++t)
        circuit.step();
    CHECK(circuit.inspect(55, 16).charge > 0.50f);
    CHECK(circuit.countMaterial(sim::MaterialId::Gunpowder) == 0);
}

void testChemistryCombustionAndAcid() {
    // Gunpowder ignition by Fire
    sim::SimWorld world(48, 48, 1);
    for (int x = 15; x <= 30; ++x)
        world.setCell(x, 30, sim::MaterialId::Gunpowder);
    for (int x = 14; x <= 31; ++x)
        world.setCell(x, 31, sim::MaterialId::Stone);
    world.setCell(15, 29, sim::MaterialId::Fire);

    for (int t = 0; t < 40; ++t)
        world.step();
    CHECK(world.countMaterial(sim::MaterialId::Gunpowder) < 16);

    // Acid corroding Stone
    sim::SimWorld acidWorld(48, 48, 1);
    for (int x = 18; x <= 26; ++x)
        acidWorld.setCell(x, 30, sim::MaterialId::Stone);
    for (int x = 19; x <= 25; ++x)
        acidWorld.setCell(x, 29, sim::MaterialId::Acid);
    const uint32_t initialStone = acidWorld.countMaterial(sim::MaterialId::Stone);
    for (int t = 0; t < 80; ++t)
        acidWorld.step();
    CHECK(acidWorld.countMaterial(sim::MaterialId::Stone) < initialStone);
}

void testZeroGAndRadialGravity() {
    sim::SimWorld zg(48, 48, 1);
    zg.setGravityMode(sim::GravityMode::ZeroG);
    zg.setCell(24, 24, sim::MaterialId::Sand);
    for (int t = 0; t < 40; ++t)
        zg.step();
    CHECK(zg.inspect(24, 24).material == sim::MaterialId::Sand);

    sim::SimWorld rg(48, 48, 1);
    rg.setGravityMode(sim::GravityMode::RadialCenter);
    rg.setCell(24, 24, sim::MaterialId::Stone);
    rg.setCell(10, 24, sim::MaterialId::Sand);
    for (int t = 0; t < 6; ++t)
        rg.step();
    CHECK(rg.inspect(10, 24).material == sim::MaterialId::Air);
    bool movedTowardCenter = false;
    for (int x = 11; x <= 23; ++x) {
        if (rg.inspect(x, 24).material == sim::MaterialId::Sand) movedTowardCenter = true;
    }
    CHECK(movedTowardCenter);
    for (int t = 0; t < 30; ++t)
        rg.step();
    CHECK(rg.inspect(23, 24).material == sim::MaterialId::Sand);
}

void testAllPresetsAndViewsFinite() {
    const sim::PresetId presets[] = {sim::PresetId::Laboratory,    sim::PresetId::Volcano,
                                     sim::PresetId::Hydrodynamics, sim::PresetId::Explosives,
                                     sim::PresetId::ElectricGrid,  sim::PresetId::ZeroGOrb};
    const sim::ViewMode views[] = {sim::ViewMode::Physical, sim::ViewMode::Thermal, sim::ViewMode::Pressure,
                                   sim::ViewMode::Velocity, sim::ViewMode::EnergyLight};

    for (auto p : presets) {
        sim::SimWorld world(160, 90, 2);
        world.loadPreset(p);
        for (int t = 0; t < 90; ++t) {
            world.step();
        }
        const auto &st = world.stats();
        CHECK(std::isfinite(st.totalThermalEnergy));
        CHECK(std::isfinite(st.totalKineticEnergy));
        CHECK(std::isfinite(st.minTempK) && st.minTempK >= 1.0f);
        CHECK(std::isfinite(st.maxTempK) && st.maxTempK <= 8000.0f);
        CHECK(std::isfinite(st.maxPressure));

        for (int y = 0; y < world.height(); y += 9) {
            for (int x = 0; x < world.width(); x += 10) {
                const auto px = world.inspect(x, y);
                CHECK(std::isfinite(px.tempK));
                CHECK(std::isfinite(px.pressure));
                CHECK(std::isfinite(px.vx) && std::isfinite(px.vy));
                CHECK(std::isfinite(px.airVx) && std::isfinite(px.airVy));
                CHECK(std::isfinite(px.charge));
                CHECK(std::isfinite(px.lightR) && std::isfinite(px.lightG) && std::isfinite(px.lightB));
            }
        }

        for (auto v : views) {
            world.render(v);
            const uint32_t *fb = world.framebuffer();
            bool hasNonBlack = false;
            for (int i = 0; i < world.pixelCount(); ++i) {
                if ((fb[i] & 0x00FFFFFFu) != 0u) {
                    hasNonBlack = true;
                    break;
                }
            }
            CHECK(hasNonBlack);
        }
    }
}

void testIsotropicLightingAndOcclusion() {
    // Part A: Euclidean isotropy & 4-quadrant symmetry around a stationary emissive source
    sim::SimWorld world(65, 65, 1);
    world.setBoundaryMode(sim::BoundaryMode::Closed);
    world.setGravityMode(sim::GravityMode::ZeroG);

    for (int t = 0; t < 12; ++t) {
        world.setCell(32, 32, sim::MaterialId::Lava);
        world.setCellTemp(32, 32, 1800.0f);
        world.step();
    }

    const float axPosX = world.inspect(36, 32).lightR; // r = 4.0, L1 = 4
    const float axNegX = world.inspect(28, 32).lightR;
    const float axPosY = world.inspect(32, 36).lightR;
    const float axNegY = world.inspect(32, 28).lightR;

    const float diagPP = world.inspect(35, 35).lightR; // r = sqrt(18) = 4.243, L1 = 6
    const float diagNP = world.inspect(29, 35).lightR;
    const float diagPN = world.inspect(35, 29).lightR;
    const float diagNN = world.inspect(29, 29).lightR;

    const float axDist5 = world.inspect(37, 32).lightR; // r = 5.0, L1 = 5

    // Circular 8-way halo: (3,3) at Euclidean r=4.24 must be brighter than (5,0) at r=5.0
    // (a 4-neighbor Manhattan diamond scheme reverses this because L1(3,3)=6 > L1(5,0)=5)
    CHECK(axPosX > 0.35f);
    CHECK(diagPP > axDist5);
    CHECK(diagPP > axPosX * 0.75f && diagPP < axPosX * 1.15f);

    // Axial and diagonal quadrant symmetry
    CHECK(std::abs(axPosX - axNegX) < 0.02f);
    CHECK(std::abs(axPosY - axNegY) < 0.02f);
    CHECK(std::abs(axPosX - axPosY) < 0.02f);
    CHECK(std::abs(diagPP - diagNP) < 0.02f);
    CHECK(std::abs(diagPP - diagPN) < 0.02f);
    CHECK(std::abs(diagPP - diagNN) < 0.02f);

    // Part B: Opaque Stone barrier occlusion
    sim::SimWorld occ(65, 65, 1);
    occ.setBoundaryMode(sim::BoundaryMode::Closed);
    occ.setGravityMode(sim::GravityMode::ZeroG);
    for (int y = 24; y <= 40; ++y) {
        occ.setCell(34, y, sim::MaterialId::Stone);
    }
    for (int t = 0; t < 12; ++t) {
        occ.setCell(32, 32, sim::MaterialId::Lava);
        occ.setCellTemp(32, 32, 1800.0f);
        occ.step();
    }
    const float openSide = occ.inspect(28, 32).lightR;
    const float behindStone = occ.inspect(36, 32).lightR;
    CHECK(openSide > 0.35f);
    CHECK(behindStone < openSide * 0.45f);
}

void testExplosivesCannonBreach() {
    const int sizes[][2] = {{160, 90}, {64, 64}};
    for (const auto &sz : sizes) {
        const int W = sz[0];
        const int H = sz[1];
        sim::SimWorld world(W, H, 2);
        world.loadPreset(sim::PresetId::Explosives);

        const uint32_t initWood = world.countMaterial(sim::MaterialId::Wood);
        const uint32_t initGlass = world.countMaterial(sim::MaterialId::Glass);
        const uint32_t initMethane = world.countMaterial(sim::MaterialId::Methane);
        CHECK(initWood > 0 && initGlass > 0 && initMethane > 0);

        for (int t = 0; t < 180; ++t) {
            world.step();
        }

        CHECK(world.countMaterial(sim::MaterialId::Gunpowder) == 0);
        CHECK(world.countMaterial(sim::MaterialId::Nitro) == 0);

        // Sand slug must exit the Obsidian cannon barrel (x > W * 50 / 100)
        bool slugExitedBarrel = false;
        for (int y = 0; y < H; ++y) {
            for (int x = W * 50 / 100 + 1; x < W; ++x) {
                if (world.inspect(x, y).material == sim::MaterialId::Sand) {
                    slugExitedBarrel = true;
                    break;
                }
            }
            if (slugExitedBarrel) break;
        }
        CHECK(slugExitedBarrel);

        // Slug breaches the Wood wall and shatters/ignites the Glass/Methane target
        CHECK(world.countMaterial(sim::MaterialId::Wood) < initWood);
        CHECK(world.countMaterial(sim::MaterialId::Glass) < initGlass);
        CHECK(world.countMaterial(sim::MaterialId::Methane) < initMethane);
    }
}

void testVolcanoEruptionAndLavaQuenching() {
    // Part A: Volcano periodic eruption & active plumes
    sim::SimWorld volcano(160, 90, 2);
    volcano.loadPreset(sim::PresetId::Volcano);

    float peakPressure = 0.0f;
    bool plumeAboveCrater = false;
    for (int t = 0; t < 180; ++t) {
        volcano.step();
        peakPressure = std::max(peakPressure, volcano.stats().maxPressure);
        if (t >= 20) {
            for (int y = 1; y < volcano.height() * 27 / 100; ++y) {
                for (int x = volcano.width() * 35 / 100; x <= volcano.width() * 65 / 100; ++x) {
                    const auto m = volcano.inspect(x, y).material;
                    if (m == sim::MaterialId::Lava || m == sim::MaterialId::Spark || m == sim::MaterialId::Fire ||
                        m == sim::MaterialId::Smoke || m == sim::MaterialId::Steam || m == sim::MaterialId::Obsidian) {
                        plumeAboveCrater = true;
                    }
                }
            }
        }
    }
    CHECK(peakPressure > 0.5f);
    CHECK(plumeAboveCrater);
    CHECK(std::isfinite(volcano.stats().totalThermalEnergy));

    // Part B: Direct Lava + Water quenching -> Obsidian + Steam
    sim::SimWorld quench(48, 48, 1);
    quench.setBoundaryMode(sim::BoundaryMode::Closed);
    for (int x = 14; x <= 34; ++x)
        quench.setCell(x, 38, sim::MaterialId::Bedrock);
    for (int y = 26; y <= 38; ++y) {
        quench.setCell(14, y, sim::MaterialId::Bedrock);
        quench.setCell(34, y, sim::MaterialId::Bedrock);
    }
    for (int y = 32; y <= 37; ++y) {
        for (int x = 15; x <= 24; ++x)
            quench.setCell(x, y, sim::MaterialId::Lava);
        for (int x = 25; x <= 33; ++x)
            quench.setCell(x, y, sim::MaterialId::Water);
    }

    for (int t = 0; t < 40; ++t)
        quench.step();
    CHECK(quench.countMaterial(sim::MaterialId::Obsidian) >= 1);
    CHECK(quench.countMaterial(sim::MaterialId::Steam) + quench.countMaterial(sim::MaterialId::Water) >= 1);
    CHECK(std::isfinite(quench.stats().maxTempK) && quench.stats().maxTempK <= 8000.0f);
}

void testHydrostaticLevelingSandStabilityAndSaltBridge() {
    // Part A: Stepped Water pool levels smoothly without trapped air bubbles
    sim::SimWorld basin(64, 48, 1);
    for (int x = 8; x <= 55; ++x)
        basin.setCell(x, 42, sim::MaterialId::Glass);
    for (int y = 20; y <= 42; ++y) {
        basin.setCell(8, y, sim::MaterialId::Glass);
        basin.setCell(55, y, sim::MaterialId::Glass);
    }
    for (int y = 24; y <= 41; ++y) {
        for (int x = 9; x <= 28; ++x)
            basin.setCell(x, y, sim::MaterialId::Water);
    }
    for (int t = 0; t < 200; ++t)
        basin.step();

    int leftW = 0, rightW = 0, trappedAir = 0;
    for (int y = 20; y <= 41; ++y) {
        for (int x = 9; x <= 31; ++x) {
            if (basin.inspect(x, y).material == sim::MaterialId::Water) ++leftW;
        }
        for (int x = 32; x <= 54; ++x) {
            if (basin.inspect(x, y).material == sim::MaterialId::Water) ++rightW;
        }
    }
    for (int y = 35; y <= 41; ++y) {
        for (int x = 9; x <= 54; ++x) {
            if (basin.inspect(x, y).material == sim::MaterialId::Air) ++trappedAir;
        }
    }
    CHECK(std::abs(leftW - rightW) <= 18);
    CHECK(trappedAir == 0);

    // Part B: Resting Sand pile stability (no phantom drift after settling)
    sim::SimWorld pile(48, 48, 1);
    for (int x = 10; x <= 38; ++x)
        pile.setCell(x, 40, sim::MaterialId::Stone);
    for (int y = 30; y <= 36; ++y) {
        for (int x = 20; x <= 28; ++x)
            pile.setCell(x, y, sim::MaterialId::Sand);
    }
    for (int t = 0; t < 100; ++t)
        pile.step();

    std::vector<uint8_t> sandSnap(static_cast<size_t>(pile.pixelCount()), 0);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            if (pile.inspect(x, y).material == sim::MaterialId::Sand) {
                CHECK(y < 40);
                sandSnap[static_cast<size_t>(y * 48 + x)] = 1;
            }
        }
    }
    for (int t = 0; t < 60; ++t)
        pile.step();
    int movedSand = 0;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const uint8_t isSand = (pile.inspect(x, y).material == sim::MaterialId::Sand) ? 1u : 0u;
            if (isSand != sandSnap[static_cast<size_t>(y * 48 + x)]) ++movedSand;
        }
    }
    CHECK(movedSand == 0);

    // Part C: Battery -> Metal -> SaltWater electrolyte bridge -> Metal -> Gunpowder (lower rows y=37..40)
    sim::SimWorld saltCircuit(64, 48, 1);
    saltCircuit.setGravityMode(sim::GravityMode::ZeroG);
    for (int x = 20; x <= 26; ++x) {
        saltCircuit.setCell(x, 37, sim::MaterialId::Glass);
        saltCircuit.setCell(x, 40, sim::MaterialId::Glass);
    }
    for (int y = 38; y <= 39; ++y) {
        saltCircuit.setCell(6, y, sim::MaterialId::Battery);
        saltCircuit.setCell(7, y, sim::MaterialId::Battery);
        for (int x = 8; x <= 20; ++x)
            saltCircuit.setCell(x, y, sim::MaterialId::Metal);
        for (int x = 21; x <= 25; ++x)
            saltCircuit.setCell(x, y, sim::MaterialId::SaltWater);
        for (int x = 26; x <= 42; ++x)
            saltCircuit.setCell(x, y, sim::MaterialId::Metal);
    }
    saltCircuit.setCell(43, 38, sim::MaterialId::Gunpowder);

    for (int t = 0; t < 110; ++t)
        saltCircuit.step();
    CHECK(saltCircuit.inspect(23, 38).charge > 0.25f);
    CHECK(saltCircuit.inspect(23, 38).tempK > sim::kAmbientTempK + 0.3f);
    CHECK(saltCircuit.inspect(35, 38).charge > 0.20f);
    CHECK(saltCircuit.countMaterial(sim::MaterialId::Gunpowder) == 0);
}

void testReactionsRichness() {
    // 1. Acid corrosion generates gas bubbles (Methane/Smoke) and consumes Metal
    sim::SimWorld acidW(48, 48, 1);
    for (int x = 18; x <= 28; ++x)
        acidW.setCell(x, 34, sim::MaterialId::Bedrock);
    for (int y = 26; y <= 34; ++y) {
        acidW.setCell(18, y, sim::MaterialId::Bedrock);
        acidW.setCell(28, y, sim::MaterialId::Bedrock);
    }
    for (int x = 19; x <= 27; ++x)
        acidW.setCell(x, 33, sim::MaterialId::Metal);
    for (int x = 19; x <= 27; ++x)
        acidW.setCell(x, 32, sim::MaterialId::Acid);
    const uint32_t initMetal = acidW.countMaterial(sim::MaterialId::Metal);
    bool sawGasBubble = false;
    for (int t = 0; t < 90; ++t) {
        acidW.step();
        if (acidW.countMaterial(sim::MaterialId::Methane) + acidW.countMaterial(sim::MaterialId::Smoke) > 0) {
            sawGasBubble = true;
        }
    }
    CHECK(acidW.countMaterial(sim::MaterialId::Metal) < initMetal);
    CHECK(sawGasBubble);

    // 2. Supercooled Water dendritic Ice crystal nucleation off existing Ice
    sim::SimWorld seeded(48, 48, 1);
    seeded.setGravityMode(sim::GravityMode::ZeroG);
    seeded.setAmbientTempK(270.0f);
    for (int y = 18; y <= 28; ++y) {
        for (int x = 18; x <= 28; ++x) {
            seeded.setCell(x, y, sim::MaterialId::Water);
            seeded.setCellTemp(x, y, 270.5f);
        }
    }
    seeded.setCell(23, 23, sim::MaterialId::Ice);
    seeded.setCellTemp(23, 23, 260.0f);

    sim::SimWorld unseeded(48, 48, 1);
    unseeded.setGravityMode(sim::GravityMode::ZeroG);
    unseeded.setAmbientTempK(270.0f);
    for (int y = 18; y <= 28; ++y) {
        for (int x = 18; x <= 28; ++x) {
            unseeded.setCell(x, y, sim::MaterialId::Water);
            unseeded.setCellTemp(x, y, 270.5f);
        }
    }
    for (int t = 0; t < 35; ++t) {
        seeded.step();
        unseeded.step();
    }
    CHECK(seeded.countMaterial(sim::MaterialId::Ice) > 4);
    CHECK(unseeded.countMaterial(sim::MaterialId::Ice) == 0);

    // 3. Plant hydration/growth without exhausting water pools rapidly
    sim::SimWorld garden(48, 48, 1);
    for (int x = 14; x <= 34; ++x)
        garden.setCell(x, 40, sim::MaterialId::Glass);
    for (int y = 32; y <= 40; ++y) {
        garden.setCell(14, y, sim::MaterialId::Glass);
        garden.setCell(34, y, sim::MaterialId::Glass);
    }
    for (int y = 35; y <= 39; ++y) {
        for (int x = 15; x <= 33; ++x)
            garden.setCell(x, y, sim::MaterialId::Water);
    }
    garden.setCell(24, 34, sim::MaterialId::Plant);
    const uint32_t initWater = garden.countMaterial(sim::MaterialId::Water);
    for (int t = 0; t < 150; ++t)
        garden.step();
    CHECK(garden.countMaterial(sim::MaterialId::Plant) > 2);
    CHECK(garden.countMaterial(sim::MaterialId::Water) > initWater / 2);
}

void testAllPresetsMinSizeAndMultithreadedFullGridFinite() {
    const sim::PresetId presets[] = {sim::PresetId::Laboratory,    sim::PresetId::Volcano,
                                     sim::PresetId::Hydrodynamics, sim::PresetId::Explosives,
                                     sim::PresetId::ElectricGrid,  sim::PresetId::ZeroGOrb};
    const sim::ViewMode views[] = {sim::ViewMode::Physical, sim::ViewMode::Thermal, sim::ViewMode::Pressure,
                                   sim::ViewMode::Velocity, sim::ViewMode::EnergyLight};

    const struct {
        int w;
        int h;
        unsigned threads;
        int steps;
    } configs[] = {{64, 64, 1, 100}, {160, 90, 4, 100}};

    for (const auto &cfg : configs) {
        for (auto p : presets) {
            sim::SimWorld world(cfg.w, cfg.h, cfg.threads);
            world.loadPreset(p);
            for (int t = 0; t < cfg.steps; ++t) {
                world.step();
            }

            const auto &st = world.stats();
            CHECK(std::isfinite(st.totalThermalEnergy));
            CHECK(std::isfinite(st.totalKineticEnergy));
            CHECK(std::isfinite(st.minTempK) && st.minTempK >= 1.0f);
            CHECK(std::isfinite(st.maxTempK) && st.maxTempK <= 8000.0f);
            CHECK(std::isfinite(st.maxPressure));

            // Full-grid inspection (every pixel, zero NaNs/Infs)
            for (int y = 0; y < world.height(); ++y) {
                for (int x = 0; x < world.width(); ++x) {
                    const auto px = world.inspect(x, y);
                    CHECK(std::isfinite(px.tempK) && px.tempK >= 1.0f && px.tempK <= 8000.0f);
                    CHECK(std::isfinite(px.pressure));
                    CHECK(std::isfinite(px.vx) && std::isfinite(px.vy));
                    CHECK(std::isfinite(px.airVx) && std::isfinite(px.airVy));
                    CHECK(std::isfinite(px.charge));
                    CHECK(std::isfinite(px.lightR) && std::isfinite(px.lightG) && std::isfinite(px.lightB));
                    CHECK(std::isfinite(px.life));
                }
            }

            for (auto v : views) {
                world.render(v);
                const uint32_t *fb = world.framebuffer();
                bool hasNonBlack = false;
                for (int i = 0; i < world.pixelCount(); ++i) {
                    if ((fb[i] & 0x00FFFFFFu) != 0u) {
                        hasNonBlack = true;
                        break;
                    }
                }
                CHECK(hasNonBlack);
            }
        }
    }
}

void testRendererDiagnosticsLinesAndPPM() {
    sim::SimWorld world(64, 48, 2);
    for (int y = 18; y <= 28; ++y) {
        for (int x = 24; x <= 34; ++x) {
            world.setCell(x, y, sim::MaterialId::Stone);
        }
    }
    for (int y = 32; y <= 40; ++y) {
        for (int x = 8; x <= 16; ++x) {
            world.setCell(x, y, sim::MaterialId::Water);
        }
    }
    world.detonate(46, 30, 4, 3.0f, 1500.0f);
    for (int t = 0; t < 10; ++t)
        world.step();

    // Solid obstacle silhouettes must remain clearly legible in Thermal, Pressure, and Velocity modes
    const sim::ViewMode diagViews[] = {sim::ViewMode::Thermal, sim::ViewMode::Pressure, sim::ViewMode::Velocity};
    for (auto vm : diagViews) {
        world.render(vm);
        const uint32_t *fb = world.framebuffer();
        const uint32_t edgePx = fb[world.index(24, 18)];
        const uint32_t innerPx = fb[world.index(29, 23)];
        const uint32_t airPx = fb[world.index(23, 18)];
        CHECK(edgePx != innerPx);
        CHECK(edgePx != airPx);
    }

    // Continuous line drawing (Shift+Drag backend) leaves zero horizontal gaps
    sim::SimWorld lineWorld(64, 48, 1);
    lineWorld.paintLine(4, 6, 40, 22, 0, sim::MaterialId::Bedrock);
    CHECK(lineWorld.inspect(4, 6).material == sim::MaterialId::Bedrock);
    CHECK(lineWorld.inspect(40, 22).material == sim::MaterialId::Bedrock);
    for (int x = 4; x <= 40; ++x) {
        bool colHasBedrock = false;
        for (int y = 6; y <= 22; ++y) {
            if (lineWorld.inspect(x, y).material == sim::MaterialId::Bedrock) {
                colHasBedrock = true;
                break;
            }
        }
        CHECK(colHasBedrock);
    }

    // Valid binary P6 PPM export with exact payload size
    const std::string ppmPath = "test_verify_tmp.ppm";
    CHECK(world.savePPM(ppmPath, sim::ViewMode::Physical));
    {
        std::ifstream in(ppmPath, std::ios::binary | std::ios::ate);
        CHECK(in.good());
        const auto totalBytes = static_cast<size_t>(in.tellg());
        in.seekg(0);
        std::string magic;
        int pw = 0, ph = 0, pmax = 0;
        in >> magic >> pw >> ph >> pmax;
        CHECK(magic == "P6");
        CHECK(pw == 64 && ph == 48 && pmax == 255);
        in.get(); // consume single whitespace separator after 255
        const auto headerBytes = static_cast<size_t>(in.tellg());
        CHECK(totalBytes == headerBytes + static_cast<size_t>(64 * 48 * 3));
    }
    std::remove(ppmPath.c_str());
}

void testPresets480x270Functional() {
    // 1. Laboratory: Gunpowder fuse on elevated Wood shelf ignites cleanly at default 480x270
    {
        sim::SimWorld lab(480, 270, 4);
        lab.loadPreset(sim::PresetId::Laboratory);
        const uint32_t initGP = lab.countMaterial(sim::MaterialId::Gunpowder);
        CHECK(initGP > 100);
        for (int t = 0; t < 200; ++t)
            lab.step();
        CHECK(lab.countMaterial(sim::MaterialId::Gunpowder) == 0);
    }

    // 2. Hydrodynamics: U-tube communicating vessels equalize water from left arm up into right arm at 480x270
    {
        sim::SimWorld hydro(480, 270, 4);
        hydro.loadPreset(sim::PresetId::Hydrodynamics);
        auto measureRightArm = [&]() -> std::pair<int, int> {
            int cnt = 0;
            int minY = 270;
            for (int y = 0; y < 270; ++y) {
                for (int x = 300; x < 410; ++x) {
                    const auto m = hydro.inspect(x, y).material;
                    if (m == sim::MaterialId::Water || m == sim::MaterialId::SaltWater) {
                        ++cnt;
                        if (y < minY) minY = y;
                    }
                }
            }
            return {cnt, minY};
        };
        const auto [initRightLiq, initTopY] = measureRightArm();
        for (int t = 0; t < 350; ++t)
            hydro.step();
        const auto [afterRightLiq, afterTopY] = measureRightArm();
        CHECK(afterTopY <= initTopY - 10);
        CHECK(afterRightLiq > initRightLiq + 400);
    }

    // 3. ElectricGrid: 4V Battery -> Metal -> SaltWater electrolyte bridge -> Metal ignites Gunpowder at 480x270
    {
        sim::SimWorld elec(480, 270, 4);
        elec.loadPreset(sim::PresetId::ElectricGrid);
        CHECK(elec.countMaterial(sim::MaterialId::Gunpowder) > 0);
        for (int t = 0; t < 260; ++t)
            elec.step();
        CHECK(elec.countMaterial(sim::MaterialId::Gunpowder) == 0);
    }
}

void testZeroGMomentumAndObliqueCollision() {
    auto sumMomentum = [](const sim::SimWorld &w, double &outPx, double &outPy, double &outKE) {
        outPx = 0.0;
        outPy = 0.0;
        outKE = 0.0;
        for (int y = 0; y < w.height(); ++y) {
            for (int x = 0; x < w.width(); ++x) {
                const auto px = w.inspect(x, y);
                if (px.material == sim::MaterialId::Air) continue;
                const auto &props = sim::getMaterial(px.material);
                if (!props.isMovable()) continue;
                const double m = static_cast<double>(props.density);
                outPx += m * static_cast<double>(px.vx);
                outPy += m * static_cast<double>(px.vy);
                outKE += 0.5 * m *
                         (static_cast<double>(px.vx) * static_cast<double>(px.vx) +
                          static_cast<double>(px.vy) * static_cast<double>(px.vy));
            }
        }
    };

    // 1. Symmetric head-on ZeroG collision (equal Sand grains)
    {
        sim::SimWorld sym(64, 48, 1);
        sym.setBoundaryMode(sim::BoundaryMode::Closed);
        sym.setGravityMode(sim::GravityMode::ZeroG);
        sym.setCell(20, 24, sim::MaterialId::Sand);
        sym.setCellVelocity(20, 24, 3.5f, 0.0f);
        sym.setCell(22, 24, sim::MaterialId::Sand);
        sym.setCellVelocity(22, 24, -3.5f, 0.0f);

        for (int t = 0; t < 3; ++t)
            sym.step();
        double px = 0.0, py = 0.0, ke = 0.0;
        sumMomentum(sym, px, py, ke);
        CHECK(sym.countMaterial(sim::MaterialId::Sand) == 2);
        CHECK(std::abs(px) < 0.35);
        CHECK(std::abs(py) < 0.05);
    }

    // 2. Asymmetric head-on ZeroG collision (Sand rho=1.65 vs stationary Salt rho=1.45)
    {
        sim::SimWorld asym(64, 48, 1);
        asym.setBoundaryMode(sim::BoundaryMode::Closed);
        asym.setGravityMode(sim::GravityMode::ZeroG);
        asym.setCell(20, 24, sim::MaterialId::Sand);
        asym.setCellVelocity(20, 24, 4.0f, 0.0f);
        asym.setCell(21, 24, sim::MaterialId::Salt);
        asym.setCellVelocity(21, 24, 0.0f, 0.0f);

        double px0 = 0.0, py0 = 0.0, ke0 = 0.0;
        sumMomentum(asym, px0, py0, ke0);

        asym.step();

        double px1 = 0.0, py1 = 0.0, ke1 = 0.0;
        sumMomentum(asym, px1, py1, ke1);
        CHECK(asym.countMaterial(sim::MaterialId::Sand) == 1);
        CHECK(asym.countMaterial(sim::MaterialId::Salt) == 1);
        // Linear momentum conserved up to ~6% aerodynamic drag coupling over 1 tick, no kinetic energy blowup
        CHECK(px1 > px0 * 0.86 && px1 <= px0 * 1.01);
        CHECK(std::abs(py1) < 0.05);
        CHECK(ke1 < ke0);

        // Salt target must gain substantial rightward velocity
        bool saltAccelerated = false;
        for (int x = 21; x <= 26; ++x) {
            const auto info = asym.inspect(x, 24);
            if (info.material == sim::MaterialId::Salt && info.vx > 1.2f) {
                saltAccelerated = true;
            }
        }
        CHECK(saltAccelerated);
    }

    // 3. Oblique 2D ZeroG collision (two Sand grains with non-collinear velocities)
    {
        sim::SimWorld obl(64, 48, 1);
        obl.setBoundaryMode(sim::BoundaryMode::Closed);
        obl.setGravityMode(sim::GravityMode::ZeroG);
        obl.setCell(20, 24, sim::MaterialId::Sand);
        obl.setCellVelocity(20, 24, 3.5f, 1.5f);
        obl.setCell(21, 24, sim::MaterialId::Sand);
        obl.setCellVelocity(21, 24, -1.5f, 2.0f);

        double px0 = 0.0, py0 = 0.0, ke0 = 0.0;
        sumMomentum(obl, px0, py0, ke0);

        obl.step();

        double px1 = 0.0, py1 = 0.0, ke1 = 0.0;
        sumMomentum(obl, px1, py1, ke1);
        CHECK(obl.countMaterial(sim::MaterialId::Sand) == 2);
        CHECK(px1 > px0 * 0.85 && px1 <= px0 * 1.02);
        CHECK(py1 > py0 * 0.85 && py1 <= py0 * 1.02);
        CHECK(ke1 < ke0);
    }

    // 3b. Diagonal-approach oblique 2D ZeroG collision (45 deg impact on stationary Salt)
    {
        sim::SimWorld diag(64, 48, 1);
        diag.setBoundaryMode(sim::BoundaryMode::Closed);
        diag.setGravityMode(sim::GravityMode::ZeroG);
        diag.setCell(20, 20, sim::MaterialId::Sand);
        diag.setCellVelocity(20, 20, 3.0f, 3.0f);
        diag.setCell(21, 21, sim::MaterialId::Salt);
        diag.setCellVelocity(21, 21, 0.0f, 0.0f);

        double px0 = 0.0, py0 = 0.0, ke0 = 0.0;
        sumMomentum(diag, px0, py0, ke0);

        diag.step();

        double px1 = 0.0, py1 = 0.0, ke1 = 0.0;
        sumMomentum(diag, px1, py1, ke1);
        CHECK(diag.countMaterial(sim::MaterialId::Sand) == 1);
        CHECK(diag.countMaterial(sim::MaterialId::Salt) == 1);
        CHECK(px1 > px0 * 0.85 && px1 <= px0 * 1.02);
        CHECK(py1 > py0 * 0.85 && py1 <= py0 * 1.02);
        CHECK(ke1 < ke0);

        bool saltMovedDiag = false;
        for (int y = 21; y <= 24; ++y) {
            for (int x = 21; x <= 24; ++x) {
                const auto info = diag.inspect(x, y);
                if (info.material == sim::MaterialId::Salt && info.vx > 0.9f && info.vy > 0.9f) {
                    saltMovedDiag = true;
                }
            }
        }
        CHECK(saltMovedDiag);
    }

    // 4. Sub-pixel fractional motion accuracy in ZeroG
    {
        sim::SimWorld frac(48, 48, 1);
        frac.setBoundaryMode(sim::BoundaryMode::Closed);
        frac.setGravityMode(sim::GravityMode::ZeroG);
        frac.setCell(10, 24, sim::MaterialId::Sand);
        frac.setCellVelocity(10, 24, 0.45f, 0.22f);

        // After 1 tick: accumulated fx < 1.0, fy < 1.0 -> stays at (10, 24)
        frac.step();
        CHECK(frac.inspect(10, 24).material == sim::MaterialId::Sand);

        // After 3 ticks total: fx > 1.0 while fy < 1.0 -> stepped right in X, still y == 24
        frac.step();
        frac.step();
        CHECK(frac.inspect(10, 24).material == sim::MaterialId::Air);
        CHECK(frac.inspect(11, 24).material == sim::MaterialId::Sand);

        // After 6 ticks total: fy also crosses 1.0 -> reaches y >= 25
        for (int t = 0; t < 3; ++t)
            frac.step();
        bool foundMoved = false;
        for (int y = 25; y <= 27; ++y) {
            for (int x = 11; x <= 14; ++x) {
                if (frac.inspect(x, y).material == sim::MaterialId::Sand) foundMoved = true;
            }
        }
        CHECK(foundMoved);
        CHECK(frac.countMaterial(sim::MaterialId::Sand) == 1);
    }
}

void testDiagonalBarrierNoLeak() {
    // Part A: Sealed 1-px diagonal Stone staircase holding Water + fast ballistic Sand under Earth gravity
    {
        sim::SimWorld world(64, 64, 1);
        world.setBoundaryMode(sim::BoundaryMode::Closed);
        world.setGravityMode(sim::GravityMode::Earth);

        // Build closed upper-right wedge bounded by x = 12 (top-left cap), x = 50 (right wall), and y = x diagonal
        for (int y = 1; y <= 12; ++y)
            world.setCell(12, y, sim::MaterialId::Stone);
        for (int y = 1; y <= 50; ++y)
            world.setCell(50, y, sim::MaterialId::Stone);
        for (int k = 12; k <= 50; ++k)
            world.setCell(k, k, sim::MaterialId::Stone);

        // Fill Water inside upper-right triangle (x > y)
        uint32_t initWater = 0;
        for (int x = 20; x <= 48; ++x) {
            for (int y = 22; y < x; ++y) {
                world.setCell(x, y, sim::MaterialId::Water);
                ++initWater;
            }
        }
        // Launch fast ballistic Sand downward and diagonally onto the 1-px Stone staircase
        world.setCell(28, 10, sim::MaterialId::Sand);
        world.setCellVelocity(28, 10, 0.0f, 8.5f);
        world.setCell(34, 12, sim::MaterialId::Sand);
        world.setCellVelocity(34, 12, -5.0f, 5.0f);
        const uint32_t initSand = world.countMaterial(sim::MaterialId::Sand);
        CHECK(initWater > 100 && initSand == 2);

        for (int t = 0; t < 180; ++t)
            world.step();

        CHECK(world.countMaterial(sim::MaterialId::Water) == initWater);
        CHECK(world.countMaterial(sim::MaterialId::Sand) == initSand);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto m = world.inspect(x, y).material;
                if (m == sim::MaterialId::Water || m == sim::MaterialId::Sand) {
                    // Must strictly remain on the upper-right side of the diagonal barrier (x > y)
                    CHECK(x > y && x > 12 && x < 50);
                }
            }
        }
    }

    // Part B: Sealed 1-px diagonal Sand dam blocking diagonal Water & Sand probes in ZeroG (no diagonal teleport)
    {
        sim::SimWorld dam(64, 64, 1);
        dam.setBoundaryMode(sim::BoundaryMode::Closed);
        dam.setGravityMode(sim::GravityMode::ZeroG);

        // 1-px diagonal Sand dam anchored by Stone corner walls
        for (int y = 1; y <= 14; ++y)
            dam.setCell(14, y, sim::MaterialId::Stone);
        for (int y = 14; y <= 50; ++y)
            dam.setCell(50, y, sim::MaterialId::Stone);
        for (int x = 14; x <= 50; ++x)
            dam.setCell(x, 14, sim::MaterialId::Stone);
        dam.setCell(50, 50, sim::MaterialId::Stone);
        for (int k = 15; k <= 49; ++k)
            dam.setCell(k, k, sim::MaterialId::Sand);

        // Spaced Water & Sand probes moving diagonally (-1.1, +1.1) directly at gaps (k+1, k) -> (k, k+1)
        const int waterK[] = {20, 28, 36};
        for (int k : waterK) {
            dam.setCell(k + 1, k, sim::MaterialId::Water);
            dam.setCellVelocity(k + 1, k, -1.1f, 1.1f);
        }
        dam.setCell(43, 42, sim::MaterialId::Sand);
        dam.setCellVelocity(43, 42, -1.0f, 1.0f);

        for (int t = 0; t < 6; ++t)
            dam.step();

        CHECK(dam.countMaterial(sim::MaterialId::Water) == 3);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto m = dam.inspect(x, y).material;
                if (m == sim::MaterialId::Water) CHECK(x > y);
                if (m == sim::MaterialId::Sand) CHECK(x >= y);
            }
        }
    }

    // Part C: Sealed 1-px diagonal Ice staircase holding Water + Sand under Earth gravity
    {
        sim::SimWorld iceStair(64, 64, 1);
        iceStair.setBoundaryMode(sim::BoundaryMode::Closed);
        iceStair.setGravityMode(sim::GravityMode::Earth);
        iceStair.setAmbientTempK(273.05f);
        iceStair.clear(sim::MaterialId::Air);

        for (int y = 1; y <= 12; ++y) {
            iceStair.setCell(12, y, sim::MaterialId::Stone);
            iceStair.setCellTemp(12, y, 273.05f);
        }
        for (int y = 1; y <= 50; ++y) {
            iceStair.setCell(50, y, sim::MaterialId::Stone);
            iceStair.setCellTemp(50, y, 273.05f);
        }
        for (int k = 12; k <= 50; ++k) {
            iceStair.setCell(k, k, sim::MaterialId::Ice);
            iceStair.setCellTemp(k, k, 272.95f);
        }

        uint32_t initWater = 0;
        for (int x = 22; x <= 48; ++x) {
            for (int y = 24; y < x; ++y) {
                iceStair.setCell(x, y, sim::MaterialId::Water);
                iceStair.setCellTemp(x, y, 273.10f);
                ++initWater;
            }
        }
        iceStair.setCell(30, 12, sim::MaterialId::Sand);
        iceStair.setCellTemp(30, 12, 273.05f);
        iceStair.setCellVelocity(30, 12, 0.0f, 4.0f);

        for (int t = 0; t < 160; ++t)
            iceStair.step();

        CHECK(iceStair.countMaterial(sim::MaterialId::Ice) == 39);
        CHECK(iceStair.countMaterial(sim::MaterialId::Water) == initWater);
        CHECK(iceStair.countMaterial(sim::MaterialId::Sand) == 1);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto m = iceStair.inspect(x, y).material;
                if (m == sim::MaterialId::Water || m == sim::MaterialId::Sand) {
                    CHECK(x > y && x > 12 && x < 50);
                }
            }
        }
    }

    // Part D: Resting Sand dam holding back a Water pool under Earth gravity (no seepage through Sand slope)
    {
        sim::SimWorld sandDam(64, 64, 1);
        sandDam.setBoundaryMode(sim::BoundaryMode::Closed);
        sandDam.setGravityMode(sim::GravityMode::Earth);

        for (int x = 10; x <= 52; ++x)
            sandDam.setCell(x, 48, sim::MaterialId::Stone);
        for (int y = 24; y <= 48; ++y) {
            sandDam.setCell(10, y, sim::MaterialId::Stone);
            sandDam.setCell(52, y, sim::MaterialId::Stone);
        }
        // Thick 45-deg Sand dam separating left Air chamber (x <= 18) from right Water pool (x >= 32)
        for (int y = 28; y <= 47; ++y) {
            const int dx = y - 28;
            for (int x = 22 - dx / 2; x <= 26 + dx; ++x) {
                if (x > 10 && x < 52) sandDam.setCell(x, y, sim::MaterialId::Sand);
            }
        }
        uint32_t initWater = 0;
        for (int y = 38; y <= 47; ++y) {
            for (int x = 44; x <= 51; ++x) {
                sandDam.setCell(x, y, sim::MaterialId::Water);
                ++initWater;
            }
        }
        const uint32_t initSand = sandDam.countMaterial(sim::MaterialId::Sand);
        for (int t = 0; t < 120; ++t)
            sandDam.step();

        CHECK(sandDam.countMaterial(sim::MaterialId::Water) == initWater);
        CHECK(sandDam.countMaterial(sim::MaterialId::Sand) == initSand);
        for (int y = 0; y < 64; ++y) {
            for (int x = 11; x <= 18; ++x) {
                CHECK(sandDam.inspect(x, y).material != sim::MaterialId::Water);
            }
        }
    }
}

void testSlopeNormalAndFloorBounce() {
    // Part A: Fast Spark falling straight down (0, +4) onto 45 deg Stone ramp deflects laterally (vx > 1.5)
    {
        sim::SimWorld ramp(64, 64, 1);
        ramp.setBoundaryMode(sim::BoundaryMode::Closed);
        ramp.setGravityMode(sim::GravityMode::Earth);

        // 45-degree Stone ramp facing up-right: outward normal (+1/sqrt(2), -1/sqrt(2))
        for (int x = 10; x <= 50; ++x) {
            for (int y = x + 6; y <= std::min(56, x + 12); ++y) {
                ramp.setCell(x, y, sim::MaterialId::Stone);
            }
        }

        ramp.setCell(24, 24, sim::MaterialId::Spark);
        ramp.setCellVelocity(24, 24, 0.0f, 4.0f);

        float maxVx = 0.0f;
        bool deflectedRight = false;
        for (int t = 0; t < 6; ++t) {
            ramp.step();
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const auto px = ramp.inspect(x, y);
                    if (px.material == sim::MaterialId::Spark) {
                        maxVx = std::max(maxVx, px.vx);
                        if (x > 24 && px.vx > 1.5f) deflectedRight = true;
                    }
                }
            }
        }
        CHECK(maxVx > 1.5f);
        CHECK(deflectedRight);
    }

    // Part B: Fast Spark hitting flat horizontal Stone floor diagonally (+3, +3) bounces upward (vy < -0.8)
    {
        sim::SimWorld floorWorld(64, 64, 1);
        floorWorld.setBoundaryMode(sim::BoundaryMode::Closed);
        floorWorld.setGravityMode(sim::GravityMode::Earth);

        for (int y = 36; y <= 40; ++y) {
            for (int x = 8; x <= 56; ++x) {
                floorWorld.setCell(x, y, sim::MaterialId::Stone);
            }
        }

        floorWorld.setCell(20, 31, sim::MaterialId::Spark);
        floorWorld.setCellVelocity(20, 31, 3.0f, 3.0f);

        float minVyAfterHit = 0.0f;
        bool bouncedUp = false;
        for (int t = 0; t < 6; ++t) {
            floorWorld.step();
            for (int y = 0; y < 36; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const auto px = floorWorld.inspect(x, y);
                    if (px.material == sim::MaterialId::Spark) {
                        minVyAfterHit = std::min(minVyAfterHit, px.vy);
                        if (px.vy < -0.8f && px.vx > 0.5f) bouncedUp = true;
                    }
                }
            }
        }
        CHECK(minVyAfterHit < -0.8f);
        CHECK(bouncedUp);
    }
}

void testFreeFallSandDoesNotShatterGlassOrWood() {
    // Part A: Sand dropped from top of a 120-tall grid under Earth gravity rests intact on 1-px Glass/Ice/Wood
    sim::SimWorld world(64, 120, 1);
    world.setBoundaryMode(sim::BoundaryMode::Closed);
    world.setGravityMode(sim::GravityMode::Earth);
    world.setAmbientTempK(260.0f); // Keep Ice frozen across entire Air field
    world.clear(sim::MaterialId::Air);

    // Bedrock side walls enclosing the roof so Sand falls directly onto the roof
    for (int y = 1; y <= 100; ++y) {
        world.setCell(9, y, sim::MaterialId::Bedrock);
        world.setCell(55, y, sim::MaterialId::Bedrock);
    }
    for (int x = 10; x <= 24; ++x)
        world.setCell(x, 100, sim::MaterialId::Glass);
    for (int x = 25; x <= 39; ++x)
        world.setCell(x, 100, sim::MaterialId::Ice);
    for (int x = 40; x <= 54; ++x)
        world.setCell(x, 100, sim::MaterialId::Wood);

    uint32_t initSand = 0;
    for (int y = 3; y <= 5; ++y) {
        for (int x = 12; x <= 52; x += 2) {
            world.setCell(x, y, sim::MaterialId::Sand);
            ++initSand;
        }
    }

    for (int t = 0; t < 130; ++t)
        world.step();

    CHECK(world.countMaterial(sim::MaterialId::Glass) == 15);
    CHECK(world.countMaterial(sim::MaterialId::Ice) == 15);
    CHECK(world.countMaterial(sim::MaterialId::Wood) == 15);
    CHECK(world.countMaterial(sim::MaterialId::Sand) == initSand);

    for (int y = 100; y < 120; ++y) {
        for (int x = 0; x < 64; ++x) {
            CHECK(world.inspect(x, y).material != sim::MaterialId::Sand);
        }
    }

    // Part B: High-velocity ballistic Sand (vy = 8.0) and cannon blast (Explosives preset) still breach
    {
        sim::SimWorld fast(48, 48, 1);
        fast.setBoundaryMode(sim::BoundaryMode::Closed);
        fast.setGravityMode(sim::GravityMode::ZeroG);
        for (int x = 10; x <= 38; ++x) {
            fast.setCell(x, 24, (x < 24) ? sim::MaterialId::Glass : sim::MaterialId::Wood);
        }
        fast.setCell(16, 20, sim::MaterialId::Sand);
        fast.setCellVelocity(16, 20, 0.0f, 8.0f);
        fast.setCell(30, 20, sim::MaterialId::Sand);
        fast.setCellVelocity(30, 20, 0.0f, 8.0f);
        for (int t = 0; t < 6; ++t)
            fast.step();
        CHECK(fast.countMaterial(sim::MaterialId::Glass) < 14);
        CHECK(fast.countMaterial(sim::MaterialId::Wood) < 15);
    }

    sim::SimWorld cannon(160, 90, 2);
    cannon.loadPreset(sim::PresetId::Explosives);
    const uint32_t initWood = cannon.countMaterial(sim::MaterialId::Wood);
    const uint32_t initGlass = cannon.countMaterial(sim::MaterialId::Glass);
    for (int t = 0; t < 180; ++t)
        cannon.step();
    CHECK(cannon.countMaterial(sim::MaterialId::Wood) < initWood);
    CHECK(cannon.countMaterial(sim::MaterialId::Glass) < initGlass);
}

void testBlastWallOcclusion() {
    sim::SimWorld world(80, 64, 1);
    world.setBoundaryMode(sim::BoundaryMode::Closed);
    world.setGravityMode(sim::GravityMode::ZeroG);

    // Closed 1-px Bedrock box around center (32, 32): x in [24..40], y in [20..44]
    for (int x = 24; x <= 40; ++x) {
        world.setCell(x, 20, sim::MaterialId::Bedrock);
        world.setCell(x, 44, sim::MaterialId::Bedrock);
    }
    for (int y = 20; y <= 44; ++y) {
        world.setCell(24, y, sim::MaterialId::Bedrock);
        world.setCell(40, y, sim::MaterialId::Bedrock);
    }
    const uint32_t initBedrock = world.countMaterial(sim::MaterialId::Bedrock);

    // Sand directly outside intact Bedrock walls (well within blast radius r=12, rShock=24)
    const int outsideCoords[][2] = {{23, 32}, {22, 32}, {41, 32}, {42, 32}, {32, 19}, {32, 18}, {32, 45}, {32, 46}};
    for (const auto &c : outsideCoords) {
        world.setCell(c[0], c[1], sim::MaterialId::Sand);
    }
    // Control Sand inside the box to confirm blast works inside
    world.setCell(36, 32, sim::MaterialId::Sand);

    world.detonate(32, 32, 12, 8.0f, 2400.0f);

    // Immediately after detonate(), outside Sand & temperatures must be completely unperturbed
    for (const auto &c : outsideCoords) {
        const auto px = world.inspect(c[0], c[1]);
        CHECK(px.material == sim::MaterialId::Sand);
        CHECK(std::abs(px.vx) < 1e-4f);
        CHECK(std::abs(px.vy) < 1e-4f);
        CHECK(std::abs(px.tempK - sim::kAmbientTempK) < 1e-3f);
    }
    // Inner Sand received radial outward impulse
    CHECK(world.inspect(36, 32).vx > 1.0f);

    for (int t = 0; t < 14; ++t)
        world.step();

    CHECK(world.countMaterial(sim::MaterialId::Bedrock) == initBedrock);
    for (const auto &c : outsideCoords) {
        const auto px = world.inspect(c[0], c[1]);
        CHECK(px.material == sim::MaterialId::Sand);
        CHECK(std::abs(px.vx) < 0.02f);
        CHECK(std::abs(px.vy) < 0.02f);
        CHECK(std::abs(px.airVx) < 0.02f);
        CHECK(std::abs(px.airVy) < 0.02f);
    }
}

void testLiquidNoTeleportOverOil() {
    sim::SimWorld world(48, 48, 1);
    world.setBoundaryMode(sim::BoundaryMode::Closed);
    world.setGravityMode(sim::GravityMode::Earth);

    // Setup 1: Stone shelf with drop-off at x = 23 (y = 28 -> leftToRight == true on tick 0)
    for (int y = 24; y <= 29; ++y)
        world.setCell(12, y, sim::MaterialId::Stone);
    for (int x = 12; x <= 22; ++x)
        world.setCell(x, 29, sim::MaterialId::Stone);
    for (int x = 20; x <= 27; ++x)
        world.setCell(x, 35, sim::MaterialId::Stone);
    for (int x = 13; x <= 18; ++x)
        world.setCell(x, 28, sim::MaterialId::Water);
    for (int x = 19; x <= 21; ++x)
        world.setCell(x, 28, sim::MaterialId::Oil);

    // Setup 2: Closed flat Stone trough (y = 38 -> leftToRight == true on tick 0)
    for (int y = 35; y <= 39; ++y) {
        world.setCell(12, y, sim::MaterialId::Stone);
        world.setCell(26, y, sim::MaterialId::Stone);
    }
    for (int x = 12; x <= 26; ++x)
        world.setCell(x, 39, sim::MaterialId::Stone);
    for (int x = 13; x <= 18; ++x)
        world.setCell(x, 38, sim::MaterialId::Water);
    for (int x = 19; x <= 22; ++x)
        world.setCell(x, 38, sim::MaterialId::Oil);

    const uint32_t initWater = world.countMaterial(sim::MaterialId::Water);
    const uint32_t initOil = world.countMaterial(sim::MaterialId::Oil);

    // After 1 tick, Water dispersing right must NOT skip over Oil cells into x >= 21
    world.step();

    for (int x = 21; x <= 26; ++x) {
        for (int y = 27; y <= 34; ++y) {
            CHECK(world.inspect(x, y).material != sim::MaterialId::Water);
        }
        CHECK(world.inspect(x, 38).material != sim::MaterialId::Water);
    }

    for (int t = 0; t < 80; ++t)
        world.step();
    CHECK(world.countMaterial(sim::MaterialId::Water) == initWater);
    CHECK(world.countMaterial(sim::MaterialId::Oil) == initOil);
}

} // namespace

int main() {
    runTest("MaterialConstants", testMaterialConstants);
    runTest("GravityAndNoTunneling", testGravityAndNoTunneling);
    runTest("DensityDisplacement", testDensityDisplacement);
    runTest("CommunicatingVessels", testCommunicatingVessels);
    runTest("ThermalConservationAndConductivity", testThermalConservationAndConductivity);
    runTest("PhaseTransitions", testPhaseTransitions);
    runTest("BlastPressureAndMomentum", testBlastPressureAndMomentum);
    runTest("ElectricityAndJouleHeating", testElectricityAndJouleHeating);
    runTest("ChemistryCombustionAndAcid", testChemistryCombustionAndAcid);
    runTest("ZeroGAndRadialGravity", testZeroGAndRadialGravity);
    runTest("AllPresetsAndViewsFinite", testAllPresetsAndViewsFinite);
    runTest("IsotropicLightingAndOcclusion", testIsotropicLightingAndOcclusion);
    runTest("ExplosivesCannonBreach", testExplosivesCannonBreach);
    runTest("VolcanoEruptionAndLavaQuenching", testVolcanoEruptionAndLavaQuenching);
    runTest("HydrostaticLevelingSandStabilityAndSaltBridge", testHydrostaticLevelingSandStabilityAndSaltBridge);
    runTest("ReactionsRichness", testReactionsRichness);
    runTest("AllPresetsMinSizeAndMultithreadedFullGridFinite", testAllPresetsMinSizeAndMultithreadedFullGridFinite);
    runTest("RendererDiagnosticsLinesAndPPM", testRendererDiagnosticsLinesAndPPM);
    runTest("Presets480x270Functional", testPresets480x270Functional);
    runTest("ZeroGMomentumAndObliqueCollision", testZeroGMomentumAndObliqueCollision);
    runTest("DiagonalBarrierNoLeak", testDiagonalBarrierNoLeak);
    runTest("SlopeNormalAndFloorBounce", testSlopeNormalAndFloorBounce);
    runTest("FreeFallSandDoesNotShatterGlassOrWood", testFreeFallSandDoesNotShatterGlassOrWood);
    runTest("BlastWallOcclusion", testBlastWallOcclusion);
    runTest("LiquidNoTeleportOverOil", testLiquidNoTeleportOverOil);

    std::cout << "\nRisultato test: " << gPassed << " passati, " << gFailed << " falliti.\n";
    return gFailed == 0 ? 0 : 1;
}
