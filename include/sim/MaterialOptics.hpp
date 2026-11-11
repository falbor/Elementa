#pragma once

#include "sim/Material.hpp"

#include <array>
#include <cstddef>

namespace sim {

struct MaterialOptics {
    MaterialId id;
    float ior;              // Index of refraction (1.00 = air/opaque, 1.31 Ice, 1.33 Water, 1.52 Glass)
    float sigmaR;           // Beer-Lambert red absorption coefficient per simulation pixel
    float sigmaG;           // Beer-Lambert green absorption coefficient per simulation pixel
    float sigmaB;           // Beer-Lambert blue absorption coefficient per simulation pixel
    float shadowOpacity;    // [0..1] shadow ray extinction per simulation pixel
    float translucency;     // [0..1] optical transparency / refraction blend weight
    float roughness;        // [0.05..1.0] surface microfacet roughness
    float metallic;         // [0..1] metallic specular tint weight
    float specularStrength; // [0..2] specular highlight multiplier
    float refractStrength;  // [0..1] normal distortion strength for refraction / heat-haze
    float detailScale;      // Procedural surface detail frequency scale
};

// clang-format off
inline constexpr std::array<MaterialOptics, kMaterialCount> kMaterialOpticsTable = {{
    // id,                   ior,    sigmaR, sigmaG, sigmaB, shadow, transl, rough, metal,  spec,   refract, detail
    {MaterialId::Air,         1.000f, 0.000f, 0.000f, 0.000f, 0.00f, 1.00f, 1.00f, 0.00f, 0.00f, 0.00f, 1.00f},
    {MaterialId::Bedrock,     1.000f, 2.000f, 2.000f, 2.000f, 1.00f, 0.00f, 0.72f, 0.20f, 0.28f, 0.00f, 1.00f},
    {MaterialId::Stone,       1.000f, 2.000f, 2.000f, 2.000f, 0.98f, 0.00f, 0.88f, 0.04f, 0.14f, 0.00f, 1.00f},
    {MaterialId::Sand,        1.000f, 1.600f, 1.600f, 1.600f, 0.85f, 0.00f, 0.92f, 0.00f, 0.10f, 0.00f, 1.25f},
    {MaterialId::Water,       1.333f, 0.135f, 0.042f, 0.016f, 0.06f, 0.95f, 0.08f, 0.00f, 1.25f, 0.85f, 1.00f},
    {MaterialId::SaltWater,   1.345f, 0.115f, 0.036f, 0.018f, 0.07f, 0.92f, 0.10f, 0.00f, 1.20f, 0.88f, 1.10f},
    {MaterialId::Salt,        1.540f, 0.800f, 0.800f, 0.800f, 0.72f, 0.12f, 0.52f, 0.00f, 0.55f, 0.08f, 1.40f},
    {MaterialId::Ice,         1.310f, 0.048f, 0.018f, 0.008f, 0.12f, 0.82f, 0.18f, 0.00f, 1.15f, 0.72f, 1.00f},
    {MaterialId::Snow,        1.150f, 0.420f, 0.360f, 0.300f, 0.48f, 0.22f, 0.86f, 0.00f, 0.45f, 0.05f, 1.35f},
    {MaterialId::Wood,        1.000f, 2.000f, 2.000f, 2.000f, 0.95f, 0.00f, 0.82f, 0.00f, 0.12f, 0.00f, 1.00f},
    {MaterialId::Plant,       1.000f, 1.200f, 0.900f, 1.300f, 0.68f, 0.12f, 0.76f, 0.00f, 0.18f, 0.00f, 1.15f},
    {MaterialId::Oil,         1.470f, 0.170f, 0.230f, 0.340f, 0.28f, 0.58f, 0.12f, 0.00f, 1.10f, 0.65f, 0.90f},
    {MaterialId::Acid,        1.360f, 0.085f, 0.018f, 0.145f, 0.10f, 0.88f, 0.14f, 0.00f, 1.05f, 0.78f, 1.20f},
    {MaterialId::Metal,       1.000f, 2.000f, 2.000f, 2.000f, 1.00f, 0.00f, 0.24f, 0.92f, 1.45f, 0.00f, 1.00f},
    {MaterialId::MoltenMetal, 1.000f, 1.600f, 1.800f, 2.000f, 0.65f, 0.00f, 0.28f, 0.85f, 0.95f, 0.15f, 1.00f},
    {MaterialId::Glass,       1.520f, 0.014f, 0.006f, 0.005f, 0.04f, 0.96f, 0.06f, 0.00f, 1.55f, 0.98f, 1.00f},
    {MaterialId::MoltenGlass, 1.500f, 0.090f, 0.140f, 0.240f, 0.25f, 0.45f, 0.16f, 0.00f, 1.15f, 0.75f, 1.00f},
    {MaterialId::Lava,        1.000f, 1.500f, 1.900f, 2.000f, 0.75f, 0.00f, 0.62f, 0.08f, 0.35f, 0.10f, 1.00f},
    {MaterialId::Obsidian,    1.490f, 1.800f, 1.900f, 1.600f, 0.98f, 0.00f, 0.16f, 0.12f, 1.35f, 0.00f, 1.10f},
    {MaterialId::Gunpowder,   1.000f, 1.900f, 1.900f, 1.900f, 0.88f, 0.00f, 0.95f, 0.00f, 0.06f, 0.00f, 1.40f},
    {MaterialId::Nitro,       1.470f, 0.055f, 0.020f, 0.045f, 0.12f, 0.86f, 0.10f, 0.00f, 1.15f, 0.80f, 1.00f},
    {MaterialId::Fire,        1.000f, 0.020f, 0.040f, 0.080f, 0.00f, 0.78f, 0.90f, 0.00f, 0.00f, 0.35f, 1.20f},
    {MaterialId::Smoke,       1.000f, 0.240f, 0.240f, 0.240f, 0.16f, 0.68f, 0.95f, 0.00f, 0.00f, 0.12f, 1.00f},
    {MaterialId::Steam,       1.010f, 0.080f, 0.080f, 0.070f, 0.06f, 0.82f, 0.92f, 0.00f, 0.00f, 0.22f, 1.00f},
    {MaterialId::Methane,     1.020f, 0.050f, 0.025f, 0.035f, 0.04f, 0.88f, 0.92f, 0.00f, 0.00f, 0.18f, 1.00f},
    {MaterialId::Spark,       1.000f, 0.000f, 0.000f, 0.000f, 0.00f, 0.75f, 0.50f, 0.00f, 0.00f, 0.10f, 1.00f},
    {MaterialId::Plasma,      1.000f, 0.000f, 0.000f, 0.000f, 0.00f, 0.72f, 0.50f, 0.00f, 0.00f, 0.45f, 1.20f},
    {MaterialId::Battery,     1.000f, 2.000f, 2.000f, 2.000f, 0.98f, 0.00f, 0.34f, 0.70f, 1.05f, 0.00f, 1.00f},
}};
// clang-format on

constexpr bool validateMaterialOpticsTable() noexcept {
    for (size_t i = 0; i < kMaterialCount; ++i) {
        if (static_cast<size_t>(kMaterialOpticsTable[i].id) != i) return false;
    }
    return true;
}
static_assert(validateMaterialOpticsTable(), "kMaterialOpticsTable must match MaterialId enum order");

constexpr const MaterialOptics &getMaterialOptics(MaterialId id) noexcept {
    const auto idx = static_cast<size_t>(id);
    if (idx >= kMaterialCount) return kMaterialOpticsTable[0];
    return kMaterialOpticsTable[idx];
}

constexpr const std::array<MaterialOptics, kMaterialCount> &getAllMaterialOptics() noexcept {
    return kMaterialOpticsTable;
}

} // namespace sim
