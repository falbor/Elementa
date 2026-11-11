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

void check(bool cond, const std::string& expr, const char* file, int line) {
    if (!cond) {
        std::ostringstream oss;
        oss << file << ":" << line << " Assertion failed: " << expr;
        throw std::runtime_error(oss.str());
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

void runTest(const char* name, void (*fn)()) {
    try {
        fn();
        std::cout << "[PASS] " << name << "\n";
        ++gPassed;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << name << " -> " << e.what() << "\n";
        ++gFailed;
    }
}

void testMaterialConstants() {
    const auto& mats = sim::getAllMaterials();
    for (size_t i = 0; i < mats.size(); ++i) {
        const auto& m = mats[i];
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
    for (int x = 16; x <= 32; ++x) world.setCell(x, 42, sim::MaterialId::Glass);
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

    for (int t = 0; t < 180; ++t) world.step();

    double avgYSand = 0.0, avgYWater = 0.0, avgYOil = 0.0;
    int cSand = 0, cWater = 0, cOil = 0;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const auto m = world.inspect(x, y).material;
            if (m == sim::MaterialId::Sand) { avgYSand += y; ++cSand; }
            else if (m == sim::MaterialId::Water) { avgYWater += y; ++cWater; }
            else if (m == sim::MaterialId::Oil) { avgYOil += y; ++cOil; }
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
    for (int x = 10; x <= 54; ++x) world.setCell(x, 42, sim::MaterialId::Glass);
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

    for (int t = 0; t < 260; ++t) world.step();

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
    for (int t = 0; t < 120; ++t) world.step();
    CHECK(world.countMaterial(sim::MaterialId::Water) >= 1);

    // Salt dissolution in Water
    world.clear();
    world.setCell(20, 20, sim::MaterialId::Water);
    world.setCell(21, 20, sim::MaterialId::Salt);
    for (int t = 0; t < 40; ++t) world.step();
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
            if (world.inspect(x, y).material == sim::MaterialId::MoltenGlass)
                world.setCellTemp(x, y, 1000.0f);
    world.step();
    CHECK(world.countMaterial(sim::MaterialId::Glass) == 1);
}

void testBlastPressureAndMomentum() {
    sim::SimWorld world(64, 64, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);
    world.setCell(20, 32, sim::MaterialId::Sand);
    world.setCell(44, 32, sim::MaterialId::Sand);

    world.detonate(32, 32, 8, 6.0f, 2200.0f);
    for (int t = 0; t < 8; ++t) world.step();

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
    for (int x = 6; x <= 60; ++x) circuit.setCell(x, 16, sim::MaterialId::Metal);
    circuit.setCell(61, 16, sim::MaterialId::Gunpowder);
    for (int t = 0; t < 80; ++t) circuit.step();
    CHECK(circuit.inspect(55, 16).charge > 0.50f);
    CHECK(circuit.countMaterial(sim::MaterialId::Gunpowder) == 0);
}

void testChemistryCombustionAndAcid() {
    // Gunpowder ignition by Fire
    sim::SimWorld world(48, 48, 1);
    for (int x = 15; x <= 30; ++x) world.setCell(x, 30, sim::MaterialId::Gunpowder);
    for (int x = 14; x <= 31; ++x) world.setCell(x, 31, sim::MaterialId::Stone);
    world.setCell(15, 29, sim::MaterialId::Fire);

    for (int t = 0; t < 40; ++t) world.step();
    CHECK(world.countMaterial(sim::MaterialId::Gunpowder) < 16);

    // Acid corroding Stone
    sim::SimWorld acidWorld(48, 48, 1);
    for (int x = 18; x <= 26; ++x) acidWorld.setCell(x, 30, sim::MaterialId::Stone);
    for (int x = 19; x <= 25; ++x) acidWorld.setCell(x, 29, sim::MaterialId::Acid);
    const uint32_t initialStone = acidWorld.countMaterial(sim::MaterialId::Stone);
    for (int t = 0; t < 80; ++t) acidWorld.step();
    CHECK(acidWorld.countMaterial(sim::MaterialId::Stone) < initialStone);
}

void testZeroGAndRadialGravity() {
    sim::SimWorld zg(48, 48, 1);
    zg.setGravityMode(sim::GravityMode::ZeroG);
    zg.setCell(24, 24, sim::MaterialId::Sand);
    for (int t = 0; t < 40; ++t) zg.step();
    CHECK(zg.inspect(24, 24).material == sim::MaterialId::Sand);

    sim::SimWorld rg(48, 48, 1);
    rg.setGravityMode(sim::GravityMode::RadialCenter);
    rg.setCell(24, 24, sim::MaterialId::Stone);
    rg.setCell(10, 24, sim::MaterialId::Sand);
    for (int t = 0; t < 6; ++t) rg.step();
    CHECK(rg.inspect(10, 24).material == sim::MaterialId::Air);
    bool movedTowardCenter = false;
    for (int x = 11; x <= 23; ++x) {
        if (rg.inspect(x, 24).material == sim::MaterialId::Sand) movedTowardCenter = true;
    }
    CHECK(movedTowardCenter);
    for (int t = 0; t < 30; ++t) rg.step();
    CHECK(rg.inspect(23, 24).material == sim::MaterialId::Sand);
}

void testAllPresetsAndViewsFinite() {
    const sim::PresetId presets[] = {
        sim::PresetId::Laboratory,
        sim::PresetId::Volcano,
        sim::PresetId::Hydrodynamics,
        sim::PresetId::Explosives,
        sim::PresetId::ElectricGrid,
        sim::PresetId::ZeroGOrb
    };
    const sim::ViewMode views[] = {
        sim::ViewMode::Physical,
        sim::ViewMode::Thermal,
        sim::ViewMode::Pressure,
        sim::ViewMode::Velocity,
        sim::ViewMode::EnergyLight
    };

    for (auto p : presets) {
        sim::SimWorld world(160, 90, 2);
        world.loadPreset(p);
        for (int t = 0; t < 90; ++t) {
            world.step();
        }
        const auto& st = world.stats();
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
            const uint32_t* fb = world.framebuffer();
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
    for (const auto& sz : sizes) {
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
                    if (m == sim::MaterialId::Lava || m == sim::MaterialId::Spark ||
                        m == sim::MaterialId::Fire || m == sim::MaterialId::Smoke ||
                        m == sim::MaterialId::Steam || m == sim::MaterialId::Obsidian) {
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
    for (int x = 14; x <= 34; ++x) quench.setCell(x, 38, sim::MaterialId::Bedrock);
    for (int y = 26; y <= 38; ++y) {
        quench.setCell(14, y, sim::MaterialId::Bedrock);
        quench.setCell(34, y, sim::MaterialId::Bedrock);
    }
    for (int y = 32; y <= 37; ++y) {
        for (int x = 15; x <= 24; ++x) quench.setCell(x, y, sim::MaterialId::Lava);
        for (int x = 25; x <= 33; ++x) quench.setCell(x, y, sim::MaterialId::Water);
    }

    for (int t = 0; t < 40; ++t) quench.step();
    CHECK(quench.countMaterial(sim::MaterialId::Obsidian) >= 1);
    CHECK(quench.countMaterial(sim::MaterialId::Steam) + quench.countMaterial(sim::MaterialId::Water) >= 1);
    CHECK(std::isfinite(quench.stats().maxTempK) && quench.stats().maxTempK <= 8000.0f);
}

void testHydrostaticLevelingSandStabilityAndSaltBridge() {
    // Part A: Stepped Water pool levels smoothly without trapped air bubbles
    sim::SimWorld basin(64, 48, 1);
    for (int x = 8; x <= 55; ++x) basin.setCell(x, 42, sim::MaterialId::Glass);
    for (int y = 20; y <= 42; ++y) {
        basin.setCell(8, y, sim::MaterialId::Glass);
        basin.setCell(55, y, sim::MaterialId::Glass);
    }
    for (int y = 24; y <= 41; ++y) {
        for (int x = 9; x <= 28; ++x) basin.setCell(x, y, sim::MaterialId::Water);
    }
    for (int t = 0; t < 200; ++t) basin.step();

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
    for (int x = 10; x <= 38; ++x) pile.setCell(x, 40, sim::MaterialId::Stone);
    for (int y = 30; y <= 36; ++y) {
        for (int x = 20; x <= 28; ++x) pile.setCell(x, y, sim::MaterialId::Sand);
    }
    for (int t = 0; t < 100; ++t) pile.step();

    std::vector<uint8_t> sandSnap(static_cast<size_t>(pile.pixelCount()), 0);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            if (pile.inspect(x, y).material == sim::MaterialId::Sand) {
                CHECK(y < 40);
                sandSnap[static_cast<size_t>(y * 48 + x)] = 1;
            }
        }
    }
    for (int t = 0; t < 60; ++t) pile.step();
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
        for (int x = 8; x <= 20; ++x) saltCircuit.setCell(x, y, sim::MaterialId::Metal);
        for (int x = 21; x <= 25; ++x) saltCircuit.setCell(x, y, sim::MaterialId::SaltWater);
        for (int x = 26; x <= 42; ++x) saltCircuit.setCell(x, y, sim::MaterialId::Metal);
    }
    saltCircuit.setCell(43, 38, sim::MaterialId::Gunpowder);

    for (int t = 0; t < 110; ++t) saltCircuit.step();
    CHECK(saltCircuit.inspect(23, 38).charge > 0.25f);
    CHECK(saltCircuit.inspect(23, 38).tempK > sim::kAmbientTempK + 0.3f);
    CHECK(saltCircuit.inspect(35, 38).charge > 0.20f);
    CHECK(saltCircuit.countMaterial(sim::MaterialId::Gunpowder) == 0);
}

void testReactionsRichness() {
    // 1. Acid corrosion generates gas bubbles (Methane/Smoke) and consumes Metal
    sim::SimWorld acidW(48, 48, 1);
    for (int x = 18; x <= 28; ++x) acidW.setCell(x, 34, sim::MaterialId::Bedrock);
    for (int y = 26; y <= 34; ++y) {
        acidW.setCell(18, y, sim::MaterialId::Bedrock);
        acidW.setCell(28, y, sim::MaterialId::Bedrock);
    }
    for (int x = 19; x <= 27; ++x) acidW.setCell(x, 33, sim::MaterialId::Metal);
    for (int x = 19; x <= 27; ++x) acidW.setCell(x, 32, sim::MaterialId::Acid);
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
    for (int x = 14; x <= 34; ++x) garden.setCell(x, 40, sim::MaterialId::Glass);
    for (int y = 32; y <= 40; ++y) {
        garden.setCell(14, y, sim::MaterialId::Glass);
        garden.setCell(34, y, sim::MaterialId::Glass);
    }
    for (int y = 35; y <= 39; ++y) {
        for (int x = 15; x <= 33; ++x) garden.setCell(x, y, sim::MaterialId::Water);
    }
    garden.setCell(24, 34, sim::MaterialId::Plant);
    const uint32_t initWater = garden.countMaterial(sim::MaterialId::Water);
    for (int t = 0; t < 150; ++t) garden.step();
    CHECK(garden.countMaterial(sim::MaterialId::Plant) > 2);
    CHECK(garden.countMaterial(sim::MaterialId::Water) > initWater / 2);
}

void testAllPresetsMinSizeAndMultithreadedFullGridFinite() {
    const sim::PresetId presets[] = {
        sim::PresetId::Laboratory,
        sim::PresetId::Volcano,
        sim::PresetId::Hydrodynamics,
        sim::PresetId::Explosives,
        sim::PresetId::ElectricGrid,
        sim::PresetId::ZeroGOrb
    };
    const sim::ViewMode views[] = {
        sim::ViewMode::Physical,
        sim::ViewMode::Thermal,
        sim::ViewMode::Pressure,
        sim::ViewMode::Velocity,
        sim::ViewMode::EnergyLight
    };

    const struct { int w; int h; unsigned threads; int steps; } configs[] = {
        {64, 64, 1, 100},
        {160, 90, 4, 100}
    };

    for (const auto& cfg : configs) {
        for (auto p : presets) {
            sim::SimWorld world(cfg.w, cfg.h, cfg.threads);
            world.loadPreset(p);
            for (int t = 0; t < cfg.steps; ++t) {
                world.step();
            }

            const auto& st = world.stats();
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
                const uint32_t* fb = world.framebuffer();
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
    for (int t = 0; t < 10; ++t) world.step();

    // Solid obstacle silhouettes must remain clearly legible in Thermal, Pressure, and Velocity modes
    const sim::ViewMode diagViews[] = {
        sim::ViewMode::Thermal,
        sim::ViewMode::Pressure,
        sim::ViewMode::Velocity
    };
    for (auto vm : diagViews) {
        world.render(vm);
        const uint32_t* fb = world.framebuffer();
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

    std::cout << "\nRisultato test: " << gPassed << " passati, " << gFailed << " falliti.\n";
    return gFailed == 0 ? 0 : 1;
}
