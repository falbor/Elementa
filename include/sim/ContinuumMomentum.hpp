#pragma once

#include "sim/ThreadPool.hpp"
#include <cstdint>
#include <functional>
#include <utility>
#include <span>
#include <vector>

namespace sim {

// Staggered velocity advection and implicit, material-dependent viscosity.
// Forces and pressure projection follow this stage.
class ContinuumMomentum {
public:
    using Backend = std::function<bool(std::span<float>, std::span<float>, std::span<const float>,
                                       std::span<const uint8_t>, float, bool)>;
    ContinuumMomentum(int width, int height, ThreadPool *pool = nullptr);
    void setBackend(Backend backend) { m_backend = std::move(backend); }
    void advance(std::span<float> u, std::span<float> v, std::span<const float> viscosity,
                 std::span<const uint8_t> solid, float dt, bool closed);

private:
    int m_width, m_height;
    ThreadPool *m_pool;
    Backend m_backend;
    std::vector<float> m_u, m_v, m_rhsU, m_rhsV, m_tmpU, m_tmpV;
    std::vector<uint8_t> m_blockU, m_blockV;
    std::vector<float> m_nuU, m_nuV;
    struct DiffusionStencil {
        int i, neighbor[4];
        float coefficient[4], inverseDiagonal;
    };
    std::vector<DiffusionStencil> m_diffusion;
};

} // namespace sim
