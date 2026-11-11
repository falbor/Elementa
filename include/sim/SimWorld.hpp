#pragma once

#include "sim/Material.hpp"
#include "sim/ThreadPool.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

enum class ViewMode : uint8_t {
    Physical = 0, // Material color + blackbody thermal glow + dynamic lighting
    Thermal,      // Scientific Inferno temperature map
    Pressure,     // Fluid pressure & shockwave visualization
    Velocity,     // Air & particle velocity vector magnitude/direction
    EnergyLight   // Radiative illumination & electrical charge field
};

enum class GravityMode : uint8_t {
    Earth = 0,   // Standard downward gravity
    ZeroG,       // Microgravity (fluid & thermal convection + inertia)
    Inverted,    // Upward gravity
    RadialCenter // Gravity pulled toward grid center
};

enum class BoundaryMode : uint8_t {
    Closed = 0, // Solid domain walls
    Open        // Particles leave bounds, pressure = 0 at edges
};

enum class ToolMode : uint8_t {
    Material = 0, // Paint selected MaterialId
    Wind,         // Inject fluid velocity along stroke
    Heat,         // Laser / plasma torch thermal injection
    Cryo,         // Cryogenic cooling
    SparkTool,    // High-voltage electrical pulse
    Attractor,    // Pull particles & air toward cursor
    Repulsor,     // Push particles & air away from cursor
    Erase         // Replace with Air
};

enum class PresetId : uint8_t {
    Laboratory = 0, // Multi-chamber showcase: lava/water steam generator, acid tank, gunpowder fuse, sand hourglass
    Volcano,        // Active magma chamber, stone crater, snow cap, cooling obsidian & steam plumes
    Hydrodynamics,  // Communicating vessels, oil/water separation, vortex obstacles & salt desalination
    Explosives,     // Reinforced stone chambers with gunpowder trails, nitro, methane pocket & electrical igniter
    ElectricGrid,   // Copper conductors, water electrolysis bath, heating coil & spark gaps
    ZeroGOrb        // Microgravity glowing plasma/water/oil/sand orbital chaos
};

struct PixelInfo {
    int x = 0;
    int y = 0;
    MaterialId material = MaterialId::Air;
    float tempK = kAmbientTempK;
    float pressure = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float airVx = 0.0f;
    float airVy = 0.0f;
    float charge = 0.0f;
    float lightR = 0.0f;
    float lightG = 0.0f;
    float lightB = 0.0f;
    float life = 0.0f;
};

struct SimStats {
    uint64_t tick = 0;
    uint32_t activeParticles = 0;
    double totalThermalEnergy = 0.0;
    double totalKineticEnergy = 0.0;
    float minTempK = kAmbientTempK;
    float maxTempK = kAmbientTempK;
    float maxPressure = 0.0f;

    double totalStepMs = 0.0;
    double fluidMs = 0.0;
    double thermoMs = 0.0;
    double reactionMs = 0.0;
    double particleMs = 0.0;
    double lightMs = 0.0;
    double renderMs = 0.0;
};

class SimWorld {
public:
    SimWorld(int width, int height, unsigned threads = 0, uint32_t seed = 0x5EED1234u);

    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }
    int pixelCount() const noexcept { return m_count; }

    // Advance physics simulation by exactly 1 tick (dt = 1.0 unit step)
    void step();

    // Render all W*H pixels into internal BGRA8888 buffer (0xAARRGGBB little-endian BGRA)
    void render(ViewMode mode);

    const uint32_t* framebuffer() const noexcept { return m_framebuffer.data(); }
    const SimStats& stats() const noexcept { return m_stats; }

    // Configuration
    void setGravityMode(GravityMode mode) noexcept { m_gravityMode = mode; }
    GravityMode gravityMode() const noexcept { return m_gravityMode; }

    void setBoundaryMode(BoundaryMode mode) noexcept { m_boundaryMode = mode; }
    BoundaryMode boundaryMode() const noexcept { return m_boundaryMode; }

    void setAmbientTempK(float tempK) noexcept { m_ambientTempK = tempK; }
    float ambientTempK() const noexcept { return m_ambientTempK; }

    // Clear and presets
    void clear(MaterialId fill = MaterialId::Air);
    void loadPreset(PresetId preset);

    // Direct pixel & brush editing
    void setCell(int x, int y, MaterialId mat, bool resetVelocity = true);
    void setCellTemp(int x, int y, float tempK);
    void setCellVelocity(int x, int y, float vx, float vy);
    void setCellCharge(int x, int y, float charge);

    void paintCircle(int cx, int cy, int radius, MaterialId mat, bool overwriteSolid = true);
    void paintLine(int x0, int y0, int x1, int y1, int radius, MaterialId mat, bool overwriteSolid = true);

    void applyToolCircle(ToolMode tool, int cx, int cy, int radius, float dx, float dy, MaterialId paintMat);
    void applyToolLine(ToolMode tool, int x0, int y0, int x1, int y1, int radius, MaterialId paintMat);

    // Inject radial pressure/heat/spark explosion at (cx, cy)
    void detonate(int cx, int cy, int radius, float blastStrength, float peakTempK);

    // Inspection & counts
    bool inBounds(int x, int y) const noexcept {
        return x >= 0 && x < m_width && y >= 0 && y < m_height;
    }

    int index(int x, int y) const noexcept { return y * m_width + x; }

    PixelInfo inspect(int x, int y) const noexcept;
    uint32_t countMaterial(MaterialId mat) const noexcept;
    double computeTotalThermalEnergy() const noexcept;

    // Export current rendered framebuffer as binary PPM (P6)
    bool savePPM(const std::string& path, ViewMode mode = ViewMode::Physical);

private:
    // Simulation sub-passes (every pixel computed)
    void stepFluidField();
    void stepThermoElectric();
    void stepReactions();
    void stepParticles();
    void stepLighting();
    void computeStats();

    // Particle kinematics helpers
    void updateParticleAt(int x, int y, uint8_t tickParity);
    bool canDisplace(MaterialId mover, MaterialId target) const noexcept;
    void moveParticle(int srcIdx, int dstIdx) noexcept;
    void swapParticles(int aIdx, int bIdx) noexcept;
    void convertCell(int idx, MaterialId newMat, bool keepTemp = true) noexcept;
    void computeGravityAt(int x, int y, float& gx, float& gy) const noexcept;

    // Fast deterministic PRNG
    uint32_t nextRand() noexcept;
    uint32_t hashCoord(int x, int y, uint64_t tick) const noexcept;

    // Friend-like internal helpers implemented across modular .cpp files
    int m_width;
    int m_height;
    int m_count;
    ThreadPool m_pool;

    GravityMode m_gravityMode = GravityMode::Earth;
    BoundaryMode m_boundaryMode = BoundaryMode::Closed;
    float m_ambientTempK = kAmbientTempK;

    uint32_t m_rngState;
    uint8_t m_tickParity = 1;

    // --- Per-pixel particle state (SOA) ---
    std::vector<uint8_t> m_mat;
    std::vector<float> m_vx;
    std::vector<float> m_vy;
    std::vector<float> m_fx;
    std::vector<float> m_fy;
    std::vector<float> m_life;
    std::vector<float> m_charge;
    std::vector<uint8_t> m_colorSeed;
    std::vector<uint8_t> m_movedTick;
    std::vector<uint8_t> m_solidMask;

    // --- Per-pixel Eulerian continuous fields ---
    std::vector<float> m_airVx;
    std::vector<float> m_airVy;
    std::vector<float> m_airVxTmp;
    std::vector<float> m_airVyTmp;
    std::vector<float> m_pressure;
    std::vector<float> m_div;
    std::vector<float> m_blastDiv; // Added divergence from explosions/boiling

    std::vector<float> m_tempK;
    std::vector<float> m_tempTmp;
    std::vector<float> m_chargeTmp;

    // --- Per-pixel radiative light field ---
    std::vector<float> m_lightR;
    std::vector<float> m_lightG;
    std::vector<float> m_lightB;
    std::vector<float> m_lightTmpR;
    std::vector<float> m_lightTmpG;
    std::vector<float> m_lightTmpB;

    // --- Rendered RGBA (BGRA8888) output ---
    std::vector<uint32_t> m_framebuffer;

    SimStats m_stats{};
};

} // namespace sim
