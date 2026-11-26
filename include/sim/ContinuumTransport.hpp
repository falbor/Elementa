#pragma once

#include "sim/Material.hpp"

#include "sim/ThreadPool.hpp"
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace sim {

// Authoritative fractional species amounts. Cells are control volumes, not
// particles: multiple gas/liquid species can share a cell and move fractions
// of their contents through faces in a single tick.
// This is the conservative transport stage; callers supply face velocities
// from their pressure/free-surface solve and couple reactions separately.
// Air is also stored as a thermal carrier so displaced background heat is not
// recreated when fractional liquid or gas concentrations change.
class ContinuumTransport {
public:
    ContinuumTransport(int width, int height, ThreadPool *pool = nullptr);
    void clear() noexcept;
    void clearCell(int x, int y);
    void swapCells(int ax, int ay, int bx, int by);
    void set(MaterialId species, int x, int y, float amount);
    void setState(MaterialId species, int x, int y, float amount, float temperature, float lifetime);
    float temperature(MaterialId species, int x, int y) const;
    float lifetime(MaterialId species, int x, int y) const;
    void setLifetime(MaterialId species, int x, int y, float remaining);
    double thermalEnergy(MaterialId species) const;
    void synchronizeTemperature(std::span<const float> temperature);
    void age(MaterialId species, float elapsed, bool removeExpired);
    void convert(MaterialId from, MaterialId to, int x, int y);
    void convertAmount(MaterialId from, MaterialId to, int x, int y, float amount);
    void separateLiquids(std::span<const float> gravityX, std::span<const float> gravityY,
                         std::span<const uint8_t> solid, float dt);
    void diffuseGases(std::span<const uint8_t> solid, float dt, float diffusivity = 0.025f);
    // Bounded volume-of-fluid transport. Reconstruct a liquid interface instead
    // of diffusing its volume fraction as a gaseous concentration.
    void advectLiquids(std::span<const float> u, std::span<const float> v, std::span<const uint8_t> solid, float dt,
                       bool closed);
    float amount(MaterialId species, int x, int y) const;
    double total(MaterialId species) const;
    std::span<const float> field(MaterialId species) const;
    std::span<const float> heatField(MaterialId species) const { return m_heat[checkedSpecies(species)]; }
    std::span<const float> ageField(MaterialId species) const { return m_age[checkedSpecies(species)]; }

    // u: H*(W+1) vertical faces, v: (H+1)*W horizontal faces.
    // Solid cells have zero face flux, including sealed diagonal corners.
    // Closed borders have zero flux; open borders allow outflow, never inflow.
    // No evaporation/decay or mass clipping occurs in this stage.
    void advect(std::span<const float> u, std::span<const float> v, std::span<const uint8_t> solid, float dt,
                bool closed, std::optional<PhaseState> phase = std::nullopt);
    static bool isSpecies(MaterialId species) noexcept;

private:
    int m_width, m_height;
    ThreadPool *m_pool;
    struct Weights {
        float keep, left, right, up, down;
    };
    std::vector<Weights> m_weights;
    std::array<std::vector<float>, kMaterialCount> m_amount;
    std::array<std::vector<float>, kMaterialCount> m_heat, m_age;
    std::vector<float> m_next, m_nextHeat, m_nextAge;
    struct LiquidFlux {
        int source, target;
        double amount;
    };
    struct LiquidWorkspace {
        std::vector<LiquidFlux> fluxes;
        std::vector<float> fill;
        std::vector<double> interfaceOffset, outgoing, outgoingFraction, transferFraction;
        std::vector<int> incomingHead, nextFlux, outHead, nextOut, parent, root, search, touched;
        std::vector<uint8_t> marked;
    } m_liquid;
    size_t checkedSpecies(MaterialId species) const;
    size_t checkedCell(int x, int y) const;
};

} // namespace sim
