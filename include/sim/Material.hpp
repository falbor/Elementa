#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace sim {

enum class MaterialId : uint8_t {
    Air = 0,
    Bedrock,
    Stone,
    Sand,
    Water,
    SaltWater,
    Salt,
    Ice,
    Snow,
    Wood,
    Plant,
    Oil,
    Acid,
    Metal,
    MoltenMetal,
    Glass,
    MoltenGlass,
    Lava,
    Obsidian,
    Gunpowder,
    Nitro,
    Fire,
    Smoke,
    Steam,
    Methane,
    Spark,
    Plasma,
    Battery,
    Count
};

enum class PhaseState : uint8_t {
    EmptyGas = 0, // Air or gases occupying fluid cells
    Powder,       // Granular solids subject to gravity + angle of repose
    Liquid,       // Incompressible-like flowing liquids with hydrostatic dispersion
    StaticSolid,  // Immovable structural solids (walls, metal, ice, wood, glass)
    Energy        // Fire, sparks, plasma
};

struct MaterialProps {
    MaterialId id;
    std::string_view nameIt;
    std::string_view shortLabel;
    PhaseState phase;

    float density;                // Relative density (Air=0.0012, Water=1.0, Sand=1.6, Metal=7.8)
    float thermalConductivity;    // Normalized [0.01 .. 1.0]
    float heatCapacity;           // Volumetric heat capacity [0.2 .. 5.0]
    float electricalConductivity; // [0.0 .. 1.0]
    float lightTransmittance;     // [0.0 .. 1.0] per pixel step
    float dragCoeff;              // Coupling to Eulerian air velocity [0.0 .. 1.0]
    float restitution;            // Bounce elasticity [0.0 .. 0.9]
    int dispersion;               // Horizontal spread radius per tick (liquids/gases)

    float defaultTempK;           // Spawn temperature in Kelvin (293.15 K = 20 C)
    float meltTempK;              // 0 if does not melt
    float freezeTempK;            // 0 if does not freeze
    float boilTempK;              // 0 if does not boil
    float condenseTempK;          // 0 if does not condense
    float ignitionTempK;          // 0 if non-flammable

    uint8_t baseR;
    uint8_t baseG;
    uint8_t baseB;
    uint8_t colorVariance;        // Per-grain visual noise

    float emissiveR;
    float emissiveG;
    float emissiveB;

    constexpr bool isSolidObstacle() const noexcept {
        return phase == PhaseState::StaticSolid;
    }

    constexpr bool isMovable() const noexcept {
        return phase == PhaseState::Powder || phase == PhaseState::Liquid ||
               phase == PhaseState::Energy ||
               (phase == PhaseState::EmptyGas && id != MaterialId::Air);
    }

    constexpr bool isGasOrEnergy() const noexcept {
        return phase == PhaseState::EmptyGas || phase == PhaseState::Energy;
    }
};

constexpr size_t kMaterialCount = static_cast<size_t>(MaterialId::Count);
constexpr float kAmbientTempK = 293.15f; // 20.0 °C
constexpr float kKelvinZeroC = 273.15f;

inline constexpr std::array<MaterialProps, kMaterialCount> kMaterialTable = {{
    // Air
    {
        MaterialId::Air, "Aria / Vuoto", "Aria", PhaseState::EmptyGas,
        0.0012f, 0.06f, 0.25f, 0.0f, 0.96f, 0.0f, 0.0f, 0,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        11, 14, 22, 0,
        0.0f, 0.0f, 0.0f
    },
    // Bedrock
    {
        MaterialId::Bedrock, "Muro Indistruttibile", "Muro", PhaseState::StaticSolid,
        10.0f, 0.15f, 4.5f, 0.0f, 0.20f, 0.0f, 0.4f, 0,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        58, 63, 76, 10,
        0.0f, 0.0f, 0.0f
    },
    // Stone
    {
        MaterialId::Stone, "Pietra", "Pietra", PhaseState::StaticSolid,
        2.65f, 0.28f, 2.2f, 0.0f, 0.24f, 0.0f, 0.35f, 0,
        kAmbientTempK, 1473.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        118, 123, 132, 18,
        0.0f, 0.0f, 0.0f
    },
    // Sand
    {
        MaterialId::Sand, "Sabbia", "Sabbia", PhaseState::Powder,
        1.65f, 0.18f, 1.6f, 0.0f, 0.38f, 0.055f, 0.12f, 0,
        kAmbientTempK, 1723.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        218, 188, 114, 24,
        0.0f, 0.0f, 0.0f
    },
    // Water
    {
        MaterialId::Water, "Acqua", "Acqua", PhaseState::Liquid,
        1.00f, 0.30f, 4.18f, 0.10f, 0.90f, 0.08f, 0.05f, 5,
        kAmbientTempK, 0.0f, 273.15f, 373.15f, 0.0f, 0.0f,
        36, 126, 224, 14,
        0.0f, 0.0f, 0.0f
    },
    // SaltWater
    {
        MaterialId::SaltWater, "Acqua Salata", "AcquaS", PhaseState::Liquid,
        1.08f, 0.32f, 4.00f, 0.78f, 0.88f, 0.08f, 0.05f, 5,
        kAmbientTempK, 0.0f, 266.15f, 377.15f, 0.0f, 0.0f,
        54, 152, 218, 14,
        0.0f, 0.0f, 0.0f
    },
    // Salt
    {
        MaterialId::Salt, "Sale", "Sale", PhaseState::Powder,
        1.45f, 0.20f, 1.3f, 0.0f, 0.52f, 0.06f, 0.10f, 0,
        kAmbientTempK, 1074.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        234, 238, 244, 10,
        0.0f, 0.0f, 0.0f
    },
    // Ice
    {
        MaterialId::Ice, "Ghiaccio", "Ghiaccio", PhaseState::StaticSolid,
        0.92f, 0.34f, 2.05f, 0.0f, 0.86f, 0.0f, 0.20f, 0,
        261.15f, 273.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        142, 212, 248, 12,
        0.0f, 0.0f, 0.0f
    },
    // Snow
    {
        MaterialId::Snow, "Neve", "Neve", PhaseState::Powder,
        0.35f, 0.12f, 1.2f, 0.0f, 0.72f, 0.24f, 0.02f, 0,
        266.15f, 273.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        235, 244, 255, 8,
        0.0f, 0.0f, 0.0f
    },
    // Wood
    {
        MaterialId::Wood, "Legno", "Legno", PhaseState::StaticSolid,
        0.72f, 0.10f, 1.4f, 0.0f, 0.22f, 0.0f, 0.25f, 0,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 533.15f,
        128, 82, 46, 18,
        0.0f, 0.0f, 0.0f
    },
    // Plant
    {
        MaterialId::Plant, "Pianta", "Pianta", PhaseState::StaticSolid,
        0.55f, 0.12f, 1.2f, 0.02f, 0.42f, 0.0f, 0.10f, 0,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 493.15f,
        52, 168, 68, 22,
        0.0f, 0.0f, 0.0f
    },
    // Oil
    {
        MaterialId::Oil, "Olio", "Olio", PhaseState::Liquid,
        0.82f, 0.16f, 2.0f, 0.0f, 0.70f, 0.10f, 0.04f, 5,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 543.15f,
        74, 54, 34, 10,
        0.0f, 0.0f, 0.0f
    },
    // Acid
    {
        MaterialId::Acid, "Acido", "Acido", PhaseState::Liquid,
        1.15f, 0.28f, 2.6f, 0.65f, 0.85f, 0.09f, 0.04f, 5,
        kAmbientTempK, 0.0f, 0.0f, 390.15f, 0.0f, 0.0f,
        106, 236, 62, 16,
        0.08f, 0.32f, 0.04f
    },
    // Metal
    {
        MaterialId::Metal, "Metallo", "Metallo", PhaseState::StaticSolid,
        7.80f, 0.92f, 3.2f, 0.96f, 0.20f, 0.0f, 0.55f, 0,
        kAmbientTempK, 1358.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        154, 168, 184, 10,
        0.0f, 0.0f, 0.0f
    },
    // MoltenMetal
    {
        MaterialId::MoltenMetal, "Metallo Fuso", "MetFuso", PhaseState::Liquid,
        7.50f, 0.85f, 3.2f, 0.88f, 0.48f, 0.03f, 0.04f, 3,
        1480.0f, 0.0f, 1223.15f, 0.0f, 0.0f, 0.0f,
        245, 162, 62, 14,
        0.55f, 0.25f, 0.04f
    },
    // Glass
    {
        MaterialId::Glass, "Vetro", "Vetro", PhaseState::StaticSolid,
        2.50f, 0.22f, 1.9f, 0.0f, 0.94f, 0.0f, 0.45f, 0,
        kAmbientTempK, 1723.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        180, 224, 236, 6,
        0.0f, 0.0f, 0.0f
    },
    // MoltenGlass
    {
        MaterialId::MoltenGlass, "Vetro Fuso", "VetFuso", PhaseState::Liquid,
        2.40f, 0.26f, 1.9f, 0.0f, 0.72f, 0.04f, 0.03f, 2,
        1800.0f, 0.0f, 1373.15f, 0.0f, 0.0f, 0.0f,
        255, 176, 86, 12,
        0.48f, 0.22f, 0.05f
    },
    // Lava
    {
        MaterialId::Lava, "Lava", "Lava", PhaseState::Liquid,
        3.10f, 0.42f, 3.0f, 0.08f, 0.42f, 0.04f, 0.02f, 2,
        1520.0f, 0.0f, 1123.15f, 0.0f, 0.0f, 0.0f,
        235, 84, 22, 22,
        0.95f, 0.34f, 0.04f
    },
    // Obsidian
    {
        MaterialId::Obsidian, "Ossidiana", "Ossid.", PhaseState::StaticSolid,
        2.80f, 0.25f, 2.5f, 0.0f, 0.22f, 0.0f, 0.45f, 0,
        kAmbientTempK, 1950.15f, 0.0f, 0.0f, 0.0f, 0.0f,
        46, 36, 64, 12,
        0.0f, 0.0f, 0.0f
    },
    // Gunpowder
    {
        MaterialId::Gunpowder, "Polvere da Sparo", "Polvere", PhaseState::Powder,
        1.10f, 0.14f, 1.1f, 0.0f, 0.36f, 0.09f, 0.08f, 0,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 453.15f,
        66, 66, 72, 14,
        0.0f, 0.0f, 0.0f
    },
    // Nitro
    {
        MaterialId::Nitro, "Nitroglicerina", "Nitro", PhaseState::Liquid,
        1.55f, 0.20f, 1.8f, 0.0f, 0.80f, 0.08f, 0.04f, 4,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 393.15f,
        82, 206, 144, 14,
        0.04f, 0.14f, 0.06f
    },
    // Fire
    {
        MaterialId::Fire, "Fuoco", "Fuoco", PhaseState::Energy,
        0.0004f, 0.35f, 0.45f, 0.15f, 0.90f, 0.35f, 0.0f, 2,
        1180.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        255, 134, 28, 30,
        0.95f, 0.46f, 0.09f
    },
    // Smoke
    {
        MaterialId::Smoke, "Fumo", "Fumo", PhaseState::EmptyGas,
        0.0009f, 0.10f, 0.32f, 0.0f, 0.72f, 0.48f, 0.0f, 3,
        360.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        78, 82, 90, 14,
        0.0f, 0.0f, 0.0f
    },
    // Steam
    {
        MaterialId::Steam, "Vapore", "Vapore", PhaseState::EmptyGas,
        0.0006f, 0.16f, 0.65f, 0.0f, 0.84f, 0.46f, 0.0f, 3,
        395.0f, 0.0f, 0.0f, 0.0f, 365.15f, 0.0f,
        196, 214, 232, 12,
        0.0f, 0.0f, 0.0f
    },
    // Methane
    {
        MaterialId::Methane, "Metano", "Metano", PhaseState::EmptyGas,
        0.00055f, 0.12f, 0.42f, 0.0f, 0.88f, 0.45f, 0.0f, 4,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 493.15f,
        144, 198, 178, 12,
        0.0f, 0.0f, 0.0f
    },
    // Spark
    {
        MaterialId::Spark, "Scintilla", "Scint.", PhaseState::Energy,
        0.0003f, 0.40f, 0.30f, 1.0f, 0.94f, 0.25f, 0.6f, 1,
        1850.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        255, 240, 140, 18,
        1.15f, 0.96f, 0.42f
    },
    // Plasma
    {
        MaterialId::Plasma, "Plasma", "Plasma", PhaseState::Energy,
        0.0002f, 0.55f, 0.50f, 1.0f, 0.95f, 0.32f, 0.2f, 3,
        3800.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        188, 138, 255, 20,
        0.85f, 0.52f, 1.25f
    },
    // Battery
    {
        MaterialId::Battery, "Batteria (4V)", "Batteria", PhaseState::StaticSolid,
        4.50f, 0.40f, 2.80f, 0.98f, 0.25f, 0.0f, 0.40f, 0,
        kAmbientTempK, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        56, 178, 236, 8,
        0.08f, 0.34f, 0.56f
    }
}};

constexpr const MaterialProps& getMaterial(MaterialId id) noexcept {
    const auto idx = static_cast<size_t>(id);
    if (idx >= kMaterialCount) return kMaterialTable[0];
    return kMaterialTable[idx];
}

constexpr const std::array<MaterialProps, kMaterialCount>& getAllMaterials() noexcept {
    return kMaterialTable;
}

constexpr uint8_t toIdx(MaterialId id) noexcept {
    return static_cast<uint8_t>(id);
}

constexpr MaterialId fromIdx(uint8_t idx) noexcept {
    if (idx >= kMaterialCount) return MaterialId::Air;
    return static_cast<MaterialId>(idx);
}

inline void blackbodyGlow(float tempK, float& outR, float& outG, float& outB) noexcept {
    if (tempK < 720.0f) {
        outR = 0.0f;
        outG = 0.0f;
        outB = 0.0f;
        return;
    }

    const float t = std::clamp((tempK - 720.0f) / 3200.0f, 0.0f, 1.5f);
    const float intensity = std::min(1.6f, 0.15f + 1.35f * std::pow(std::min(t, 1.2f), 0.85f));

    if (tempK < 1200.0f) {
        const float u = (tempK - 720.0f) / 480.0f;
        outR = intensity * std::clamp(0.35f + 0.65f * u, 0.0f, 1.0f);
        outG = intensity * std::clamp(0.28f * u * u, 0.0f, 1.0f);
        outB = 0.0f;
    } else if (tempK < 2000.0f) {
        const float u = (tempK - 1200.0f) / 800.0f;
        outR = intensity;
        outG = intensity * std::clamp(0.28f + 0.58f * u, 0.0f, 1.0f);
        outB = intensity * std::clamp(0.32f * u * u, 0.0f, 1.0f);
    } else if (tempK < 3200.0f) {
        const float u = (tempK - 2000.0f) / 1200.0f;
        outR = intensity;
        outG = intensity * std::clamp(0.86f + 0.14f * u, 0.0f, 1.0f);
        outB = intensity * std::clamp(0.32f + 0.60f * u, 0.0f, 1.0f);
    } else {
        const float u = std::clamp((tempK - 3200.0f) / 1800.0f, 0.0f, 1.0f);
        outR = intensity * (1.0f - 0.14f * u);
        outG = intensity * (1.0f - 0.04f * u);
        outB = intensity;
    }
}

void infernoColormap(float t01, uint8_t& outR, uint8_t& outG, uint8_t& outB) noexcept;

} // namespace sim
