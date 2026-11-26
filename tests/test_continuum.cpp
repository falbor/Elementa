#include "sim/ContinuumInterface.hpp"
#include "sim/ContinuumMomentum.hpp"
#include "sim/ContinuumPressure.hpp"
#include "sim/ContinuumTransport.hpp"
#include "sim/SimWorld.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

void interfaceGeometry() {
    for (double nx : {-1.0, -0.3, 0.0, 0.3, 1.0})
        for (double ny : {-1.0, -0.3, 0.0, 0.3, 1.0}) {
            if (nx == 0.0 && ny == 0.0) continue;
            for (int k = 0; k <= 100; ++k) {
                const double q = k / 100.0, offset = sim::planeOffset(nx, ny, q);
                require(std::abs(sim::planeFraction(nx, ny, offset) - q) < 1e-12,
                        "PLIC plane reconstruction preserves the specified volume");
                // Two disjoint half-cell strips partition the reconstructed
                // liquid exactly, including diagonal and reflected normals.
                const double left = sim::planeFraction(nx, ny, offset + nx * 0.25, 0.5, 1.0);
                const double right = sim::planeFraction(nx, ny, offset - nx * 0.25, 0.5, 1.0);
                require(std::abs(0.5 * (left + right) - q) < 1e-12,
                        "PLIC strips partition the cell without overlap or missing volume");
            }
        }
}

void fractionalAdvection() {
    for (const auto &material : sim::getAllMaterials()) {
        if (!sim::ContinuumTransport::isSpecies(material.id)) continue;
        sim::ContinuumTransport fluid(16, 12);
        std::vector<float> u(17 * 12, 0.2f), v(16 * 13, 0.0f);
        std::vector<uint8_t> solid(16 * 12, 0);
        fluid.set(material.id, 5, 5, 1.0f);
        fluid.advect(u, v, solid, 1.0f, true);
        require(std::abs(fluid.amount(material.id, 5, 5) - 0.8f) < 1e-6f, "Fractional source");
        require(std::abs(fluid.amount(material.id, 6, 5) - 0.2f) < 1e-6f, "Fractional destination");
        require(std::abs(fluid.total(material.id) - 1.0) < 1e-6, "Species mass conservation");
    }
}

void mixtureAndWalls() {
    sim::ContinuumTransport fluid(16, 12);
    std::vector<float> u(17 * 12, 2.0f), v(16 * 13, -1.3f);
    std::vector<uint8_t> solid(16 * 12, 0);
    for (int y = 0; y < 12; ++y)
        solid[y * 16 + 8] = 1;
    fluid.set(sim::MaterialId::Water, 5, 5, 0.6f);
    fluid.set(sim::MaterialId::Oil, 5, 5, 0.4f);
    fluid.set(sim::MaterialId::Smoke, 5, 5, 0.3f);
    for (int tick = 0; tick < 30; ++tick)
        fluid.advect(u, v, solid, 1.0f, true);
    for (auto id : {sim::MaterialId::Water, sim::MaterialId::Oil, sim::MaterialId::Smoke}) {
        const double expected = id == sim::MaterialId::Water ? 0.6 : id == sim::MaterialId::Oil ? 0.4 : 0.3;
        require(std::abs(fluid.total(id) - expected) < 1e-5, "Mixture mass at high Courant number");
        for (float amount : fluid.field(id))
            require(amount >= 0.0f && std::isfinite(amount), "Positivity");
        for (int y = 0; y < 12; ++y)
            for (int x = 8; x < 16; ++x)
                require(fluid.amount(id, x, y) == 0.0f, "No transport through thin wall");
    }
}

void openBoundaryAndValidation() {
    sim::ContinuumTransport fluid(4, 4);
    std::vector<float> u(5 * 4, 0.2f), v(4 * 5, 0.0f);
    std::vector<uint8_t> solid(16, 0);
    fluid.set(sim::MaterialId::Methane, 3, 2, 1.0f);
    fluid.advect(u, v, solid, 1.0f, false);
    require(std::abs(fluid.total(sim::MaterialId::Methane) - 0.8) < 1e-6, "Open boundary outflow");
    fluid.clear();
    fluid.advect(u, v, solid, 1.0f, false);
    require(fluid.total(sim::MaterialId::Methane) == 0.0, "No artificial boundary inflow");
    u[0] = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try {
        fluid.advect(u, v, solid, 1.0f, true);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "Reject invalid input before mutating fields");
}

void pressureProjection() {
    constexpr int W = 16, H = 12;
    sim::ContinuumPressure pressure(W, H);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<float> volume(W * H, 1.0f), density(W * H, 1.0f);
    std::vector<uint8_t> solid(W * H, 0);
    for (size_t i = 0; i < u.size(); ++i)
        u[i] = std::sin(static_cast<float>(i) * 0.7f);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = std::cos(static_cast<float>(i) * 0.3f);
    // Two isolated chambers test the Neumann nullspace in disconnected regions.
    for (int y = 0; y < H; ++y)
        solid[y * W + 8] = 1;
    const auto result = pressure.project(u, v, volume, density, solid, 0.5f, true, 500, 1e-6f);
    require(result.converged, "Pressure convergence in disconnected chambers");
    require(result.divergenceAfter < result.divergenceBefore * 1e-4, "Divergence reduction");
    for (int y = 0; y < H; ++y) {
        require(u[y * (W + 1)] == 0.0f && u[y * (W + 1) + W] == 0.0f, "Closed border flux");
        require(u[y * (W + 1) + 8] == 0.0f && u[y * (W + 1) + 9] == 0.0f, "Wall normal flux");
    }
}

void largePressureProjection() {
    constexpr int W = 64, H = 96;
    for (float contrast : {1.0f, 9.0f}) {
        sim::ContinuumPressure pressure(W, H);
        std::vector<float> u(H * (W + 1)), v((H + 1) * W), volume(W * H, 1.0f), density(W * H, 1.0f);
        std::vector<uint8_t> solid(W * H, 0);
        for (int y = 0; y < H; ++y) {
            solid[y * W + W / 2] = 1;
            for (int x = W / 2 + 1; x < W; ++x)
                density[y * W + x] = contrast;
        }
        for (size_t i = 0; i < u.size(); ++i)
            u[i] = 0.03f * std::sin(static_cast<float>(i) * 0.13f);
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = 0.02f * std::cos(static_cast<float>(i) * 0.17f);
        const auto result = pressure.project(u, v, volume, density, solid, 1.0f, true, 2000, 1e-6f);
        require(result.converged, "Large pressure solve converges in disconnected chambers");
        require(result.components == 2, "Pressure acceleration preserves separate connected components");
        if (contrast == 1.0f) require(result.coarseNodes > 0, "Large pressure regression exercises the coarse solver");
        require(result.divergenceAfter < result.divergenceBefore * 1e-4,
                "Pressure acceleration preserves accuracy across density contrasts");
    }
}

void parallelPressureIslands() {
    constexpr int W = 96, H = 96;
    sim::ThreadPool pool(4);
    sim::ContinuumPressure serial(W, H), parallel(W, H, &pool);
    std::vector<float> volume(W * H, 1.0f), density(W * H), sourceU((W + 1) * H), sourceV(W * (H + 1));
    std::vector<uint8_t> solid(W * H);
    for (int y = 0; y < H; ++y)
        solid[y * W + W / 2] = 1;
    for (size_t i = 0; i < sourceU.size(); ++i)
        sourceU[i] = 0.1f * std::sin(static_cast<float>(i) * 0.13f);
    for (size_t i = 0; i < sourceV.size(); ++i)
        sourceV[i] = 0.1f * std::cos(static_cast<float>(i) * 0.17f);
    for (int scenario = 0; scenario < 5; ++scenario) {
        const float contrast = scenario == 0 || scenario == 3 ? 1.0f : 9.0f;
        // Include atmosphere around both pools: missing neighbors must read
        // zero pressure, never the first unknown in a compact island buffer.
        for (int i = 0; i < W * H; ++i)
            volume[i] = scenario < 3 || (i / W >= 20 && i / W < H - 8 && i % W > 4 && i % W < W - 5)
                            ? 1.0f : 0.0f;
        std::vector<float> surface;
        if (scenario == 4) {
            for (int x = 5; x < W - 5; ++x) volume[20 * W + x] = 0.7f;
            surface.resize(W * H);
            sim::reconstructSurface(W, H, volume, solid, surface);
        }
        for (int i = 0; i < W * H; ++i)
            density[i] = scenario == 2 ? ((i / W / 8) % 2 ? contrast : 1.0f)
                                      : (i % W < W / 2 ? 1.0f : contrast);
        auto su = sourceU, sv = sourceV, pu = sourceU, pv = sourceV;
        const auto reference = serial.project(su, sv, volume, density, solid, 1.0f, true, 2000, 1e-6f, surface);
        const auto result = parallel.project(pu, pv, volume, density, solid, 1.0f, true, 2000, 1e-6f, surface);
        require(reference.converged && result.converged && result.parallelComponents == 2,
                "Independent pressure blocks converge in parallel");
        require(result.divergenceAfter < 1e-5, "Parallel pressure blocks preserve incompressibility");
        double error = 0.0;
        for (size_t i = 0; i < pu.size(); ++i)
            error = std::max(error, static_cast<double>(std::abs(pu[i] - su[i])));
        for (size_t i = 0; i < pv.size(); ++i)
            error = std::max(error, static_cast<double>(std::abs(pv[i] - sv[i])));
        std::cout << "Parallel pressure: density contrast=" << contrast << " maximum velocity difference=" << error
                  << '\n';
        require(error < 1e-4, "Parallel island solve matches global pressure projection");
        auto repeatU = sourceU, repeatV = sourceV;
        parallel.project(repeatU, repeatV, volume, density, solid, 1.0f, true, 2000, 1e-6f, surface);
        require(repeatU == pu && repeatV == pv, "Island scheduling produces reproducible face velocities");
    }
}

void subcellPressureBoundary() {
    constexpr int W = 16, H = 16;
    sim::ContinuumPressure pressure(W, H);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<float> volume(W * H), density(W * H, 1.0f), phi(W * H);
    std::vector<uint8_t> solid(W * H);
    for (float q : {0.0f, 0.1f, 0.4f, 0.6f, 0.9f, 1.0f}) {
        std::fill(u.begin(), u.end(), 0.0f);
        std::fill(v.begin(), v.end(), 0.24f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                volume[y * W + x] = y < 8 ? 0.0f : y == 8 ? q : 1.0f;
                phi[y * W + x] = 8.5f - q - y;
            }
        sim::reconstructSurface(W, H, volume, solid, phi);
        if (q > 0.0f && q < 1.0f)
            for (int y = 7; y <= 9; ++y)
                require(std::abs(phi[y * W + 8] - (8.5f - q - y)) < 1e-6f,
                        "Volume reconstruction locates a flat fractional surface exactly");
        const auto result = pressure.project(u, v, volume, density, solid, 1.0f, true, 500, 1e-6f, phi);
        require(result.converged && result.divergenceAfter < 1e-5, "Subcell pressure projection converges");
        require(std::abs(pressure.pressure()[12 * W + 8] - 0.24 * (12 - (8.5 - q))) < 1e-5,
                "Hydrostatic pressure follows actual fractional surface elevation");
    }
    double referenceFlow = 0.0;
    for (bool mirrored : {false, true}) {
        std::fill(u.begin(), u.end(), 0.0f);
        std::fill(v.begin(), v.end(), 0.24f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float q = (x < W / 2) != mirrored ? 0.9f : 0.1f;
                volume[y * W + x] = y < 8 ? 0.0f : y == 8 ? q : 1.0f;
                phi[y * W + x] = 8.5f - q - y;
            }
        pressure.project(u, v, volume, density, solid, 1.0f, true, 500, 1e-6f, phi);
        double flow = 0.0;
        for (int y = 9; y < H; ++y)
            flow += u[y * (W + 1) + W / 2];
        require((mirrored ? -flow : flow) > 0.01, "Fractional head drives flow toward lower water without air forces");
        if (!mirrored)
            referenceFlow = flow;
        else
            require(std::abs(flow + referenceFlow) < 1e-5, "Subcell pressure response respects reflection symmetry");
    }
}

void freeSurfaceAndVolume() {
    constexpr int W = 12, H = 16;
    sim::ContinuumPressure pressure(W, H);
    sim::ContinuumTransport fluid(W, H);
    std::vector<float> u((W + 1) * H, 0.0f), v(W * (H + 1), 0.24f);
    std::vector<float> volume(W * H, 0.0f), density(W * H, 1.0f);
    std::vector<uint8_t> solid(W * H, 0);
    for (int y = 8; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            volume[y * W + x] = 1.0f;
            density[y * W + x] = y < 12 ? 0.8f : 1.0f;
            fluid.set(sim::MaterialId::Water, x, y, 1.0f);
        }
    const auto result = pressure.project(u, v, volume, density, solid, 1.0f, true, 500, 1e-6f);
    require(result.converged && result.divergenceAfter < 1e-5, "Variable-density free-surface projection");
    require(pressure.pressure()[15 * W + 5] > pressure.pressure()[8 * W + 5], "Hydrostatic head");
    for (int y = 8; y <= H; ++y)
        for (int x = 0; x < W; ++x)
            require(std::abs(v[y * W + x]) < 1e-5f, "Resting pool equilibrium");
    fluid.advect(u, v, solid, 1.0f, true);
    require(std::abs(fluid.total(sim::MaterialId::Water) - 96.0) < 1e-4, "Projected transport conserves volume");
    for (float value : fluid.field(sim::MaterialId::Water))
        require(value >= 0.0f && value <= 1.00001f, "Resting liquid does not compress");
}

void integratedWorldTransport() {
    for (auto material : {sim::MaterialId::Water, sim::MaterialId::Oil, sim::MaterialId::Methane}) {
        sim::SimWorld world(24, 24, 1);
        world.setGravityMode(sim::GravityMode::ZeroG);
        world.setCell(12, 12, material);
        world.setCellVelocity(12, 12, 2.0f, 0.0f);
        world.enableContinuum();
        world.step();
        require(std::abs(world.fluidAmount(material) - 1.0) < 1e-5, "World fractional mass conservation");
        const float remaining = world.fluidAmountAt(material, 12, 12);
        require(remaining > 0.0f && remaining < 1.0f, "World transports a fraction rather than a whole pixel");
        world.setCell(12, 12, sim::MaterialId::Air);
        require(world.fluidAmountAt(material, 12, 12) == 0.0f, "Erase updates authoritative field");
        world.clear();
        require(world.fluidAmount(material) == 0.0, "Clear updates authoritative field");
        world.setCell(8, 8, material);
        require(world.fluidAmountAt(material, 8, 8) == 1.0f, "Paint updates authoritative field");
    }
}

void transportedThermodynamics() {
    sim::ContinuumTransport fluid(8, 8);
    std::vector<float> u(9 * 8, 0.2f), v(8 * 9, 0.0f);
    std::vector<uint8_t> solid(64, 0);
    fluid.setState(sim::MaterialId::Smoke, 3, 3, 0.4f, 500.0f, 10.0f);
    fluid.setState(sim::MaterialId::Water, 3, 3, 0.6f, 300.0f, 0.0f);
    const double initialEnergy =
        fluid.thermalEnergy(sim::MaterialId::Smoke) + fluid.thermalEnergy(sim::MaterialId::Water);
    fluid.advect(u, v, solid, 1.0f, true);
    const double energy = fluid.thermalEnergy(sim::MaterialId::Smoke) + fluid.thermalEnergy(sim::MaterialId::Water);
    require(std::abs(energy - initialEnergy) < 1e-4, "Transport conserves species thermal energy");
    require(std::abs(fluid.temperature(sim::MaterialId::Smoke, 4, 3) - 500.0f) < 1e-4f, "Heat follows smoke");
    require(std::abs(fluid.lifetime(sim::MaterialId::Smoke, 4, 3) - 10.0f) < 1e-4f, "Age follows smoke");
    fluid.age(sim::MaterialId::Smoke, 3.0f, true);
    require(std::abs(fluid.lifetime(sim::MaterialId::Smoke, 4, 3) - 7.0f) < 1e-4f, "No rejuvenation on advection");
    const double waterEnergy = fluid.thermalEnergy(sim::MaterialId::Water);
    const float fraction = fluid.amount(sim::MaterialId::Water, 3, 3);
    fluid.convert(sim::MaterialId::Water, sim::MaterialId::Steam, 3, 3);
    require(std::abs(fluid.amount(sim::MaterialId::Steam, 3, 3) - fraction) < 1e-6f,
            "Conversion preserves fractional amount");
    require(std::abs(fluid.thermalEnergy(sim::MaterialId::Water) + fluid.thermalEnergy(sim::MaterialId::Steam) -
                     waterEnergy) < 1e-4,
            "Conversion preserves energy with different heat capacities");
    require(std::abs(fluid.total(sim::MaterialId::Smoke) - 0.4) < 1e-6, "Conversion preserves other species");
    fluid.age(sim::MaterialId::Smoke, 10.0f, true);
    require(fluid.total(sim::MaterialId::Smoke) == 0.0, "Expired smoke leaves no residual concentration");
}

void integratedFractionalPhaseChangeAndRendering() {
    sim::SimWorld world(24, 24, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);
    world.setCell(12, 12, sim::MaterialId::Water);
    world.setCellVelocity(12, 12, 2.0f, 0.0f);
    world.enableContinuum();
    world.step();
    bool fractionalCoverage = false;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            const float amount = world.fluidAmountAt(sim::MaterialId::Water, x, y);
            const auto visual = world.fluidVisualAt(x, y);
            if (amount > 0.001f && amount < 0.999f) {
                require(std::abs(visual[3] - amount) < 1e-5f, "Rendering uses fractional coverage");
                require(visual[0] <= visual[3] && visual[1] <= visual[3] && visual[2] <= visual[3],
                        "Premultiplied field");
                fractionalCoverage = true;
            }
            if (amount > 0.0f) world.setCellTemp(x, y, 1000.0f);
        }
    }
    require(fractionalCoverage, "Fractional visual field exists in integrated world");
    world.render(sim::ViewMode::Physical);
    world.step();
    require(world.fluidAmount(sim::MaterialId::Steam) > 0.1, "Fractional water boils in real reaction pass");
    require(std::abs(world.fluidAmount(sim::MaterialId::Steam) + world.fluidAmount(sim::MaterialId::Water) - 1.0) <
                1e-5,
            "Phase change never recreates whole fluid cells");
}

void momentumAndViscosity() {
    constexpr int W = 16, H = 12;
    sim::ContinuumMomentum momentum(W, H);
    std::vector<float> u((W + 1) * H, 0.2f), v(W * (H + 1), -0.1f);
    std::vector<float> nu(W * H, 0.0f);
    std::vector<uint8_t> solid(W * H, 0);
    momentum.advance(u, v, nu, solid, 1.0f, false);
    for (float speed : u)
        require(std::abs(speed - 0.2f) < 1e-6f, "Uniform momentum translates unchanged");
    for (float speed : v)
        require(std::abs(speed + 0.1f) < 1e-6f, "Uniform transverse momentum");
    for (int y = 0; y < H; ++y)
        for (int x = 0; x <= W; ++x)
            u[y * (W + 1) + x] = (y & 1) ? 0.1f : -0.1f;
    std::fill(v.begin(), v.end(), 0.0f);
    auto thinU = u, thickU = u, thinV = v, thickV = v;
    std::fill(nu.begin(), nu.end(), 0.008f);
    momentum.advance(thinU, thinV, nu, solid, 1.0f, false);
    std::fill(nu.begin(), nu.end(), 0.22f);
    momentum.advance(thickU, thickV, nu, solid, 1.0f, false);
    double thinEnergy = 0.0, thickEnergy = 0.0;
    for (size_t i = 0; i < u.size(); ++i) {
        thinEnergy += thinU[i] * thinU[i];
        thickEnergy += thickU[i] * thickU[i];
        require(std::isfinite(thickU[i]) && std::abs(thickU[i]) <= 0.10001f, "Implicit viscosity remains bounded");
    }
    require(thickEnergy < thinEnergy * 0.5, "Viscous liquid damps shear more strongly");
    for (int y = 0; y < H; ++y)
        solid[y * W + 8] = 1;
    momentum.advance(u, v, nu, solid, 1.0f, true);
    for (int y = 0; y < H; ++y)
        require(u[y * (W + 1) + 8] == 0.0f && u[y * (W + 1) + 9] == 0.0f, "Momentum respects wall faces");
}

void momentumBackendFallback() {
    constexpr int W = 16, H = 12;
    sim::ContinuumMomentum reference(W, H), accelerated(W, H);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<float> nu(W * H, 0.17f);
    std::vector<uint8_t> solid(W * H, 0);
    for (size_t i = 0; i < u.size(); ++i) u[i] = 0.3f * std::sin(float(i));
    for (size_t i = 0; i < v.size(); ++i) v[i] = 0.2f * std::cos(float(i));
    for (int y = 3; y < 9; ++y) solid[y * W + 8] = 1;
    auto expectedU = u, expectedV = v;
    int attempts = 0;
    accelerated.setBackend([&](auto, auto, auto, auto, float, bool) {
        ++attempts;
        return false;
    });
    for (int step = 0; step < 4; ++step) {
        reference.advance(expectedU, expectedV, nu, solid, 0.7f, true);
        accelerated.advance(u, v, nu, solid, 0.7f, true);
        require(u == expectedU && v == expectedV, "Unavailable accelerator preserves CPU results");
    }
    require(attempts == 4, "Momentum attempts the configured backend");
    bool rejected = false;
    try { accelerated.advance(u, v, nu, solid, -1.0f, true); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected && attempts == 4, "Invalid momentum input is rejected before acceleration");
    accelerated.setBackend({});
    accelerated.advance(u, v, nu, solid, 0.7f, true);
    reference.advance(expectedU, expectedV, nu, solid, 0.7f, true);
    require(attempts == 4 && u == expectedU && v == expectedV, "Backend detachment restores CPU operation");
}

void movingFreeSurface() {
    sim::SimWorld world(32, 32, 1);
    for (int y = 4; y <= 28; ++y) {
        world.setCell(4, y, sim::MaterialId::Bedrock);
        world.setCell(28, y, sim::MaterialId::Bedrock);
    }
    for (int x = 4; x <= 28; ++x)
        world.setCell(x, 28, sim::MaterialId::Bedrock);
    for (int y = 15; y < 28; ++y)
        for (int x = 5; x <= 12; ++x)
            world.setCell(x, y, sim::MaterialId::Water);
    world.enableContinuum();
    const double initial = world.fluidAmount(sim::MaterialId::Water);
    for (int tick = 0; tick < 80; ++tick)
        world.step();
    double rightAmount = 0.0;
    float maxVolume = 0.0f;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float amount = world.fluidAmountAt(sim::MaterialId::Water, x, y);
            require(std::isfinite(amount) && amount >= 0.0f, "Moving surface stays finite and positive");
            if (x > 16) rightAmount += amount;
            maxVolume = std::max(maxVolume, amount);
            if (x <= 4 || x >= 28 || y >= 28) require(amount < 1e-6f, "Moving pool stays inside basin");
        }
    std::cout << "Moving pool: mass=" << world.fluidAmount(sim::MaterialId::Water) << " right=" << rightAmount
              << " max volume=" << maxVolume << '\n';
    require(std::abs(world.fluidAmount(sim::MaterialId::Water) - initial) < 0.002, "Moving pool conserves volume");
    require(rightAmount > 10.0, "Dam break spreads as a continuous liquid");
    require(maxVolume < 1.01f, "Moving free surface does not compress liquid");
}

void fractionalSurfaceLevels() {
    // The two halves have the same wet-cell topology but different actual
    // surface heights. A pressure solve based only on wet/dry flags cannot
    // detect this head difference, despite reporting a tiny divergence.
    for (bool reversed : {false, true}) {
        sim::SimWorld world(32, 32, 1);
        for (int y = 1; y <= 20; ++y) {
            world.setCell(4, y, sim::MaterialId::Bedrock);
            world.setCell(27, y, sim::MaterialId::Bedrock);
        }
        for (int x = 4; x <= 27; ++x)
            world.setCell(x, 20, sim::MaterialId::Bedrock);
        for (int x = 5; x < 27; ++x) {
            for (int y = 17; y < 20; ++y)
                world.setCell(x, y, sim::MaterialId::Water);
            world.setFluidStateAt(sim::MaterialId::Water, x, 16, (x < 16) != reversed ? 0.9f : 0.1f,
                                  sim::kAmbientTempK);
        }
        const double initial = world.fluidAmount(sim::MaterialId::Water);
        for (int tick = 0; tick < 600; ++tick)
            world.step();
        double left = 0.0, right = 0.0;
        for (int y = 0; y < 32; ++y)
            for (int x = 5; x < 27; ++x)
                (x < 16 ? left : right) += world.fluidAmountAt(sim::MaterialId::Water, x, y);
        const double difference = std::abs(left - right) / 11.0;
        std::cout << "Fractional surface: initial head difference=0.8 final=" << difference
                  << " mass drift=" << world.fluidAmount(sim::MaterialId::Water) - initial << std::endl;
        require(std::abs(world.fluidAmount(sim::MaterialId::Water) - initial) < 0.005,
                "Fractional surface settling conserves volume");
        require(difference < 0.1, "Subcell surface heights equalize under gravity");
    }
}

void liquidSettlesAsCompactPool() {
    for (auto gravity : {sim::GravityMode::Earth, sim::GravityMode::Inverted}) {
        sim::SimWorld world(32, 40, 1);
        world.setGravityMode(gravity);
        for (int y = 1; y <= 36; ++y) {
            world.setCell(4, y, sim::MaterialId::Bedrock);
            world.setCell(28, y, sim::MaterialId::Bedrock);
        }
        for (int x = 4; x <= 28; ++x) {
            world.setCell(x, 36, sim::MaterialId::Bedrock);
            world.setCell(x, 3, sim::MaterialId::Bedrock);
        }
        for (int y = 5; y < 13; ++y)
            for (int x = 12; x < 20; ++x)
                world.setCell(x, gravity == sim::GravityMode::Earth ? y : 39 - y, sim::MaterialId::Water);
        world.enableContinuum();
        for (int tick = 0; tick < 240; ++tick)
            world.step();
        double bottom = 0.0, compact = 0.0, moment = 0.0, coreDeficit = 0.0;
        float minHeight = 64.0f, maxHeight = 0.0f;
        for (int x = 5; x < 28; ++x) {
            float height = 0.0f;
            for (int y = 0; y < 36; ++y)
                height += world.fluidAmountAt(sim::MaterialId::Water, x, y);
            minHeight = std::min(minHeight, height);
            maxHeight = std::max(maxHeight, height);
        }
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 32; ++x) {
                const float q = world.fluidAmountAt(sim::MaterialId::Water, x, y);
                const int height = gravity == sim::GravityMode::Earth ? y : 39 - y;
                if (height >= 32) bottom += q;
                if (x >= 5 && x < 28 && height >= 34 && height < 36)
                    coreDeficit += std::max(0.0f, 1.0f - q);
                if (q > 0.9f) compact += q;
                moment += height * q;
            }
        std::cout << "Settled pool: mass=" << world.fluidAmount(sim::MaterialId::Water) << " bottom=" << bottom
                  << " compact=" << compact << " centroid=" << moment / 64.0 << " height range=" << minHeight << ','
                  << maxHeight << " core deficit=" << coreDeficit << '\n';
        require(std::abs(world.fluidAmount(sim::MaterialId::Water) - 64.0) < 0.003, "Falling liquid conserves volume");
        require(bottom > 60.0 && moment / 64.0 > 33.0, "Liquid falls and collects on the basin floor");
        // 64 units across 23 columns form two full layers (46 units) and
        // a 0.7826-cell surface layer. Counting only q>0.9 incorrectly rejects
        // that exact flat equilibrium. Check the full interior layers instead.
        require(coreDeficit < 0.1, "Settled liquid fills the two interior layers without becoming a dilute cloud");
        require(maxHeight - minHeight < 1.1f, "Settled liquid spreads across the floor to a common level");
    }
}

void liquidInterfaceTransport() {
    constexpr int W = 32, H = 20;
    sim::ContinuumTransport liquid(W, H);
    std::vector<float> u(H * (W + 1), 0.2f), v((H + 1) * W, 0.0f);
    std::vector<uint8_t> solid(W * H, 0);
    for (int y = 5; y < 13; ++y)
        for (int x = 6; x < 12; ++x) {
            liquid.setState(sim::MaterialId::Water, x, y, 0.75f, 300.0f, 10.0f);
            liquid.setState(sim::MaterialId::Oil, x, y, 0.25f, 320.0f, 20.0f);
        }
    const double heat = liquid.thermalEnergy(sim::MaterialId::Water) + liquid.thermalEnergy(sim::MaterialId::Oil);
    for (int tick = 0; tick < 40; ++tick)
        liquid.advectLiquids(u, v, solid, 1.0f, true);
    double compact = 0.0, moment = 0.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float water = liquid.amount(sim::MaterialId::Water, x, y);
            const float oil = liquid.amount(sim::MaterialId::Oil, x, y), q = water + oil;
            require(q >= 0.0f && q <= 1.00001f, "Geometric liquid transport stays within cell volume");
            require(std::abs(water - 3.0f * oil) < 1e-5f, "Liquid volume flux preserves mixture composition");
            if (q > 0.99f) compact += q;
            moment += x * q;
            if (water > 1e-5f)
                require(std::abs(liquid.lifetime(sim::MaterialId::Water, x, y) - 10.0f) < 1e-4f,
                        "Geometric flux transports species age");
        }
    require(std::abs(liquid.total(sim::MaterialId::Water) - 36.0) < 0.001 &&
                std::abs(liquid.total(sim::MaterialId::Oil) - 12.0) < 0.001,
            "Geometric transport conserves each liquid");
    std::cout << "Translated liquid: compact=" << compact << " centroid=" << moment / 48.0 << std::endl;
    require(compact > 47.0 && std::abs(moment / 48.0 - 16.5) < 0.01,
            "Translated liquid retains its sharp body and follows face velocity");
    require(std::abs(liquid.thermalEnergy(sim::MaterialId::Water) + liquid.thermalEnergy(sim::MaterialId::Oil) - heat) <
                0.1,
            "Geometric liquid transport conserves thermal energy");

    // Two opposing streams must fill their shared receiver without compressing
    // it or discarding either mixture, including at a one-cell-thick wall.
    liquid.clear();
    std::fill(u.begin(), u.end(), 0.0f);
    liquid.set(sim::MaterialId::Water, 9, 9, 1.0f);
    liquid.set(sim::MaterialId::Oil, 11, 9, 1.0f);
    u[9 * (W + 1) + 10] = 3.0f;
    u[9 * (W + 1) + 11] = -3.0f;
    solid[9 * W + 12] = 1;
    liquid.advectLiquids(u, v, solid, 1.0f, true);
    require(liquid.amount(sim::MaterialId::Water, 10, 9) + liquid.amount(sim::MaterialId::Oil, 10, 9) <= 1.00001f,
            "Opposing liquid streams cannot overfill a receiving cell");
    require(std::abs(liquid.total(sim::MaterialId::Water) - 1.0) < 1e-6 &&
                std::abs(liquid.total(sim::MaterialId::Oil) - 1.0) < 1e-6,
            "Flux limiting never clips liquid mass");
    require(liquid.amount(sim::MaterialId::Oil, 12, 9) == 0.0f, "Geometric liquid transport respects walls");

    liquid.clear();
    std::fill(u.begin(), u.end(), 0.0f);
    liquid.set(sim::MaterialId::Water, 9, 9, 3.0f * std::numeric_limits<float>::denorm_min());
    liquid.set(sim::MaterialId::Oil, 9, 9, 0.25f);
    u[9 * (W + 1) + 9] = -0.3f;
    u[9 * (W + 1) + 10] = 0.3f;
    v[9 * W + 9] = -0.3f;
    v[10 * W + 9] = 0.3f;
    liquid.advectLiquids(u, v, solid, 1.0f, true);
    for (float q : liquid.field(sim::MaterialId::Water))
        require(q >= 0.0f, "Four outgoing faces cannot make a subnormal trace species negative");

    liquid.clear();
    std::fill(u.begin(), u.end(), 0.2f);
    std::fill(v.begin(), v.end(), 0.0f);
    liquid.set(sim::MaterialId::Water, W - 1, 5, 1.0f);
    liquid.advectLiquids(u, v, solid, 1.0f, false);
    require(std::abs(liquid.total(sim::MaterialId::Water) - 0.8) < 1e-6,
            "Geometric liquid transport allows open-boundary outflow");
}

void fractionalDensitySeparation() {
    for (float gravity : {0.24f, -0.24f}) {
        sim::ContinuumTransport fluid(2, 2);
        std::vector<float> gx(4, 0.0f), gy(4, gravity);
        std::vector<uint8_t> solid(4, 0);
        const int upper = gravity > 0.0f ? 0 : 1, lower = 1 - upper;
        fluid.setState(sim::MaterialId::Water, 0, upper, 1.0f, 310.0f, 0.0f);
        fluid.setState(sim::MaterialId::Oil, 0, lower, 1.0f, 290.0f, 0.0f);
        const double energy = fluid.thermalEnergy(sim::MaterialId::Water) + fluid.thermalEnergy(sim::MaterialId::Oil);
        fluid.separateLiquids(gx, gy, solid, 1.0f);
        require(fluid.amount(sim::MaterialId::Water, 0, lower) > 0.0f &&
                    fluid.amount(sim::MaterialId::Water, 0, lower) < 0.1f,
                "Density separation exchanges fractions, not entire cells");
        for (int t = 0; t < 200; ++t)
            fluid.separateLiquids(gx, gy, solid, 1.0f);
        require(fluid.amount(sim::MaterialId::Water, 0, lower) > 0.8f, "Heavy liquid moves with gravity");
        require(fluid.amount(sim::MaterialId::Oil, 0, upper) > 0.8f, "Light liquid floats against gravity");
        require(std::abs(fluid.total(sim::MaterialId::Water) - 1.0) < 1e-5 &&
                    std::abs(fluid.total(sim::MaterialId::Oil) - 1.0) < 1e-5,
                "Segregation conserves both species");
        for (int y = 0; y < 2; ++y)
            require(std::abs(fluid.amount(sim::MaterialId::Water, 0, y) + fluid.amount(sim::MaterialId::Oil, 0, y) -
                             1.0f) < 1e-5,
                    "Segregation preserves local liquid fill");
        require(std::abs(fluid.thermalEnergy(sim::MaterialId::Water) + fluid.thermalEnergy(sim::MaterialId::Oil) -
                         energy) < 0.01,
                "Segregation transports heat conservatively");
    }
}

void integratedThermalConservation() {
    sim::SimWorld world(24, 24, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);
    world.setCell(10, 12, sim::MaterialId::Water);
    world.setCellTemp(10, 12, 340.0f);
    world.setCellVelocity(10, 12, 2.0f, 0.5f);
    world.setCell(13, 12, sim::MaterialId::Oil);
    world.setCellTemp(13, 12, 310.0f);
    world.enableContinuum();
    const double energy = world.computeTotalThermalEnergy();
    for (int tick = 0; tick < 30; ++tick)
        world.step();
    const double after = world.computeTotalThermalEnergy();
    std::cout << "Fractional thermal energy drift=" << after - energy << '\n';
    require(std::abs(after - energy) < 0.05, "Conduction and transport conserve mixture plus background heat");
    require(std::abs(world.stats().totalThermalEnergy - after) < 1e-6, "Diagnostics use fractional heat capacities");
    require(std::abs(world.fluidAmount(sim::MaterialId::Water) - 1.0) < 1e-5 &&
                std::abs(world.fluidAmount(sim::MaterialId::Oil) - 1.0) < 1e-5,
            "Thermal coupling preserves fluid amounts");
}

void mixedReactionsAndFractionalSolids() {
    sim::SimWorld world(24, 24, 1);
    world.setGravityMode(sim::GravityMode::ZeroG);
    world.setFluidStateAt(sim::MaterialId::Water, 12, 12, 0.8f, 300.0f);
    world.setFluidStateAt(sim::MaterialId::MoltenMetal, 12, 12, 0.2f, 300.0f);
    require(world.inspect(12, 12).material == sim::MaterialId::Water, "Minor species is not the material proxy");
    world.step();
    require(std::abs(world.fluidAmount(sim::MaterialId::Metal) - 0.2) < 1e-5,
            "Non-dominant molten component solidifies fractionally");
    require(world.fluidAmount(sim::MaterialId::MoltenMetal) < 1e-6, "Minor reactant is consumed");
    require(std::abs(world.fluidAmount(sim::MaterialId::Water) - 0.8) < 1e-5, "Reaction preserves dominant liquid");

    world.clear();
    world.setFluidStateAt(sim::MaterialId::Oil, 12, 12, 0.2f, 1500.0f);
    world.step();
    require(world.fluidAmount(sim::MaterialId::Oil) < 1e-5, "Fractional fuel combusts");
    require(world.fluidAmount(sim::MaterialId::Fire) > 0.0 && world.fluidAmount(sim::MaterialId::Fire) < 1.0,
            "Combustion does not turn fractional fuel into a whole flame cell");
}

void allContinuousPresetsFinite() {
    for (auto preset : {sim::PresetId::Laboratory, sim::PresetId::Volcano, sim::PresetId::Hydrodynamics,
                        sim::PresetId::Explosives, sim::PresetId::ElectricGrid, sim::PresetId::ZeroGOrb}) {
        sim::SimWorld world(32, 32, 2);
        world.loadPreset(preset);
        world.enableContinuum();
        for (int t = 0; t < 12; ++t) {
            try {
                world.step();
            } catch (const std::exception &error) {
                throw std::runtime_error("Preset " + std::to_string(static_cast<int>(preset)) + " tick " +
                                         std::to_string(t) + ": " + error.what());
            }
        }
        world.render(sim::ViewMode::Physical);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const auto pixel = world.inspect(x, y);
                require(std::isfinite(pixel.tempK) && std::isfinite(pixel.vx) && std::isfinite(pixel.vy),
                        "Preset cell remains finite");
                const auto visual = world.fluidVisualAt(x, y);
                for (float component : visual)
                    require(std::isfinite(component) && component >= 0.0f && component <= 1.00001f,
                            "Preset continuous visual field remains bounded");
                for (const auto &material : sim::getAllMaterials()) {
                    const float amount = world.fluidAmountAt(material.id, x, y);
                    require(std::isfinite(amount) && amount >= 0.0f, "Preset species remains positive and finite");
                }
            }
    }
}

void liquidWorkspaceReuse() {
    constexpr int W = 40, H = 32;
    sim::ThreadPool pool(4);
    sim::ContinuumTransport reused(W, H, &pool);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<uint8_t> solid(W * H, 0);
    for (int scenario = 0; scenario < 8; ++scenario) {
        reused.clear();
        sim::ContinuumTransport fresh(W, H, &pool);
        const int left = scenario % 2 ? 25 : 3;
        for (int y = 4; y < 13; ++y)
            for (int x = left; x < left + 8; ++x)
                for (auto *fluid : {&reused, &fresh}) {
                    fluid->setState(sim::MaterialId::Water, x, y, 0.7f, 305.0f, 17.0f);
                    fluid->setState(sim::MaterialId::Oil, x, y, 0.3f, 320.0f, 9.0f);
                }
        std::fill(u.begin(), u.end(), scenario == 3 ? 0.0f : (scenario % 2 ? -1.2f : 0.8f));
        std::fill(v.begin(), v.end(), scenario == 3 ? 0.0f : 0.4f);
        for (int step = 0; step < 3; ++step) {
            reused.advectLiquids(u, v, solid, 1.0f, true);
            fresh.advectLiquids(u, v, solid, 1.0f, true);
            for (auto id : {sim::MaterialId::Water, sim::MaterialId::Oil}) {
                require(std::equal(reused.field(id).begin(), reused.field(id).end(), fresh.field(id).begin()),
                        "Moving liquid regions do not read stale workspace concentrations");
                require(std::equal(reused.heatField(id).begin(), reused.heatField(id).end(), fresh.heatField(id).begin()),
                        "Reused liquid workspace preserves thermal moments");
                require(std::equal(reused.ageField(id).begin(), reused.ageField(id).end(), fresh.ageField(id).begin()),
                        "Reused liquid workspace preserves age moments");
            }
        }
    }
}

void microscopicCapacityCorrection() {
    constexpr int W = 16, H = 12;
    sim::ContinuumTransport liquid(W, H);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<uint8_t> solid(W * H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            liquid.setState(sim::MaterialId::Water, x, y, 0.75f, 300.0f, 10.0f);
            liquid.setState(sim::MaterialId::Oil, x, y, 0.25f, 320.0f, 20.0f);
        }
    for (int y = 0; y < H; ++y)
        for (int x = 1; x < W; ++x) u[y * (W + 1) + x] = x % 4 < 2 ? 1.5e-5f : -1.5e-5f;
    for (int y = 1; y < H; ++y)
        for (int x = 0; x < W; ++x) v[y * W + x] = y % 4 < 2 ? 1.5e-5f : -1.5e-5f;
    // No proposed vacancy reaches 1e-4, but converging flows exceed the
    // capacity tolerance. The fine correction must use these tiny vacancies.
    liquid.advectLiquids(u, v, solid, 1.0f, true);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float water = liquid.amount(sim::MaterialId::Water, x, y);
            const float oil = liquid.amount(sim::MaterialId::Oil, x, y);
            require(water + oil <= 1.000004f, "Microscopic vacancies still resolve liquid overfill");
            require(std::abs(water - 3.0f * oil) < 1e-6f, "Fine capacity correction preserves mixture ratios");
        }
    require(std::abs(liquid.total(sim::MaterialId::Water) - W * H * 0.75) < 0.0001,
            "Fine capacity correction conserves water");
    require(std::abs(liquid.total(sim::MaterialId::Oil) - W * H * 0.25) < 0.0001,
            "Fine capacity correction conserves oil");
}

void boundedLiquidParallelStress() {
    constexpr int W = 64, H = 32;
    sim::ThreadPool pool(4);
    sim::ContinuumTransport serial(W, H), parallel(W, H, &pool);
    std::vector<float> u(H * (W + 1)), v((H + 1) * W);
    std::vector<uint8_t> solid(W * H, 0);
    for (int y = 0; y < H; ++y)
        solid[y * W + W / 2] = 1;
    for (size_t i = 0; i < u.size(); ++i)
        u[i] = 0.4f * std::sin(static_cast<float>(i) * 0.11f);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = 0.3f * std::cos(static_cast<float>(i) * 0.17f);
    for (auto *world : {&serial, &parallel}) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (!solid[y * W + x]) {
                    world->setState(sim::MaterialId::Water, x, y, 0.75f, 300.0f, 10.0f);
                    world->setState(sim::MaterialId::Oil, x, y, 0.25f, 320.0f, 20.0f);
                }
        const double water = world->total(sim::MaterialId::Water), oil = world->total(sim::MaterialId::Oil);
        const double heat = world->thermalEnergy(sim::MaterialId::Water) + world->thermalEnergy(sim::MaterialId::Oil);
        for (int tick = 0; tick < 20; ++tick)
            world->advectLiquids(u, v, solid, 1.0f, true);
        require(std::abs(world->total(sim::MaterialId::Water) - water) < 0.002 &&
                    std::abs(world->total(sim::MaterialId::Oil) - oil) < 0.002,
                "Capacity routing conserves every species in full tanks");
        require(std::abs(world->thermalEnergy(sim::MaterialId::Water) + world->thermalEnergy(sim::MaterialId::Oil) -
                         heat) < 2.0,
                "Capacity routing conserves mixture heat");
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float fill =
                    world->amount(sim::MaterialId::Water, x, y) + world->amount(sim::MaterialId::Oil, x, y);
                require(fill >= 0.0f && fill <= 1.00001f, "Arbitrary opposing currents remain bounded in full tanks");
                if (solid[y * W + x]) require(fill == 0.0f, "Capacity routing never crosses a one-cell wall");
            }
    }
    for (auto id : {sim::MaterialId::Water, sim::MaterialId::Oil})
        for (int i = 0; i < W * H; ++i) {
            require(serial.field(id)[i] == parallel.field(id)[i], "Parallel liquid amount matches serial routing");
            require(serial.heatField(id)[i] == parallel.heatField(id)[i],
                    "Parallel liquid heat matches serial routing");
            require(serial.ageField(id)[i] == parallel.ageField(id)[i], "Parallel liquid age matches serial routing");
        }
}

void quiescentGasDispersion() {
    sim::ContinuumTransport fluid(8, 8);
    std::vector<uint8_t> solid(64, 0);
    fluid.setState(sim::MaterialId::Smoke, 3, 3, 1.0f, 500.0f, 20.0f);
    const double heat = fluid.thermalEnergy(sim::MaterialId::Smoke);
    fluid.diffuseGases(solid, 1.0f);
    require(std::abs(fluid.amount(sim::MaterialId::Smoke, 3, 3) - 0.9f) < 1e-6f,
            "Stationary gas disperses continuously");
    require(std::abs(fluid.amount(sim::MaterialId::Smoke, 4, 3) - 0.025f) < 1e-6f, "Gas diffusion is fractional");
    require(std::abs(fluid.total(sim::MaterialId::Smoke) - 1.0) < 1e-6, "Diffusion conserves gas amount");
    require(std::abs(fluid.thermalEnergy(sim::MaterialId::Smoke) - heat) < 1e-4, "Diffusion conserves heat");
    require(std::abs(fluid.lifetime(sim::MaterialId::Smoke, 4, 3) - 20.0f) < 1e-5f, "Diffusion carries age");
}

void parallelTransportConsistency() {
    constexpr int W = 64, H = 32;
    sim::ThreadPool pool(4);
    sim::ContinuumTransport serial(W, H), parallel(W, H, &pool);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<uint8_t> solid(W * H, 0);
    for (size_t i = 0; i < u.size(); ++i)
        u[i] = 0.4f * std::sin(static_cast<float>(i) * 0.11f);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = 0.3f * std::cos(static_cast<float>(i) * 0.17f);
    for (auto *world : {&serial, &parallel}) {
        world->setState(sim::MaterialId::Smoke, 20, 15, 1.0f, 500.0f, 50.0f);
        world->setState(sim::MaterialId::Water, 30, 18, 1.0f, 300.0f, 0.0f);
        for (int t = 0; t < 20; ++t)
            world->advect(u, v, solid, 1.0f, true);
    }
    for (auto id : {sim::MaterialId::Smoke, sim::MaterialId::Water}) {
        for (size_t i = 0; i < solid.size(); ++i) {
            require(std::abs(serial.field(id)[i] - parallel.field(id)[i]) < 1e-6f,
                    "Parallel amount matches serial transport");
            require(std::abs(serial.heatField(id)[i] - parallel.heatField(id)[i]) < 1e-5f,
                    "Parallel heat matches serial transport");
            require(std::abs(serial.ageField(id)[i] - parallel.ageField(id)[i]) < 1e-6f,
                    "Parallel age matches serial transport");
        }
    }
}

void thermalSmokePlume() {
    sim::SimWorld hot(48, 64, 1), cold(48, 64, 1);
    for (auto *world : {&hot, &cold}) {
        for (int y = 40; y < 44; ++y)
            for (int x = 20; x < 28; ++x) {
                world->setCell(x, y, sim::MaterialId::Smoke);
                world->setCellTemp(x, y, world == &hot ? 450.0f : sim::kAmbientTempK);
            }
        world->enableContinuum();
        for (int t = 0; t < 80; ++t)
            world->step();
        require(std::abs(world->fluidAmount(sim::MaterialId::Smoke) - 32.0) < 0.001,
                "Plume transport conserves smoke before expiry");
    }
    auto centroid = [](const sim::SimWorld &world) {
        double sum = 0.0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 48; ++x)
                sum += y * world.fluidAmountAt(sim::MaterialId::Smoke, x, y);
        return sum / world.fluidAmount(sim::MaterialId::Smoke);
    };
    std::cout << "Smoke centroids: hot=" << centroid(hot) << " cold=" << centroid(cold) << '\n';
    require(centroid(hot) < centroid(cold) - 0.5, "Smoke rises through thermal convection, not whole-pixel hopping");
}

void vortexTransport() {
    constexpr int W = 32, H = 32;
    sim::ContinuumTransport fluid(W, H);
    std::vector<float> u((W + 1) * H), v(W * (H + 1));
    std::vector<uint8_t> solid(W * H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x <= W; ++x)
            u[y * (W + 1) + x] = -0.05f * (y - 16.0f);
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x < W; ++x)
            v[y * W + x] = 0.05f * (x - 16.0f);
    fluid.set(sim::MaterialId::Smoke, 20, 16, 1.0f);
    for (int t = 0; t < 20; ++t)
        fluid.advect(u, v, solid, 1.0f, true);
    double cx = 0.0, cy = 0.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = fluid.amount(sim::MaterialId::Smoke, x, y);
            cx += x * q;
            cy += y * q;
        }
    require(std::abs(fluid.total(sim::MaterialId::Smoke) - 1.0) < 1e-5, "Vortex conserves smoke amount");
    require(std::abs(cx - (16.0 + 4.0 * std::cos(1.0))) < 0.3 && std::abs(cy - (16.0 + 4.0 * std::sin(1.0))) < 0.3,
            "Concentration follows the rotating wind field");
}
void rotatingLiquidAccuracy() {
    double coarseError = 0.0;
    for (int W : {64, 128}) {
        const int H = W;
        sim::ContinuumTransport liquid(W, H);
        std::vector<float> u((W + 1) * H), v(W * (H + 1)), initial(W * H);
        std::vector<uint8_t> solid(W * H, 0);
        const double center = (W - 1) * 0.5, cx = center + 0.18 * W, cy = center, radius = 0.12 * W;
        constexpr float omega = 0.02f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x <= W; ++x) u[y * (W + 1) + x] = -omega * (y - center);
        for (int y = 0; y <= H; ++y)
            for (int x = 0; x < W; ++x) v[y * W + x] = omega * (x - center);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int inside = 0;
                for (int sy = 0; sy < 16; ++sy)
                    for (int sx = 0; sx < 16; ++sx) {
                        const double dx = x - 0.5 + (sx + 0.5) / 16.0 - cx;
                        const double dy = y - 0.5 + (sy + 0.5) / 16.0 - cy;
                        inside += dx * dx + dy * dy <= radius * radius;
                    }
                initial[y * W + x] = inside / 256.0f;
                liquid.set(sim::MaterialId::Water, x, y, initial[y * W + x]);
            }
        const double mass = liquid.total(sim::MaterialId::Water);
        const double period = 2.0 * std::acos(-1.0) / omega;
        const int steps = static_cast<int>(std::ceil(period));
        for (int step = 0; step < steps; ++step) {
            liquid.advectLiquids(u, v, solid, static_cast<float>(period / steps), true);
            // A stationary disk would also match its initial shape after one
            // period. Check its path as well, including rotation direction.
            if (step + 1 == steps / 4 || step + 1 == steps / 2) {
                double xMoment = 0.0, yMoment = 0.0;
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        const float q = liquid.amount(sim::MaterialId::Water, x, y);
                        xMoment += x * q;
                        yMoment += y * q;
                    }
                const double angle = omega * period * (step + 1) / steps;
                const double expectedX = center + (cx - center) * std::cos(angle);
                const double expectedY = center + (cx - center) * std::sin(angle);
                const double currentMass = liquid.total(sim::MaterialId::Water);
                require(std::hypot(xMoment / currentMass - expectedX, yMoment / currentMass - expectedY) < W * 0.01,
                        "Liquid follows the analytic rotating trajectory");
            }
        }
        double error = 0.0, momentX = 0.0, momentY = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float q = liquid.amount(sim::MaterialId::Water, x, y);
                require(std::isfinite(q) && q >= 0.0f && q < 1.0001f, "Rotating liquid remains bounded");
                error += std::abs(q - initial[y * W + x]);
                momentX += x * q;
                momentY += y * q;
            }
        const double finalMass = liquid.total(sim::MaterialId::Water);
        const double relativeError = error / mass;
        std::cout << "Rotating liquid W=" << W << " relative L1=" << error / mass
                  << " centroid error=" << std::hypot(momentX / finalMass - cx, momentY / finalMass - cy)
                  << " mass drift=" << finalMass - mass << std::endl;
        require(std::abs(finalMass - mass) < mass * 1e-5, "Full rotation conserves liquid volume");
        require(relativeError < (W == 64 ? 0.02 : 0.01), "Full rotation preserves the liquid interface");
        require(std::hypot(momentX / finalMass - cx, momentY / finalMass - cy) < W * 0.002,
                "Full rotation returns the liquid centroid");
        if (W == 64) coarseError = relativeError;
        else require(relativeError < coarseError * 0.75, "Rotating interface improves under grid refinement");
    }
}

void normalResolutionPresets(int ticks = 60) {
    for (auto preset : {sim::PresetId::Laboratory, sim::PresetId::Volcano, sim::PresetId::Hydrodynamics,
                        sim::PresetId::Explosives, sim::PresetId::ElectricGrid, sim::PresetId::ZeroGOrb}) {
        sim::SimWorld world(480, 270, 0);
        world.loadPreset(preset);
        world.enableContinuum();
        int unconverged = 0, maxIterations = 0;
        double maxDivergence = 0.0;
        for (int t = 0; t < ticks; ++t) {
            world.step();
            if (!world.stats().continuumPressureConverged || world.stats().continuumDivergence >= 1e-4) {
                std::cerr << "Pressure failure: preset=" << static_cast<int>(preset) << " tick=" << t
                          << " iterations=" << world.stats().continuumPressureIterations
                          << " divergence=" << world.stats().continuumDivergence << '\n';
                throw std::runtime_error("Normal-resolution pressure failure");
            }
            unconverged += !world.stats().continuumPressureConverged;
            maxIterations = std::max(maxIterations, world.stats().continuumPressureIterations);
            maxDivergence = std::max(maxDivergence, world.stats().continuumDivergence);
            for (int y = 0; y < 270; ++y)
                for (int x = 0; x < 480; ++x) {
                    float fill = 0.0f;
                    for (const auto &material : sim::getAllMaterials())
                        if (material.phase == sim::PhaseState::Liquid) fill += world.fluidAmountAt(material.id, x, y);
                    if (fill > 1.02f) {
                        std::cerr << "First overfill: preset=" << static_cast<int>(preset) << " tick=" << t
                                  << " cell=" << x << ',' << y << " fill=" << fill
                                  << " temperature=" << world.inspect(x, y).tempK
                                  << " pressure converged=" << world.stats().continuumPressureConverged
                                  << " iterations=" << world.stats().continuumPressureIterations
                                  << " divergence=" << world.stats().continuumDivergence << '\n';
                        for (const auto &material : sim::getAllMaterials()) {
                            const float q = world.fluidAmountAt(material.id, x, y);
                            if (q > 0.001f) std::cerr << material.shortLabel << '=' << q << ' ';
                        }
                        std::cerr << '\n';
                        throw std::runtime_error("Normal-resolution liquid overfill");
                    }
                }
            if (ticks > 60 && (t + 1) % 60 == 0)
                std::cout << "Long-run preset=" << static_cast<int>(preset) << " tick=" << t + 1
                          << " max divergence=" << maxDivergence << std::endl;
        }
        float maxLiquid = 0.0f;
        for (int y = 0; y < 270; ++y)
            for (int x = 0; x < 480; ++x) {
                const auto pixel = world.inspect(x, y);
                require(std::isfinite(pixel.tempK) && pixel.tempK > 0.0f && std::isfinite(pixel.vx) &&
                            std::isfinite(pixel.vy),
                        "Normal-resolution preset state remains finite");
                float fill = 0.0f;
                for (const auto &material : sim::getAllMaterials()) {
                    const float q = world.fluidAmountAt(material.id, x, y);
                    require(std::isfinite(q) && q >= 0.0f, "Normal-resolution concentrations remain positive");
                    if (material.phase == sim::PhaseState::Liquid) fill += q;
                }
                maxLiquid = std::max(maxLiquid, fill);
            }
        std::cout << "480x270 preset " << static_cast<int>(preset) << " max liquid fill=" << maxLiquid
                  << " unconverged=" << unconverged << " max iterations=" << maxIterations
                  << " max divergence=" << maxDivergence << std::endl;
        require(unconverged == 0, "Every normal-resolution pressure solve converges");
        require(maxDivergence < 1e-4, "Normal-resolution projection satisfies incompressibility");
        require(maxLiquid < 1.02f, "Normal-resolution liquid does not compress beyond numerical tolerance");
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--normal-resolution") {
            normalResolutionPresets();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--long-run") {
            normalResolutionPresets(600);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--rotation-audit") {
            rotatingLiquidAccuracy();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--accuracy-audit") {
            fractionalSurfaceLevels();
            return 0;
        }
        fractionalAdvection();
        interfaceGeometry();
        mixtureAndWalls();
        openBoundaryAndValidation();
        pressureProjection();
        largePressureProjection();
        parallelPressureIslands();
        subcellPressureBoundary();
        freeSurfaceAndVolume();
        integratedWorldTransport();
        transportedThermodynamics();
        integratedFractionalPhaseChangeAndRendering();
        momentumAndViscosity();
        momentumBackendFallback();
        movingFreeSurface();
        liquidSettlesAsCompactPool();
        liquidInterfaceTransport();
        fractionalDensitySeparation();
        integratedThermalConservation();
        mixedReactionsAndFractionalSolids();
        allContinuousPresetsFinite();
        quiescentGasDispersion();
        parallelTransportConsistency();
        boundedLiquidParallelStress();
        microscopicCapacityCorrection();
        liquidWorkspaceReuse();
        thermalSmokePlume();
        vortexTransport();
        std::cout << "Continuum: fractional advection, mixtures, mass, walls and boundaries passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
