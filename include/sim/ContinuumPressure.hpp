#pragma once

#include "sim/ThreadPool.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace sim {

struct ProjectionResult {
    int iterations = 0;
    int components = 0, coarseNodes = 0;
    int parallelComponents = 0;
    double divergenceBefore = 0.0;
    double divergenceAfter = 0.0;
    bool converged = false;
};

// Staggered (MAC) pressure projection. Liquid volume > 0 marks the fluid
// domain; zero-volume cells are atmosphere with zero gauge pressure. Passing
// volume=1 everywhere instead projects the entire gas/air domain.
// An optional signed-distance field enables a subcell ghost-fluid surface:
// only negative-distance sample centers are pressure unknowns, and the
// atmospheric boundary is interpolated along each wet/dry face connection.
class ContinuumPressure {
public:
    ContinuumPressure(int width, int height, ThreadPool *pool = nullptr);
    ProjectionResult project(std::span<float> u, std::span<float> v, std::span<const float> volume,
                             std::span<const float> density, std::span<const uint8_t> solid, float dt, bool closed,
                             int maxIterations = 300, float tolerance = 1e-5f,
                             std::span<const float> signedDistance = {});
    std::span<const double> pressure() const noexcept { return m_pressure; }

private:
    struct Stencil {
        int i, left, right, up, down;
        float diagonal, l, r, u, d;
    };
    std::vector<Stencil> m_stencils;
    std::vector<int> m_stencilIndex;
    std::vector<int> m_componentIndex, m_coarseCells;
    struct CoarseWeights {
        int node[4];
        double weight[4];
    };
    std::vector<CoarseWeights> m_coarseWeights;
    std::vector<double> m_coarseFactor, m_coarseRhs, m_coarseSolution;
    int m_coarseCount = 0;
    int m_width, m_height;
    ThreadPool *m_pool;
    std::vector<double> m_reduction;
    std::vector<float> m_forward, m_preconditioned, m_inverseDiagonal;
    std::vector<double> m_pressure;
    std::vector<float> m_diag, m_rhs, m_residual, m_direction, m_product;
    std::vector<float> m_faceX, m_faceY;
};

} // namespace sim
