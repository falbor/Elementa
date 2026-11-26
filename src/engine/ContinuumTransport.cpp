#include "sim/ContinuumTransport.hpp"
#include "sim/ContinuumInterface.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace sim {

bool ContinuumTransport::isSpecies(MaterialId species) noexcept {
    const auto id = static_cast<size_t>(species);
    if (id >= kMaterialCount || species == MaterialId::Air) return false;
    const auto phase = kMaterialTable[id].phase;
    return phase == PhaseState::Liquid || phase == PhaseState::EmptyGas;
}

ContinuumTransport::ContinuumTransport(int width, int height, ThreadPool *pool)
    : m_width(width), m_height(height), m_pool(pool) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        throw std::invalid_argument("Invalid continuum grid dimensions");
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    m_next.resize(n);
    m_weights.resize(n);
    m_nextHeat.resize(n);
    m_nextAge.resize(n);
    for (size_t id = 0; id < kMaterialCount; ++id) {
        m_amount[id].resize(n);
        m_heat[id].resize(n);
        m_age[id].resize(n);
    }
}

size_t ContinuumTransport::checkedSpecies(MaterialId species) const {
    if (static_cast<size_t>(species) >= kMaterialCount) throw std::invalid_argument("Invalid material species");
    return static_cast<size_t>(species);
}

size_t ContinuumTransport::checkedCell(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) throw std::out_of_range("Continuum cell");
    return static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
}

void ContinuumTransport::clear() noexcept {
    for (auto &channel : m_amount)
        std::fill(channel.begin(), channel.end(), 0.0f);
    for (auto &channel : m_heat)
        std::fill(channel.begin(), channel.end(), 0.0f);
    for (auto &channel : m_age)
        std::fill(channel.begin(), channel.end(), 0.0f);
}

void ContinuumTransport::clearCell(int x, int y) {
    const auto i = checkedCell(x, y);
    for (auto &channel : m_amount)
        if (!channel.empty()) channel[i] = 0.0f;
    for (auto &channel : m_heat)
        if (!channel.empty()) channel[i] = 0.0f;
    for (auto &channel : m_age)
        if (!channel.empty()) channel[i] = 0.0f;
}

void ContinuumTransport::swapCells(int ax, int ay, int bx, int by) {
    const auto a = checkedCell(ax, ay), b = checkedCell(bx, by);
    for (auto &channel : m_amount)
        if (!channel.empty()) std::swap(channel[a], channel[b]);
    for (auto &channel : m_heat)
        if (!channel.empty()) std::swap(channel[a], channel[b]);
    for (auto &channel : m_age)
        if (!channel.empty()) std::swap(channel[a], channel[b]);
}

void ContinuumTransport::set(MaterialId species, int x, int y, float value) {
    setState(species, x, y, value, getMaterial(species).defaultTempK,
             species == MaterialId::Smoke || species == MaterialId::Steam ? 350.0f : 0.0f);
}

void ContinuumTransport::setState(MaterialId species, int x, int y, float value, float temp, float life) {
    if (!std::isfinite(value) || value < 0.0f) throw std::invalid_argument("Invalid continuum amount");
    if (!std::isfinite(temp) || temp <= 0.0f || !std::isfinite(life) || life < 0.0f)
        throw std::invalid_argument("Invalid continuum thermodynamic state");
    const auto id = checkedSpecies(species), i = checkedCell(x, y);
    m_amount[id][i] = value;
    m_heat[id][i] = value * temp;
    m_age[id][i] = value * life;
}

float ContinuumTransport::temperature(MaterialId species, int x, int y) const {
    const auto id = checkedSpecies(species), i = checkedCell(x, y);
    return m_amount[id][i] > 0.0f ? m_heat[id][i] / m_amount[id][i] : getMaterial(species).defaultTempK;
}

float ContinuumTransport::lifetime(MaterialId species, int x, int y) const {
    const auto id = checkedSpecies(species), i = checkedCell(x, y);
    return m_amount[id][i] > 0.0f ? m_age[id][i] / m_amount[id][i] : 0.0f;
}

void ContinuumTransport::setLifetime(MaterialId species, int x, int y, float remaining) {
    if (!std::isfinite(remaining) || remaining < 0.0f) throw std::invalid_argument("Invalid lifetime");
    const auto id = checkedSpecies(species), i = checkedCell(x, y);
    m_age[id][i] = m_amount[id][i] * remaining;
}

double ContinuumTransport::thermalEnergy(MaterialId species) const {
    double energy = 0.0;
    for (float heat : m_heat[checkedSpecies(species)])
        energy += heat;
    return energy * getMaterial(species).heatCapacity;
}

void ContinuumTransport::synchronizeTemperature(std::span<const float> temp) {
    if (temp.size() != m_next.size()) throw std::invalid_argument("Invalid temperature field");
    for (size_t i = 0; i < temp.size(); ++i)
        if (!std::isfinite(temp[i]) || temp[i] <= 0.0f)
            throw std::invalid_argument("Invalid temperature at cell " + std::to_string(i) + ": " +
                                        std::to_string(temp[i]));
    for (size_t id = 0; id < kMaterialCount; ++id)
        for (size_t i = 0; i < m_amount[id].size(); ++i)
            m_heat[id][i] = m_amount[id][i] * temp[i];
}

void ContinuumTransport::age(MaterialId species, float elapsed, bool removeExpired) {
    if (!std::isfinite(elapsed) || elapsed < 0.0f) throw std::invalid_argument("Invalid age step");
    const auto id = checkedSpecies(species);
    for (size_t i = 0; i < m_amount[id].size(); ++i) {
        m_age[id][i] = std::max(0.0f, m_age[id][i] - m_amount[id][i] * elapsed);
        if (removeExpired && m_age[id][i] == 0.0f) m_amount[id][i] = m_heat[id][i] = 0.0f;
    }
}

void ContinuumTransport::convert(MaterialId from, MaterialId to, int x, int y) {
    convertAmount(from, to, x, y, amount(from, x, y));
}

void ContinuumTransport::convertAmount(MaterialId from, MaterialId to, int x, int y, float requested) {
    if (!std::isfinite(requested) || requested < 0.0f) throw std::invalid_argument("Invalid reaction amount");
    const auto a = checkedSpecies(from), b = checkedSpecies(to), i = checkedCell(x, y);
    if (a == b) return;
    const float convertedAmount = std::min(requested, m_amount[a][i]);
    if (convertedAmount == 0.0f) return;
    const float fraction = convertedAmount / m_amount[a][i];
    const float heat = m_heat[a][i] * fraction;
    m_amount[b][i] += convertedAmount;
    m_amount[a][i] -= convertedAmount;
    // Preserve thermal energy, including different species heat capacities.
    m_heat[b][i] += heat * (getMaterial(from).heatCapacity / getMaterial(to).heatCapacity);
    m_heat[a][i] -= heat;
    const float newLife = to == MaterialId::Smoke || to == MaterialId::Steam ? 350.0f
                          : to == MaterialId::Fire                           ? 28.0f
                          : to == MaterialId::Spark                          ? 14.0f
                          : to == MaterialId::Plasma                         ? 45.0f
                          : to == MaterialId::Acid                           ? 100.0f
                                                                             : 0.0f;
    m_age[b][i] += convertedAmount * newLife;
    m_age[a][i] *= 1.0f - fraction;
}

float ContinuumTransport::amount(MaterialId species, int x, int y) const {
    return m_amount[checkedSpecies(species)][checkedCell(x, y)];
}

std::span<const float> ContinuumTransport::field(MaterialId species) const {
    return m_amount[checkedSpecies(species)];
}

double ContinuumTransport::total(MaterialId species) const {
    double result = 0.0;
    for (float value : field(species))
        result += value;
    return result;
}

void ContinuumTransport::separateLiquids(std::span<const float> gx, std::span<const float> gy,
                                         std::span<const uint8_t> solid, float dt) {
    if (gx.size() != m_next.size() || gy.size() != m_next.size() || solid.size() != m_next.size() ||
        !std::isfinite(dt) || dt < 0.0f || dt > 1.0f)
        throw std::invalid_argument("Invalid separation fields");
    for (size_t i = 0; i < gx.size(); ++i)
        if (!std::isfinite(gx[i]) || !std::isfinite(gy[i]) || std::abs(gx[i]) > 1.0f || std::abs(gy[i]) > 1.0f)
            throw std::invalid_argument("Invalid separation gravity");
    std::vector<size_t> liquids;
    for (size_t id = 0; id < kMaterialCount; ++id)
        if (kMaterialTable[id].phase == PhaseState::Liquid && total(static_cast<MaterialId>(id)) > 0.0)
            liquids.push_back(id);
    if (liquids.size() < 2) return;
    auto face = [&](int a, int b, float gravity) {
        if (solid[a] || solid[b] || gravity == 0.0f) return;
        const int top = gravity > 0.0f ? a : b, bottom = gravity > 0.0f ? b : a;
        for (size_t heavy : liquids)
            for (size_t light : liquids) {
                const float contrast =
                    (kMaterialTable[heavy].density - kMaterialTable[light].density) / kMaterialTable[heavy].density;
                if (contrast <= 0.03f) continue;
                const float h = m_amount[heavy][top], l = m_amount[light][bottom];
                const float q = std::min(h, l) * (0.2f * dt * std::abs(gravity) * contrast);
                if (!(q > 0.0f)) continue;
                // Equal opposing volume fluxes leave each cell's total fill unchanged.
                const float heatH = m_heat[heavy][top] * (q / h), heatL = m_heat[light][bottom] * (q / l);
                const float ageH = m_age[heavy][top] * (q / h), ageL = m_age[light][bottom] * (q / l);
                m_amount[heavy][top] -= q;
                m_amount[heavy][bottom] += q;
                m_amount[light][bottom] -= q;
                m_amount[light][top] += q;
                m_heat[heavy][top] -= heatH;
                m_heat[heavy][bottom] += heatH;
                m_heat[light][bottom] -= heatL;
                m_heat[light][top] += heatL;
                m_age[heavy][top] -= ageH;
                m_age[heavy][bottom] += ageH;
                m_age[light][bottom] -= ageL;
                m_age[light][top] += ageL;
            }
    };
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < m_width; ++x) {
            const int i = y * m_width + x;
            if (x + 1 < m_width) face(i, i + 1, 0.5f * (gx[i] + gx[i + 1]));
            if (y + 1 < m_height) face(i, i + m_width, 0.5f * (gy[i] + gy[i + m_width]));
        }
}

void ContinuumTransport::diffuseGases(std::span<const uint8_t> solid, float dt, float diffusivity) {
    if (solid.size() != m_next.size() || !std::isfinite(dt) || dt < 0.0f || dt > 1.0f || !std::isfinite(diffusivity) ||
        diffusivity < 0.0f || diffusivity > 0.125f)
        throw std::invalid_argument("Invalid gas diffusion parameters");
    for (size_t id = 0; id < kMaterialCount; ++id) {
        if (kMaterialTable[id].phase != PhaseState::EmptyGas) continue;
        auto &amounts = m_amount[id];
        if (std::none_of(amounts.begin(), amounts.end(), [](float q) { return q > 0.0f; })) continue;
        m_next = amounts;
        m_nextHeat = m_heat[id];
        m_nextAge = m_age[id];
        auto face = [&](int a, int b) {
            if (solid[a] || solid[b]) return;
            const float difference = amounts[a] - amounts[b];
            const int source = difference >= 0.0f ? a : b, target = difference >= 0.0f ? b : a;
            const float q = dt * diffusivity * std::abs(difference);
            if (q == 0.0f) return;
            const float fraction = q / amounts[source];
            const float heat = m_heat[id][source] * fraction, age = m_age[id][source] * fraction;
            m_next[source] -= q;
            m_next[target] += q;
            m_nextHeat[source] -= heat;
            m_nextHeat[target] += heat;
            m_nextAge[source] -= age;
            m_nextAge[target] += age;
        };
        for (int y = 0; y < m_height; ++y)
            for (int x = 0; x < m_width; ++x) {
                const int i = y * m_width + x;
                if (x + 1 < m_width) face(i, i + 1);
                if (y + 1 < m_height) face(i, i + m_width);
            }
        amounts.swap(m_next);
        m_heat[id].swap(m_nextHeat);
        m_age[id].swap(m_nextAge);
    }
}

void ContinuumTransport::advectLiquids(std::span<const float> u, std::span<const float> v,
                                       std::span<const uint8_t> solid, float dt, bool closed) {
    const int W = m_width, H = m_height, N = W * H;
    if (u.size() != static_cast<size_t>(H) * (W + 1) || v.size() != static_cast<size_t>(H + 1) * W ||
        solid.size() != static_cast<size_t>(N) || !std::isfinite(dt) || dt < 0.0f)
        throw std::invalid_argument("Invalid liquid transport fields");
    float maximum = 0.0f;
    for (auto field : {u, v})
        for (float speed : field) {
            if (!std::isfinite(speed)) throw std::invalid_argument("Non-finite liquid velocity");
            maximum = std::max(maximum, std::abs(speed));
        }
    float maximumRate = 0.0f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (solid[y * W + x]) continue;
            const float l = -u[y * (W + 1) + x], r = u[y * (W + 1) + x + 1];
            const float a = -v[y * W + x], b = v[(y + 1) * W + x];
            const float out = std::max(0.0f, l) + std::max(0.0f, r) + std::max(0.0f, a) + std::max(0.0f, b);
            // CFL constrains the volume swept out of each donor. Receiver
            // capacity is enforced conservatively below, including converging
            // streams; a separate incoming CFL unnecessarily subdivides them.
            maximumRate = std::max(maximumRate, out);
        }
    const double required = std::ceil(static_cast<double>(maximumRate) * dt / 0.9);
    if (required > 4096.0) throw std::invalid_argument("Liquid step requires too many substeps");
    if (maximum == 0.0f || dt == 0.0f) return;
    const int steps = std::max(1, static_cast<int>(required));
    const float h = dt / steps;
    std::vector<size_t> species;
    for (size_t id = 0; id < kMaterialCount; ++id)
        if (kMaterialTable[id].phase == PhaseState::Liquid &&
            std::any_of(m_amount[id].begin(), m_amount[id].end(), [](float q) { return q > 0.0f; }))
            species.push_back(id);
    if (species.empty()) return;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (size_t id : species)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (m_amount[id][y * W + x] <= 0.0f) continue;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    // A face flux moves at most one cell per substep, including arbitrarily
    // small tails. This bound skips empty space without thresholding any mass.
    minX = std::max(0, minX - steps - 1);
    maxX = std::min(W - 1, maxX + steps + 1);
    minY = std::max(0, minY - steps - 1);
    maxY = std::min(H - 1, maxY + steps + 1);
    auto &[fluxes, fill, interfaceOffset, outgoing, outgoingFraction, transferFraction, incomingHead, nextFlux, outHead,
           nextOut, parent, root, search, touched, marked] = m_liquid;
    // Grid size is immutable. Reuse both cell scratch storage and growing
    // face/adjacency lists instead of reallocating them at every physics tick.
    if (fill.size() != static_cast<size_t>(N)) {
        fill.resize(N);
        marked.resize(N);
        for (auto *buffer : {&interfaceOffset, &outgoing, &outgoingFraction})
            buffer->resize(N);
        for (auto *buffer : {&incomingHead, &outHead, &parent, &root})
            buffer->resize(N);
    }
    // Piecewise-linear geometric reconstruction (PLIC). The occupied region
    // lies on the denser side of the interface. A shared volume flux transports every
    // liquid component and its thermal/age moments in the same proportion.
    for (int step = 0; step < steps; ++step) {
        auto t0 = std::chrono::steady_clock::now();
        std::fill(fill.begin(), fill.end(), 0.0f);
        for (size_t id : species)
            for (int y = minY; y <= maxY; ++y)
                for (int i = y * W + minX; i <= y * W + maxX; ++i)
                    fill[i] += m_amount[id][i];
        auto sample = [&](int x, int y, int center) {
            if (x < 0 || y < 0 || x >= W || y >= H || solid[y * W + x]) return fill[center];
            return fill[y * W + x];
        };
        // Reconstruct each interface once, not again for every incident face.
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x) {
                const int i = y * W + x;
                m_next[i] = m_nextHeat[i] = 0.0f;
                if (fill[i] > 0.0f && fill[i] < 1.0f) {
                    m_next[i] = sample(x + 1, y, i) - sample(x - 1, y, i);
                    m_nextHeat[i] = sample(x, y + 1, i) - sample(x, y - 1, i);
                    // Preserve axis-aligned interfaces at corners.
                    auto limited = [](float a, float b) {
                        return a * b > 0.0f ? std::copysign(2.0f * std::min(std::abs(a), std::abs(b)), a) : 0.0f;
                    };
                    const float nx = limited(fill[i] - sample(x - 1, y, i), sample(x + 1, y, i) - fill[i]);
                    const float ny = limited(fill[i] - sample(x, y - 1, i), sample(x, y + 1, i) - fill[i]);
                    if (nx != 0.0f || ny != 0.0f) {
                        m_next[i] = nx;
                        m_nextHeat[i] = ny;
                    }
                    // A 3x3 weighted gradient reduces angular bias on curved
                    // interfaces. Retain exact fractional strips so a flat
                    // rectangle does not acquire rounded corners in translation.
                    const bool verticalStrip = ny == 0.0f && nx != 0.0f &&
                                               (std::abs(sample(x, y - 1, i) - fill[i]) < 1e-6f ||
                                                std::abs(sample(x, y + 1, i) - fill[i]) < 1e-6f);
                    const bool horizontalStrip = nx == 0.0f && ny != 0.0f &&
                                                 (std::abs(sample(x - 1, y, i) - fill[i]) < 1e-6f ||
                                                  std::abs(sample(x + 1, y, i) - fill[i]) < 1e-6f);
                    if (!verticalStrip && !horizontalStrip) {
                        m_next[i] = (sample(x + 1, y - 1, i) + 2.0f * sample(x + 1, y, i) + sample(x + 1, y + 1, i) -
                                     sample(x - 1, y - 1, i) - 2.0f * sample(x - 1, y, i) - sample(x - 1, y + 1, i)) *
                                    0.25f;
                        m_nextHeat[i] =
                            (sample(x - 1, y + 1, i) + 2.0f * sample(x, y + 1, i) + sample(x + 1, y + 1, i) -
                             sample(x - 1, y - 1, i) - 2.0f * sample(x, y - 1, i) - sample(x + 1, y - 1, i)) *
                            0.25f;
                    }
                    interfaceOffset[i] = planeOffset(m_next[i], m_nextHeat[i], fill[i]);
                }
            }
        fluxes.clear();
        touched.clear();
        std::fill(marked.begin(), marked.end(), 0);
        auto face = [&](int a, int b, float speed, int axis) {
            if ((a < 0 || b < 0) && closed) return;
            if ((a >= 0 && solid[a]) || (b >= 0 && solid[b])) return;
            const int source = speed >= 0.0f ? a : b, target = speed >= 0.0f ? b : a;
            if (source < 0 || fill[source] <= 0.0f) return;
            const float gx = m_next[source], gy = m_nextHeat[source];
            const float q = std::min(1.0f, fill[source]), courant = std::abs(speed) * h;
            float volume = courant * q;
            if (std::max(std::abs(gx), std::abs(gy)) > 1e-6f) {
                const double offset = interfaceOffset[source];
                const double center = (speed >= 0.0f ? 1.0 : -1.0) * (1.0 - courant) * 0.5;
                const int sx = source % W, sy = source / W;
                // Sample the swept strip at its transverse midpoint in time.
                // Omitting this displacement introduces a first-order lateral
                // bias even in divergence-free rotation. The shared bounded
                // flux still transports all species and moments conservatively.
                const double transverse = axis == 0 ? 0.5 * (v[sy * W + sx] + v[(sy + 1) * W + sx])
                                                    : 0.5 * (u[sy * (W + 1) + sx] + u[sy * (W + 1) + sx + 1]);
                const double localOffset =
                    offset - center * (axis == 0 ? gx : gy) + 0.5 * h * transverse * (axis == 0 ? gy : gx);
                volume = static_cast<float>(
                    courant * planeFraction(gx, gy, localOffset, axis == 0 ? courant : 1.0, axis == 1 ? courant : 1.0));
            }
            if (volume > 0.0f) {
                fluxes.push_back({source, target, volume});
                for (int i : {source, target})
                    if (i >= 0 && !marked[i]) {
                        marked[i] = 1;
                        touched.push_back(i);
                    }
            }
        };
        // Use both axes together: the projected field is divergence-free in
        // two dimensions, not in each directional sweep separately.
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX + 1; ++x)
                face(x > 0 ? y * W + x - 1 : -1, x < W ? y * W + x : -1, u[y * (W + 1) + x], 0);
        for (int y = minY; y <= maxY + 1; ++y)
            for (int x = minX; x <= maxX; ++x)
                face(y > 0 ? (y - 1) * W + x : -1, y < H ? y * W + x : -1, v[y * W + x], 1);
        for (int i : touched)
            outgoing[i] = 0.0;
        for (const auto &f : fluxes)
            outgoing[f.source] += f.amount;
        for (auto &f : fluxes) {
            // Leave roundoff room when several faces empty the same donor;
            // every transferred amount is still added to its recipient.
            const float available = fill[f.source] * (1.0f - 1e-6f);
            if (outgoing[f.source] > available) f.amount *= available / outgoing[f.source];
        }
        auto t1 = std::chrono::steady_clock::now();
        // Residual-network push/relabel routes excess to spare capacity.
        // Distance labels prevent corrections circulating around full pools.
        nextFlux.assign(fluxes.size(), -1);
        nextOut.assign(fluxes.size(), -1);
        auto &future = outgoingFraction;
        // Only endpoints of the current flux graph are read below. Scratch
        // values elsewhere may be stale and need not be cleared each substep.
        for (int i : touched) {
            incomingHead[i] = outHead[i] = -1;
            future[i] = fill[i];
        }
        for (size_t k = 0; k < fluxes.size(); ++k) {
            const auto &f = fluxes[k];
            future[f.source] -= f.amount;
            nextOut[k] = outHead[f.source];
            outHead[f.source] = static_cast<int>(k);
            if (f.target >= 0) {
                future[f.target] += f.amount;
                nextFlux[k] = incomingHead[f.target];
                incomingHead[f.target] = static_cast<int>(k);
            }
        }
        auto capacity = [&](int i) { return std::max(1.0, static_cast<double>(fill[i])); };
        auto &height = parent;
        auto &cursor = root;
        constexpr double edgeEpsilon = 1e-12;
        double spareThreshold = 1e-4;
        const int unreachable = N * 2;
        auto globalRelabel = [&]() {
            for (int i : touched) {
                height[i] = unreachable;
                cursor[i] = incomingHead[i];
            }
            search.clear();
            for (int i : touched)
                if (future[i] < capacity(i) - spareThreshold) {
                    height[i] = 0;
                    search.push_back(i);
                }
            for (size_t k = 0; k < search.size(); ++k) {
                const int i = search[k];
                for (int f = outHead[i]; f >= 0; f = nextOut[f]) {
                    const auto &edge = fluxes[f];
                    if (edge.target < 0 || edge.amount <= edgeEpsilon || height[edge.target] != unreachable) continue;
                    height[edge.target] = height[i] + 1;
                    search.push_back(edge.target);
                }
            }
        };
        auto relabel = [&](int i) {
            int minimum = unreachable;
            for (int k = incomingHead[i]; k >= 0; k = nextFlux[k])
                if (fluxes[k].amount > edgeEpsilon) minimum = std::min(minimum, height[fluxes[k].source]);
            height[i] = minimum < unreachable ? minimum + 1 : unreachable;
            cursor[i] = incomingHead[i];
        };
        std::deque<int> pending;
        for (int i : touched)
            marked[i] = 0;
        auto enqueue = [&](int i) {
            if (!marked[i] && future[i] > capacity(i) + 2e-6 && height[i] < unreachable) {
                marked[i] = 1;
                pending.push_back(i);
            }
        };
        auto t2 = std::chrono::steady_clock::now();
        size_t pushes = 0, relabels = 0;
        for (int pass = 0; pass < 2; ++pass) {
            // Prefer meaningful spare capacity before routing into roundoff-
            // sized vacancies. Any remaining excess then uses the original
            // fine threshold: this changes routing, never the volume limit.
            if (std::none_of(touched.begin(), touched.end(), [&](int i) { return future[i] > capacity(i) + 2e-6; }))
                break;
            spareThreshold = pass == 0 ? 1e-4 : edgeEpsilon;
            globalRelabel();
            for (int i : touched)
                enqueue(i);
            size_t discharges = 0;
            while (!pending.empty()) {
                const int i = pending.front();
                pending.pop_front();
                marked[i] = 0;
                while (future[i] > capacity(i) + 2e-6 && height[i] < unreachable) {
                    int k = cursor[i];
                    if (k < 0) {
                        ++relabels;
                        relabel(i);
                        continue;
                    }
                    auto &edge = fluxes[k];
                    const int j = edge.source;
                    if (edge.amount <= edgeEpsilon) {
                        cursor[i] = nextFlux[k];
                        continue;
                    }
                    if (height[j] == 0 && future[j] >= capacity(j) - spareThreshold) relabel(j);
                    if (height[i] != height[j] + 1) {
                        cursor[i] = nextFlux[k];
                        continue;
                    }
                    double amount = std::min(future[i] - capacity(i), edge.amount);
                    if (height[j] == 0) amount = std::min(amount, capacity(j) - future[j]);
                    if (amount <= edgeEpsilon) {
                        cursor[i] = nextFlux[k];
                        continue;
                    }
                    ++pushes;
                    edge.amount -= amount;
                    future[i] -= amount;
                    future[j] += amount;
                    enqueue(j);
                }
                if (++discharges >= std::max<size_t>(1, touched.size() * 4)) {
                    globalRelabel();
                    discharges = 0;
                    for (int j : touched)
                        enqueue(j);
                }
            }
        }
        auto t3 = std::chrono::steady_clock::now();
        for (int i : touched)
            outgoingFraction[i] = 0.0;
        transferFraction.resize(fluxes.size());
        for (size_t k = 0; k < fluxes.size(); ++k) {
            const auto &f = fluxes[k];
            transferFraction[k] = static_cast<double>(f.amount) / fill[f.source];
            outgoingFraction[f.source] += transferFraction[k];
        }
        for (size_t id : species) {
            // Positive gather avoids subtracting four separately rounded
            // subnormal amounts from a trace species and producing -FLT_TRUE_MIN.
            // Only endpoints of proposed fluxes can change. Keep the source
            // fields intact until every gather completes, then commit those
            // cells without copying all three full-grid fields per species.
            auto gather = [&](int first, int last) {
                for (int cell = first; cell < last; ++cell) {
                    const int i = touched[cell];
                    const double remaining = std::max(0.0, 1.0 - outgoingFraction[i]);
                    double q = m_amount[id][i] * remaining, heat = m_heat[id][i] * remaining,
                           age = m_age[id][i] * remaining;
                    for (int k = incomingHead[i]; k >= 0; k = nextFlux[k]) {
                        const int source = fluxes[k].source;
                        const double fraction = transferFraction[k];
                        q += m_amount[id][source] * fraction;
                        heat += m_heat[id][source] * fraction;
                        age += m_age[id][source] * fraction;
                    }
                    m_next[i] = static_cast<float>(q);
                    m_nextHeat[i] = static_cast<float>(heat);
                    m_nextAge[i] = static_cast<float>(age);
                }
            };
            if (m_pool)
                m_pool->parallelFor(0, static_cast<int>(touched.size()), gather);
            else
                gather(0, static_cast<int>(touched.size()));
            for (int i : touched) {
                m_amount[id][i] = m_next[i];
                m_heat[id][i] = m_nextHeat[i];
                m_age[id][i] = m_nextAge[i];
            }
        }
        auto t4 = std::chrono::steady_clock::now();
        auto ms=[](auto a, auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        std::cerr << "transport-profile " << ms(t0,t1) << ' ' << ms(t1,t2) << ' ' << ms(t2,t3) << ' ' << ms(t3,t4) << ' ' << pushes << ' ' << relabels << ' ' << fluxes.size() << '\n';
    }
}

void ContinuumTransport::advect(std::span<const float> u, std::span<const float> v, std::span<const uint8_t> solid,
                                float dt, bool closed, std::optional<PhaseState> phase) {
    const int W = m_width, H = m_height;
    if (u.size() != static_cast<size_t>(H) * (W + 1) || v.size() != static_cast<size_t>(H + 1) * W ||
        solid.size() != m_next.size() || !std::isfinite(dt) || dt < 0.0f)
        throw std::invalid_argument("Invalid continuum transport fields");
    for (auto faces : {u, v}) {
        for (float speed : faces) {
            if (!std::isfinite(speed)) throw std::invalid_argument("Non-finite face velocity");
        }
    }
    float maxOutflow = 0.0f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            auto open = [&](int nx, int ny) {
                return nx < 0 || ny < 0 || nx >= W || ny >= H ? !closed : solid[ny * W + nx] == 0;
            };
            auto &w = m_weights[i];
            w = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            if (solid[i]) continue;
            float outgoing = 0.0f;
            auto face = [&](bool passable, bool inside, float speed, float &incoming) {
                if (!passable) return;
                outgoing += std::max(0.0f, speed);
                if (inside) incoming = std::max(0.0f, -speed);
            };
            face(open(x - 1, y), x > 0, -u[y * (W + 1) + x], w.left);
            face(open(x + 1, y), x + 1 < W, u[y * (W + 1) + x + 1], w.right);
            face(open(x, y - 1), y > 0, -v[y * W + x], w.up);
            face(open(x, y + 1), y + 1 < H, v[(y + 1) * W + x], w.down);
            w.keep = outgoing;
            maxOutflow = std::max(maxOutflow, outgoing);
        }
    if (dt == 0.0f || maxOutflow == 0.0f) return;
    // Tight multidimensional CFL bound, using actual outgoing face rates.
    const double required = std::ceil(static_cast<double>(dt) * maxOutflow / 0.9);
    if (required > 4096.0) throw std::invalid_argument("Continuum step requires too many substeps");
    const int steps = std::max(1, static_cast<int>(required));
    const float h = dt / static_cast<float>(steps);
    for (auto &w : m_weights) {
        w.keep = 1.0f - w.keep * h;
        w.left *= h;
        w.right *= h;
        w.up *= h;
        w.down *= h;
    }
    std::vector<size_t> activeSpecies;
    for (size_t id = 0; id < kMaterialCount; ++id) {
        const auto p = kMaterialTable[id].phase;
        if ((p == PhaseState::EmptyGas || p == PhaseState::Liquid) && (!phase || p == *phase) &&
            std::any_of(m_amount[id].begin(), m_amount[id].end(), [](float q) { return q > 0.0f; }))
            activeSpecies.push_back(id);
    }
    for (int step = 0; step < steps; ++step) {
        for (size_t species : activeSpecies) {
            auto &channel = m_amount[species];
            auto rows = [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < W; ++x) {
                        const int i = y * W + x, l = x > 0 ? i - 1 : i, r = x + 1 < W ? i + 1 : i;
                        const int up = y > 0 ? i - W : i, down = y + 1 < H ? i + W : i;
                        const auto &w = m_weights[i];
                        auto gather = [&](const auto &field) {
                            return field[i] * w.keep + field[l] * w.left + field[r] * w.right + field[up] * w.up +
                                   field[down] * w.down;
                        };
                        m_next[i] = gather(channel);
                        m_nextHeat[i] = gather(m_heat[species]);
                        m_nextAge[i] = gather(m_age[species]);
                    }
            };
            if (m_pool)
                m_pool->parallelFor(0, H, rows);
            else
                rows(0, H);
            channel.swap(m_next);
            m_heat[species].swap(m_nextHeat);
            m_age[species].swap(m_nextAge);
        }
    }
}

} // namespace sim
