#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace sim::app {

// Optional OpenGL 3.3 liquid momentum backend. All calls require its owning
// context to be current. Numerical output is read back before returning.
class GlMomentum {
public:
    GlMomentum();
    ~GlMomentum();
    bool initialize(int width, int height);
    bool isReady() const noexcept;
    void shutdown();
    bool advance(std::span<float> u, std::span<float> v, std::span<const float> viscosity,
                 std::span<const uint8_t> solid, float dt, bool closed);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Hidden-context hardware validation and synchronous CPU/GPU timing.
int validateGpuMomentum(bool integratedWorlds = false);

} // namespace sim::app
