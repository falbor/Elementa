#include "sim/SimWorld.hpp"
#include "sim/ContinuumInterface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace sim {

namespace {

float liquidViscosity(MaterialId material) {
    switch (material) {
    case MaterialId::Oil:
        return 0.12f;
    case MaterialId::Lava:
        return 0.18f;
    case MaterialId::MoltenGlass:
        return 0.22f;
    case MaterialId::MoltenMetal:
        return 0.03f;
    case MaterialId::Nitro:
        return 0.04f;
    case MaterialId::Acid:
        return 0.012f;
    case MaterialId::SaltWater:
        return 0.010f;
    default:
        return 0.008f;
    }
}
} // namespace

void SimWorld::enableContinuum() {
    if (m_continuum) return;
    m_continuum = std::make_unique<ContinuumTransport>(m_width, m_height, &m_pool);
    m_continuumPressure = std::make_unique<ContinuumPressure>(m_width, m_height, &m_pool);
    m_continuumMomentum = std::make_unique<ContinuumMomentum>(m_width, m_height, &m_pool);
    m_contViscosity.resize(m_count);
    m_contGravityX.resize(m_count);
    m_contGravityY.resize(m_count);
    m_contCapacity.resize(m_count);
    m_contConductivity.resize(m_count);
    m_contU.assign(static_cast<size_t>(m_width + 1) * m_height, 0.0f);
    m_contV.assign(static_cast<size_t>(m_height + 1) * m_width, 0.0f);
    m_contVolume.resize(m_count);
    m_contDensity.resize(m_count);
    m_contSurface.resize(m_count);
    for (int i = 0; i < m_count; ++i)
        resetContinuumCell(i, fromIdx(m_mat[i]));
    auto liquidVelocity = [&](int x, int y, bool vertical) {
        const int i = std::clamp(y, 0, m_height - 1) * m_width + std::clamp(x, 0, m_width - 1);
        if (getMaterial(fromIdx(m_mat[i])).phase != PhaseState::Liquid) return 0.0f;
        return vertical ? m_vy[i] : m_vx[i];
    };
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x <= m_width; ++x)
            m_contU[y * (m_width + 1) + x] = 0.5f * (liquidVelocity(x - 1, y, false) + liquidVelocity(x, y, false));
    for (int y = 0; y <= m_height; ++y)
        for (int x = 0; x < m_width; ++x)
            m_contV[y * m_width + x] = 0.5f * (liquidVelocity(x, y - 1, true) + liquidVelocity(x, y, true));
}

void SimWorld::resetContinuumCell(int i, MaterialId material) {
    m_continuum->clearCell(i % m_width, i / m_width);
    m_continuum->setState(material, i % m_width, i / m_width, 1.0f, m_tempK[i], std::max(0.0f, m_life[i]));
}

void SimWorld::refreshContinuumProxy(int i, const std::array<const float *, kMaterialCount> *amounts) {
    float maximum = 0.02f;
    MaterialId dominant = MaterialId::Air;
    for (const auto &material : getAllMaterials()) {
        if (material.id == MaterialId::Air) continue;
        const float amount =
            amounts ? (*amounts)[static_cast<size_t>(material.id)][i] : m_continuum->field(material.id)[i];
        if (amount > maximum) {
            maximum = amount;
            dominant = material.id;
        }
    }
    m_mat[i] = toIdx(dominant);
    if (getMaterial(dominant).phase != PhaseState::StaticSolid)
        m_life[i] = m_continuum->lifetime(dominant, i % m_width, i / m_width);
}

void SimWorld::setFluidStateAt(MaterialId species, int x, int y, float amount, float temperature) {
    if (!ContinuumTransport::isSpecies(species) || !std::isfinite(amount) || amount < 0.0f ||
        !std::isfinite(temperature) || temperature < 1.0f || temperature > 8000.0f)
        throw std::invalid_argument("Invalid fractional fluid state");
    if (!inBounds(x, y)) return;
    enableContinuum();
    const float previous = m_continuum->amount(species, x, y);
    const float air = m_continuum->amount(MaterialId::Air, x, y);
    m_continuum->setState(MaterialId::Air, x, y, std::max(0.0f, air + previous - amount), m_tempK[index(x, y)], 0.0f);
    m_continuum->setState(species, x, y, amount, temperature,
                          species == MaterialId::Smoke || species == MaterialId::Steam ? 350.0f
                          : species == MaterialId::Acid                                ? 100.0f
                                                                                       : 0.0f);
    double energy = 0.0, capacity = 0.0;
    for (const auto &material : getAllMaterials()) {
        const double c = m_continuum->amount(material.id, x, y) * material.heatCapacity;
        capacity += c;
        energy += c * m_continuum->temperature(material.id, x, y);
    }
    if (capacity > 0.0) m_tempK[index(x, y)] = static_cast<float>(energy / capacity);
    refreshContinuumProxy(index(x, y));
}

double SimWorld::fluidAmount(MaterialId species) const {
    return m_continuum ? m_continuum->total(species) : static_cast<double>(countMaterial(species));
}

float SimWorld::fluidAmountAt(MaterialId species, int x, int y) const {
    if (!inBounds(x, y)) return 0.0f;
    return m_continuum ? m_continuum->amount(species, x, y) : (fromIdx(m_mat[index(x, y)]) == species ? 1.0f : 0.0f);
}

std::array<float, 4> SimWorld::fluidVisualAt(int x, int y) const {
    std::array<float, 4> result{};
    if (!m_continuum || !inBounds(x, y)) return result;
    float weight = 0.0f, liquid = 0.0f, gas = 0.0f;
    for (const auto &material : getAllMaterials()) {
        if (!ContinuumTransport::isSpecies(material.id)) continue;
        const float amount = m_continuum->amount(material.id, x, y);
        const bool isLiquid = material.phase == PhaseState::Liquid;
        if (isLiquid)
            liquid += amount;
        else
            gas += amount;
        const float optical = amount * (isLiquid ? 1.0f : 0.6f);
        weight += optical;
        result[0] += optical * material.baseR / 255.0f;
        result[1] += optical * material.baseG / 255.0f;
        result[2] += optical * material.baseB / 255.0f;
    }
    result[3] = 1.0f - (1.0f - std::clamp(liquid, 0.0f, 1.0f)) * std::exp(-gas * 0.65f);
    if (weight > 0.0f)
        for (int c = 0; c < 3; ++c)
            result[c] *= result[3] / weight;
    return result;
}

void SimWorld::refreshContinuumThermalProperties() {
    std::fill(m_contCapacity.begin(), m_contCapacity.end(), 0.0f);
    std::fill(m_contConductivity.begin(), m_contConductivity.end(), 0.0f);
    std::vector<float> fill(m_count, 0.0f);
    for (const auto &material : getAllMaterials()) {
        const auto amounts = m_continuum->field(material.id);
        for (int i = 0; i < m_count; ++i) {
            m_contCapacity[i] += amounts[i] * material.heatCapacity;
            m_contConductivity[i] += amounts[i] * material.thermalConductivity;
            fill[i] += amounts[i];
        }
    }
    for (int i = 0; i < m_count; ++i) {
        m_contCapacity[i] = std::max(1e-6f, m_contCapacity[i]);
        m_contConductivity[i] = fill[i] > 0.0f ? m_contConductivity[i] / fill[i] : 0.0f;
    }
}

void SimWorld::stepContinuum() {
    using Clock = std::chrono::steady_clock;
    const auto beginTime = Clock::now();
    const int W = m_width, H = m_height;
    const bool closed = m_boundaryMode == BoundaryMode::Closed;
    m_continuum->synchronizeTemperature(m_tempK);
    m_continuum->age(MaterialId::Smoke, 1.0f, true);
    m_continuum->age(MaterialId::Steam, 0.4f, false);
    std::fill(m_contVolume.begin(), m_contVolume.end(), 0.0f);
    std::fill(m_contDensity.begin(), m_contDensity.end(), 0.0f);
    std::fill(m_contViscosity.begin(), m_contViscosity.end(), 0.0f);
    float minimumDensity = 1e30f, maximumDensity = 0.0f;
    for (const auto &material : getAllMaterials()) {
        if (material.phase != PhaseState::Liquid) continue;
        const auto amounts = m_continuum->field(material.id);
        for (int i = 0; i < m_count; ++i) {
            m_contVolume[i] += amounts[i];
            m_contDensity[i] += amounts[i] * material.density;
            m_contViscosity[i] += amounts[i] * liquidViscosity(material.id);
        }
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            computeGravityAt(x, y, m_contGravityX[i], m_contGravityY[i]);
            m_contDensity[i] = m_contVolume[i] > 1e-6f ? m_contDensity[i] / m_contVolume[i] : 1.0f;
            m_contViscosity[i] = m_contVolume[i] > 1e-6f ? m_contViscosity[i] / m_contVolume[i] : 0.0f;
            m_solidMask[i] = getMaterial(fromIdx(m_mat[i])).isSolidObstacle() ||
                             getMaterial(fromIdx(m_mat[i])).phase == PhaseState::Powder ||
                             (closed && (x == 0 || y == 0 || x == W - 1 || y == H - 1));
            if (!m_solidMask[i] && m_contVolume[i] > 1e-6f) {
                minimumDensity = std::min(minimumDensity, m_contDensity[i]);
                maximumDensity = std::max(maximumDensity, m_contDensity[i]);
            }
        }
    }
    m_continuumMomentum->advance(m_contU, m_contV, m_contViscosity, m_solidMask, 1.0f, closed);
    // Advected face momentum under gravity; pressure projects the liquid
    // domain against its free surface, rather than moving whole material cells.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x <= W; ++x) {
            const int l = y * W + std::max(0, x - 1), r = y * W + std::min(W - 1, x);
            float gx = 0.0f, gy = 0.0f;
            computeGravityAt(std::min(x, W - 1), y, gx, gy);
            const bool wet = m_contVolume[l] > 1e-6f || m_contVolume[r] > 1e-6f;
            float &speed = m_contU[y * (W + 1) + x];
            speed = wet ? std::clamp((speed + gx) * 0.995f + 0.01f * (m_airVx[l] + m_airVx[r]), -4.0f, 4.0f) : 0.0f;
        }
    }
    for (int y = 0; y <= H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int a = std::max(0, y - 1) * W + x, b = std::min(H - 1, y) * W + x;
            float gx = 0.0f, gy = 0.0f;
            computeGravityAt(x, std::min(y, H - 1), gx, gy);
            const bool wet = m_contVolume[a] > 1e-6f || m_contVolume[b] > 1e-6f;
            float &speed = m_contV[y * W + x];
            speed = wet ? std::clamp((speed + gy) * 0.995f + 0.01f * (m_airVy[a] + m_airVy[b]), -4.0f, 4.0f) : 0.0f;
        }
    }
    const auto pressureTime = Clock::now();
    reconstructSurface(W, H, m_contVolume, m_solidMask, m_contSurface);
    // Atmospheric pressure starts at the actual free surface. Extending the
    // pressure domain into a band of air supports dilute liquid in mid-air.
    const auto projection =
        m_continuumPressure->project(m_contU, m_contV, m_contVolume, m_contDensity, m_solidMask, 1.0f, closed, 2000,
                                     maximumDensity > 4.0f * minimumDensity ? 1e-5f : 1e-6f, m_contSurface);
    m_stats.continuumDivergence = projection.divergenceAfter;
    m_stats.continuumPressureIterations = projection.iterations;
    m_stats.continuumPressureConverged = projection.converged;
    // Limit the entire projected field by one factor: unlike per-face clipping,
    // this preserves its divergence constraint during strong shock impulses.
    float maximumSpeed = 0.0f;
    for (float speed : m_contU)
        maximumSpeed = std::max(maximumSpeed, std::abs(speed));
    for (float speed : m_contV)
        maximumSpeed = std::max(maximumSpeed, std::abs(speed));
    if (maximumSpeed > 4.0f) {
        const float scale = 4.0f / maximumSpeed;
        for (float &speed : m_contU)
            speed *= scale;
        for (float &speed : m_contV)
            speed *= scale;
    }
    const auto transportTime = Clock::now();
    m_continuum->advectLiquids(m_contU, m_contV, m_solidMask, 1.0f, closed);
    m_continuum->separateLiquids(m_contGravityX, m_contGravityY, m_solidMask, 1.0f);

    std::vector<float> gasU(m_contU.size()), gasV(m_contV.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x <= W; ++x)
            gasU[y * (W + 1) + x] = 0.5f * (m_airVx[y * W + std::max(0, x - 1)] + m_airVx[y * W + std::min(W - 1, x)]);
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x < W; ++x)
            gasV[y * W + x] = 0.5f * (m_airVy[std::max(0, y - 1) * W + x] + m_airVy[std::min(H - 1, y) * W + x]);
    m_continuum->advect(gasU, gasV, m_solidMask, 1.0f, closed, PhaseState::EmptyGas);
    m_continuum->diffuseGases(m_solidMask, 1.0f);

    const auto viewTime = Clock::now();
    // A dominant-material view supports the existing inspector and reaction
    // bridge. Species fields, including sub-threshold tails, own the mass.
    std::array<const float *, kMaterialCount> amounts{}, heat{}, age{};
    for (size_t id = 0; id < kMaterialCount; ++id) {
        amounts[id] = m_continuum->field(static_cast<MaterialId>(id)).data();
        heat[id] = m_continuum->heatField(static_cast<MaterialId>(id)).data();
        age[id] = m_continuum->ageField(static_cast<MaterialId>(id)).data();
    }
    m_pool.parallelFor(0, H, [&](int y0, int y1) {
        for (int i = y0 * W; i < y1 * W; ++i) {
            const auto old = fromIdx(m_mat[i]);
            if (old != MaterialId::Air && !ContinuumTransport::isSpecies(old)) continue;
            float dominantAmount = 0.02f;
            auto dominant = MaterialId::Air;
            double energy = 0.0, capacity = 0.0;
            for (size_t id = 0; id < kMaterialCount; ++id) {
                const auto &material = kMaterialTable[id];
                const float value = amounts[id][i];
                if (value == 0.0f) continue;
                const double cellCapacity = value * material.heatCapacity;
                capacity += cellCapacity;
                energy += static_cast<double>(heat[id][i]) * material.heatCapacity;
                if (material.id != MaterialId::Air && value > dominantAmount) {
                    dominantAmount = value;
                    dominant = material.id;
                }
            }
            m_mat[i] = toIdx(dominant);
            if (capacity > 1e-8) m_tempK[i] = std::clamp(static_cast<float>(energy / capacity), 1.0f, 8000.0f);
            if (dominant != MaterialId::Air) {
                m_vx[i] = 0.5f * (m_contU[(i / W) * (W + 1) + i % W] + m_contU[(i / W) * (W + 1) + i % W + 1]);
                m_vy[i] = 0.5f * (m_contV[i] + m_contV[i + W]);
                const auto id = static_cast<size_t>(dominant);
                m_life[i] = age[id][i] / amounts[id][i];
                if (getMaterial(dominant).phase == PhaseState::EmptyGas) {
                    m_vx[i] = m_airVx[i];
                    m_vy[i] = m_airVy[i];
                }
            }
        }
    });
    const auto endTime = Clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    m_stats.continuumMomentumMs = ms(beginTime, pressureTime);
    m_stats.continuumPressureMs = ms(pressureTime, transportTime);
    m_stats.continuumTransportMs = ms(transportTime, viewTime);
    m_stats.continuumViewMs = ms(viewTime, endTime);
}

} // namespace sim
