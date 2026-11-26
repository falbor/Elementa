#include "sim/ContinuumPressure.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace sim {

ContinuumPressure::ContinuumPressure(int width, int height, ThreadPool *pool)
    : m_width(width), m_height(height), m_pool(pool) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        throw std::invalid_argument("Invalid pressure grid dimensions");
    const size_t n = static_cast<size_t>(width) * height;
    m_stencilIndex.resize(n);
    m_componentIndex.resize(n);
    m_coarseWeights.resize(n);
    m_pressure.resize(n);
    for (auto *field : {&m_diag, &m_rhs, &m_residual, &m_direction, &m_product})
        field->resize(n);
    m_forward.resize(n);
    m_preconditioned.resize(n);
    m_inverseDiagonal.resize(n);
    m_faceX.resize(static_cast<size_t>(width + 1) * height);
    m_faceY.resize(static_cast<size_t>(height + 1) * width);
}

ProjectionResult ContinuumPressure::project(std::span<float> u, std::span<float> v, std::span<const float> volume,
                                            std::span<const float> density, std::span<const uint8_t> solid, float dt,
                                            bool closed, int maxIterations, float tolerance,
                                            std::span<const float> signedDistance) {
    const int W = m_width, H = m_height;
    const size_t n = m_pressure.size();
    if (u.size() != m_faceX.size() || v.size() != m_faceY.size() || volume.size() != n || density.size() != n ||
        solid.size() != n || !std::isfinite(dt) || dt <= 0.0f || maxIterations < 1 || !std::isfinite(tolerance) ||
        tolerance <= 0.0f || (!signedDistance.empty() && signedDistance.size() != n))
        throw std::invalid_argument("Invalid pressure fields or timestep");
    for (size_t i = 0; i < n; ++i) {
        if (!signedDistance.empty() && !std::isfinite(signedDistance[i]))
            throw std::invalid_argument("Invalid free-surface distance");
        if (!std::isfinite(volume[i]) || volume[i] < 0.0f || !std::isfinite(density[i]) || density[i] <= 0.0f) {
            std::ostringstream message;
            message << "Invalid pressure state at cell " << i << ": volume=" << volume[i] << " density=" << density[i];
            throw std::invalid_argument(message.str());
        }
    }
    for (auto face : {u, v})
        for (float speed : face)
            if (!std::isfinite(speed)) throw std::invalid_argument("Invalid pressure face velocity");
    auto active = [&](int i) {
        return i >= 0 && !solid[i] && volume[i] > 1e-6f && (signedDistance.empty() || signedDistance[i] < 0.0f);
    };
    // Ghost-fluid Dirichlet boundary: pressure vanishes at the reconstructed
    // interface, not at the center of the next dry cell. Apply the same face
    // coefficient in the matrix and velocity correction to retain symmetry.
    auto surfaceCoefficient = [&](int wet, int dry) {
        if (signedDistance.empty()) return 1.0f / density[wet];
        if (dry < 0) return 2.0f / density[wet]; // Open domain boundary lies at the face.
        const float inside = -signedDistance[wet];
        const float outside = std::max(0.0f, signedDistance[dry]);
        const float theta = std::clamp(inside / (inside + outside), 0.01f, 1.0f);
        return 1.0f / (density[wet] * theta);
    };
    auto coefficient = [&](int a, int b) {
        if ((a < 0 || b < 0) && closed) return 0.0f;
        if ((a >= 0 && solid[a]) || (b >= 0 && solid[b])) return 0.0f;
        const bool aa = active(a), ab = active(b);
        if (aa && ab) return 2.0f / (density[a] + density[b]);
        if (aa) return surfaceCoefficient(a, b);
        if (ab) return surfaceCoefficient(b, a);
        return 0.0f;
    };
    auto blocked = [&](int a, int b) {
        return ((a < 0 || b < 0) && closed) || (a >= 0 && solid[a]) || (b >= 0 && solid[b]);
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x <= W; ++x) {
            const int a = x > 0 ? y * W + x - 1 : -1, b = x < W ? y * W + x : -1;
            const int f = y * (W + 1) + x;
            m_faceX[f] = coefficient(a, b);
            if (blocked(a, b)) u[f] = 0.0f;
        }
    }
    for (int y = 0; y <= H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int a = y > 0 ? (y - 1) * W + x : -1, b = y < H ? y * W + x : -1;
            const int f = y * W + x;
            m_faceY[f] = coefficient(a, b);
            if (blocked(a, b)) v[f] = 0.0f;
        }
    }
    auto divergence = [&](int x, int y) {
        return u[y * (W + 1) + x + 1] - u[y * (W + 1) + x] + v[(y + 1) * W + x] - v[y * W + x];
    };
    ProjectionResult result;
    int count = 0;
    float minimumDensity = std::numeric_limits<float>::max(), maximumDensity = 0.0f;
    double norm = 0.0;
    std::fill(m_pressure.begin(), m_pressure.end(), 0.0);
    m_stencils.clear();
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            m_diag[i] = active(i) ? m_faceX[y * (W + 1) + x] + m_faceX[y * (W + 1) + x + 1] + m_faceY[y * W + x] +
                                        m_faceY[(y + 1) * W + x]
                                  : 0.0f;
            const float d = active(i) ? divergence(x, y) : 0.0f;
            if (active(i)) {
                minimumDensity = std::min(minimumDensity, density[i]);
                maximumDensity = std::max(maximumDensity, density[i]);
                result.divergenceBefore += static_cast<double>(d) * d;
                ++count;
            }
            m_rhs[i] = m_diag[i] > 0.0f ? -d / dt : 0.0f;
            if (m_diag[i] == 0.0f) m_pressure[i] = 0.0f;
            m_residual[i] = m_direction[i] = m_rhs[i];
            norm += static_cast<double>(m_rhs[i]) * m_rhs[i];
            if (m_diag[i] > 0.0f) {
                m_stencils.push_back({i, x > 0 ? i - 1 : 0, x + 1 < W ? i + 1 : 0, y > 0 ? i - W : 0,
                                      y + 1 < H ? i + W : 0, m_diag[i], x > 0 ? m_faceX[y * (W + 1) + x] : 0.0f,
                                      x + 1 < W ? m_faceX[y * (W + 1) + x + 1] : 0.0f,
                                      y > 0 ? m_faceY[y * W + x] : 0.0f, y + 1 < H ? m_faceY[(y + 1) * W + x] : 0.0f});
            }
        }
    }
    // A completely enclosed component has a Neumann nullspace. Remove its
    // tiny floating-point RHS imbalance and pin one pressure reference, rather
    // than allowing CG to amplify the constant mode on large sealed tanks.
    std::vector<uint8_t> visited(n, 0), pinned(n, 0);
    std::vector<int> component;
    std::vector<int> componentSizes;
    std::fill(m_componentIndex.begin(), m_componentIndex.end(), -1);
    for (const auto &seed : m_stencils) {
        if (visited[seed.i]) continue;
        const int componentId = result.components++;
        component.clear();
        component.push_back(seed.i);
        visited[seed.i] = 1;
        bool grounded = false;
        double sum = 0.0;
        for (size_t k = 0; k < component.size(); ++k) {
            const int i = component[k], x = i % W, y = i / W;
            m_componentIndex[i] = componentId;
            sum += m_rhs[i];
            auto neighbor = [&](int j, float coefficient) {
                if (coefficient <= 0.0f) return;
                if (m_diag[j] == 0.0f) {
                    grounded = true;
                    return;
                }
                if (!visited[j]) {
                    visited[j] = 1;
                    component.push_back(j);
                }
            };
            if (x > 0)
                neighbor(i - 1, m_faceX[y * (W + 1) + x]);
            else if (!closed)
                grounded = true;
            if (x + 1 < W)
                neighbor(i + 1, m_faceX[y * (W + 1) + x + 1]);
            else if (!closed)
                grounded = true;
            if (y > 0)
                neighbor(i - W, m_faceY[y * W + x]);
            else if (!closed)
                grounded = true;
            if (y + 1 < H)
                neighbor(i + W, m_faceY[(y + 1) * W + x]);
            else if (!closed)
                grounded = true;
        }
        componentSizes.push_back(static_cast<int>(component.size()));
        if (!grounded) {
            const float mean = static_cast<float>(sum / component.size());
            for (int i : component)
                m_rhs[i] -= mean;
            pinned[component.front()] = 1;
            m_rhs[component.front()] = 0.0f;
        }
    }
    norm = 0.0;
    for (auto &s : m_stencils) {
        if (pinned[s.i]) {
            m_pressure[s.i] = 0.0;
            s.diagonal = 1.0f;
            s.l = s.r = s.u = s.d = 0.0f;
        } else {
            if (pinned[s.left]) s.l = 0.0f;
            if (pinned[s.right]) s.r = 0.0f;
            if (pinned[s.up]) s.u = 0.0f;
            if (pinned[s.down]) s.d = 0.0f;
        }
        m_direction[s.i] = m_residual[s.i] = m_rhs[s.i];
        norm += static_cast<double>(m_rhs[s.i]) * m_rhs[s.i];
    }
    const double target = std::max(1e-24, norm * tolerance * tolerance);
    m_reduction.resize(m_stencils.size());
    auto eachStencil = [&](auto &&fn) {
        if (m_pool && m_stencils.size() > 200000)
            m_pool->parallelFor(0, static_cast<int>(m_stencils.size()), fn);
        else
            fn(0, static_cast<int>(m_stencils.size()));
    };
    // Modified incomplete Cholesky MIC(0), with the dropped off-diagonal
    // contributions added to the diagonal (Bridson, Fluid Simulation Notes).
    // The pressure tolerance and the projected matrix are unchanged.
    std::fill(m_forward.begin(), m_forward.end(), 0.0f);
    std::fill(m_preconditioned.begin(), m_preconditioned.end(), 0.0f);
    std::fill(m_inverseDiagonal.begin(), m_inverseDiagonal.end(), 0.0f);
    std::fill(m_stencilIndex.begin(), m_stencilIndex.end(), -1);
    for (size_t k = 0; k < m_stencils.size(); ++k)
        m_stencilIndex[m_stencils[k].i] = static_cast<int>(k);
    constexpr float tau = 0.97f;
    const bool highContrast = maximumDensity > 4.0f * minimumDensity && m_stencils.size() >= 4096;
    for (const auto &s : m_stencils) {
        if (highContrast) {
            m_inverseDiagonal[s.i] = 1.7f / s.diagonal;
            continue;
        }
        const float left = s.l * m_inverseDiagonal[s.left], up = s.u * m_inverseDiagonal[s.up];
        const int li = m_stencilIndex[s.left], ui = m_stencilIndex[s.up];
        const float crossL = li >= 0 ? m_stencils[li].d : 0.0f;
        const float crossU = ui >= 0 ? m_stencils[ui].r : 0.0f;
        float e = s.diagonal - left * left - up * up -
                  tau * (s.l * crossL * m_inverseDiagonal[s.left] * m_inverseDiagonal[s.left] +
                         s.u * crossU * m_inverseDiagonal[s.up] * m_inverseDiagonal[s.up]);
        if (e < 0.25f * s.diagonal) e = s.diagonal;
        m_inverseDiagonal[s.i] = 1.0f / std::sqrt(e);
    }
    // Exact Galerkin solve on a small bilinear coarse space. The basis spans
    // smooth pressure modes across an entire tank; MIC resolves local error.
    // Ac = P^T A P, so adding this correction preserves PCG symmetry.
    m_coarseCount = 0;
    const int sizeableComponents = static_cast<int>(std::count_if(componentSizes.begin(), componentSizes.end(),
                                                                 [](int size) { return size >= 1024; }));
    // Keep the coarse solve for one dominant tank; tiny satellite droplets
    // do not provide enough parallel work to compensate for losing it.
    const bool islandSolve = m_pool && m_pool->totalThreads() > 1 && sizeableComponents >= 2 && m_stencils.size() >= 4096;
    m_coarseCells.clear();
    std::vector<int> eligible;
    for (int c = 0; c < result.components; ++c)
        if (componentSizes[c] >= 1024) eligible.push_back(c);
    std::sort(eligible.begin(), eligible.end(), [&](int a, int b) { return componentSizes[a] > componentSizes[b]; });
    if (eligible.size() > 16) eligible.resize(16);
    std::vector<int> componentRank(result.components, -1);
    for (size_t k = 0; k < eligible.size(); ++k)
        componentRank[eligible[k]] = static_cast<int>(k);
    for (const auto &s : m_stencils)
        if (componentRank[m_componentIndex[s.i]] >= 0) m_coarseCells.push_back(s.i);
    if (!islandSolve && !highContrast && m_stencils.size() >= 4096 && !m_coarseCells.empty()) {
        for (int blockSize = 16;; blockSize *= 2) {
            const int cw = (W - 1 + blockSize - 1) / blockSize + 1;
            const int ch = (H - 1 + blockSize - 1) / blockSize + 1;
            std::vector<int> nodes(cw * ch * eligible.size(), -1);
            m_coarseCount = 0;
            for (int i : m_coarseCells) {
                const int x = i % W, y = i / W;
                const double fx = static_cast<double>(x % blockSize) / blockSize;
                const double fy = static_cast<double>(y % blockSize) / blockSize;
                auto &row = m_coarseWeights[i];
                for (int k = 0; k < 4; ++k) {
                    const int dx = k & 1, dy = k >> 1;
                    row.weight[k] = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy);
                    row.node[k] = 0;
                    if (row.weight[k] == 0.0) continue;
                    const int node =
                        componentRank[m_componentIndex[i]] * cw * ch + (y / blockSize + dy) * cw + x / blockSize + dx;
                    if (nodes[node] < 0) nodes[node] = m_coarseCount++;
                    row.node[k] = nodes[node];
                }
            }
            if (m_coarseCount <= 128) break;
        }
        const int C = m_coarseCount;
        m_coarseFactor.assign(static_cast<size_t>(C) * C, 0.0);
        auto energy = [&](int i, int j, double coefficient) {
            if (coefficient == 0.0) return;
            int node[8], count = 0;
            double weight[8]{};
            auto add = [&](int cell, double sign) {
                const auto &row = m_coarseWeights[cell];
                for (int k = 0; k < 4; ++k) {
                    if (row.weight[k] == 0.0) continue;
                    int p = 0;
                    while (p < count && node[p] != row.node[k])
                        ++p;
                    if (p == count) node[count++] = row.node[k];
                    weight[p] += sign * row.weight[k];
                }
            };
            add(i, 1.0);
            if (j >= 0) add(j, -1.0);
            for (int p = 0; p < count; ++p)
                for (int q = 0; q < count; ++q)
                    m_coarseFactor[node[p] * C + node[q]] += coefficient * weight[p] * weight[q];
        };
        for (int i : m_coarseCells) {
            const auto &s = m_stencils[m_stencilIndex[i]];
            energy(s.i, -1, s.diagonal - (s.l + s.r + s.u + s.d));
            auto face = [&](int j, float coefficient) {
                if (m_stencilIndex[j] < 0)
                    energy(s.i, -1, coefficient);
                else if (s.i < j)
                    energy(s.i, j, coefficient);
            };
            face(s.left, s.l);
            face(s.right, s.r);
            face(s.up, s.u);
            face(s.down, s.d);
        }
        double diagonalSum = 0.0;
        for (int i = 0; i < C; ++i)
            diagonalSum += m_coarseFactor[i * C + i];
        // Thin fluid regions can make coarse basis columns dependent. A tiny
        // regularization affects only the preconditioner, never the fine solve.
        const double regularization = diagonalSum / C * 1e-8;
        for (int i = 0; i < C; ++i)
            m_coarseFactor[i * C + i] += regularization;
        for (int i = 0; i < C && m_coarseCount != 0; ++i)
            for (int j = 0; j <= i; ++j) {
                double value = m_coarseFactor[i * C + j];
                for (int k = 0; k < j; ++k)
                    value -= m_coarseFactor[i * C + k] * m_coarseFactor[j * C + k];
                if (i == j) {
                    if (!(value > diagonalSum / C * 1e-6)) {
                        m_coarseCount = 0;
                        break;
                    }
                    m_coarseFactor[i * C + j] = std::sqrt(value);
                } else
                    m_coarseFactor[i * C + j] = value / m_coarseFactor[j * C + j];
            }
        m_coarseRhs.resize(C);
        m_coarseSolution.resize(C);
    }
    auto precondition = [&]() {
        if (highContrast) {
            for (const auto &s : m_stencils)
                m_forward[s.i] =
                    (m_residual[s.i] + s.l * m_forward[s.left] + s.u * m_forward[s.up]) * m_inverseDiagonal[s.i];
            double value = 0.0;
            for (auto it = m_stencils.rbegin(); it != m_stencils.rend(); ++it) {
                const auto &s = *it;
                m_preconditioned[s.i] =
                    (s.diagonal * m_forward[s.i] + s.r * m_preconditioned[s.right] + s.d * m_preconditioned[s.down]) *
                    m_inverseDiagonal[s.i];
                value += static_cast<double>(m_residual[s.i]) * m_preconditioned[s.i];
            }
            return value;
        }
        for (const auto &s : m_stencils)
            m_forward[s.i] = (m_residual[s.i] + s.l * m_inverseDiagonal[s.left] * m_forward[s.left] +
                              s.u * m_inverseDiagonal[s.up] * m_forward[s.up]) *
                             m_inverseDiagonal[s.i];
        double value = 0.0;
        for (auto it = m_stencils.rbegin(); it != m_stencils.rend(); ++it) {
            const auto &s = *it;
            m_preconditioned[s.i] = (m_forward[s.i] + s.r * m_inverseDiagonal[s.i] * m_preconditioned[s.right] +
                                     s.d * m_inverseDiagonal[s.i] * m_preconditioned[s.down]) *
                                    m_inverseDiagonal[s.i];
            value += static_cast<double>(m_residual[s.i]) * m_preconditioned[s.i];
        }
        const int C = m_coarseCount;
        if (C > 0) {
            std::fill(m_coarseRhs.begin(), m_coarseRhs.end(), 0.0);
            for (int i : m_coarseCells)
                for (int k = 0; k < 4; ++k)
                    m_coarseRhs[m_coarseWeights[i].node[k]] += m_coarseWeights[i].weight[k] * m_residual[i];
            for (int i = 0; i < C; ++i) {
                double sum = m_coarseRhs[i];
                for (int j = 0; j < i; ++j)
                    sum -= m_coarseFactor[i * C + j] * m_coarseSolution[j];
                m_coarseSolution[i] = sum / m_coarseFactor[i * C + i];
            }
            for (int i = C - 1; i >= 0; --i) {
                double sum = m_coarseSolution[i];
                for (int j = i + 1; j < C; ++j)
                    sum -= m_coarseFactor[j * C + i] * m_coarseSolution[j];
                m_coarseSolution[i] = sum / m_coarseFactor[i * C + i];
            }
            for (int i : m_coarseCells) {
                double correction = 0.0;
                for (int k = 0; k < 4; ++k)
                    correction += m_coarseWeights[i].weight[k] * m_coarseSolution[m_coarseWeights[i].node[k]];
                const float before = m_preconditioned[i];
                m_preconditioned[i] += static_cast<float>(correction);
                value += static_cast<double>(m_residual[i]) * (m_preconditioned[i] - before);
            }
        }
        return value;
    };
    if (islandSolve) {
        // Disconnected pressure domains form independent diagonal blocks.
        // Solve each block to its own relative tolerance, and stop visiting
        // it as soon as it converges. Large blocks are scheduled first.
        std::vector<std::vector<int>> islands(result.components);
        for (size_t k = 0; k < m_stencils.size(); ++k)
            islands[m_componentIndex[m_stencils[k].i]].push_back(static_cast<int>(k));
        std::vector<int> order(result.components), iterations(result.components);
        std::vector<uint8_t> converged(result.components);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return islands[a].size() > islands[b].size(); });
        // Remap each disconnected domain into contiguous local storage. Index
        // count is a read-only zero sentinel for absent neighbors; no worker
        // reads another domain's scratch fields.
        std::vector<int> localIndex(n);
        for (const auto &cells : islands)
            for (size_t k = 0; k < cells.size(); ++k)
                localIndex[m_stencils[cells[k]].i] = static_cast<int>(k);
        std::atomic<int> nextIsland{0};
        auto solveIsland = [&](int c) {
            const auto &cells = islands[c];
            const int count = static_cast<int>(cells.size());
            struct Row {
                int left, right, up, down;
                float anchor, l, r, u, d;
                float forwardL, forwardU, backwardR, backwardD, inverse, center;
            };
            std::vector<Row> rows(count);
            std::vector<float> residual(count + 1), direction(count + 1), product(count),
                forward(count + 1), preconditioned(count + 1);
            std::vector<double> pressure(count);
            double localNorm = 0.0;
            for (int k = 0; k < count; ++k) {
                const auto &s = m_stencils[cells[k]];
                const float inverse = m_inverseDiagonal[s.i];
                rows[k] = {s.l != 0.0f && m_stencilIndex[s.left] >= 0 ? localIndex[s.left] : count,
                           s.r != 0.0f && m_stencilIndex[s.right] >= 0 ? localIndex[s.right] : count,
                           s.u != 0.0f && m_stencilIndex[s.up] >= 0 ? localIndex[s.up] : count,
                           s.d != 0.0f && m_stencilIndex[s.down] >= 0 ? localIndex[s.down] : count,
                           s.diagonal - (s.l + s.r + s.u + s.d), s.l, s.r, s.u, s.d,
                           highContrast ? s.l : s.l * m_inverseDiagonal[s.left],
                           highContrast ? s.u : s.u * m_inverseDiagonal[s.up],
                           highContrast ? s.r : s.r * inverse,
                           highContrast ? s.d : s.d * inverse,
                           inverse, highContrast ? s.diagonal : 1.0f};
                residual[k] = m_residual[s.i];
                localNorm += static_cast<double>(residual[k]) * residual[k];
            }
            auto localPrecondition = [&]() {
                for (int k = 0; k < count; ++k) {
                    const auto &s = rows[k];
                    forward[k] = (residual[k] + s.forwardL * forward[s.left] +
                                  s.forwardU * forward[s.up]) * s.inverse;
                }
                double value = 0.0;
                for (int k = count - 1; k >= 0; --k) {
                    const auto &s = rows[k];
                    preconditioned[k] = (s.center * forward[k] + s.backwardR * preconditioned[s.right] +
                                         s.backwardD * preconditioned[s.down]) * s.inverse;
                    value += static_cast<double>(residual[k]) * preconditioned[k];
                }
                return value;
            };
            const double localTarget = std::max(1e-24, localNorm * tolerance * tolerance);
            double rz = localPrecondition();
            std::copy_n(preconditioned.begin(), count, direction.begin());
            for (int iter = 0; iter < maxIterations && localNorm > localTarget; ++iter) {
                double dot = 0.0;
                for (int k = 0; k < count; ++k) {
                    const auto &s = rows[k];
                    const double center = direction[k];
                    const double value = s.anchor * center + s.l * (center - direction[s.left]) +
                                         s.r * (center - direction[s.right]) + s.u * (center - direction[s.up]) +
                                         s.d * (center - direction[s.down]);
                    product[k] = static_cast<float>(value);
                    dot += center * value;
                }
                if (!(dot > 0.0)) break;
                const float alpha = static_cast<float>(rz / dot);
                localNorm = 0.0;
                for (int k = 0; k < count; ++k) {
                    pressure[k] += alpha * direction[k];
                    residual[k] -= alpha * product[k];
                    localNorm += static_cast<double>(residual[k]) * residual[k];
                }
                iterations[c] = iter + 1;
                if (localNorm <= localTarget) break;
                const double nextRz = localPrecondition();
                const float beta = static_cast<float>(nextRz / rz);
                for (int k = 0; k < count; ++k)
                    direction[k] = preconditioned[k] + beta * direction[k];
                rz = nextRz;
            }
            for (int k = 0; k < count; ++k)
                m_pressure[m_stencils[cells[k]].i] = pressure[k];
            converged[c] = localNorm <= localTarget;
        };
        // parallelFor needs at least 16 ranges to engage the shared pool.
        m_pool->parallelFor(0, 16, [&](int, int) {
            for (;;) {
                const int job = nextIsland.fetch_add(1, std::memory_order_relaxed);
                if (job >= result.components) break;
                solveIsland(order[job]);
            }
        });
        result.iterations = *std::max_element(iterations.begin(), iterations.end());
        result.converged = std::all_of(converged.begin(), converged.end(), [](uint8_t value) { return value != 0; });
        result.parallelComponents = result.components;
    } else {
        double rz = precondition();
        for (const auto &s : m_stencils)
            m_direction[s.i] = m_preconditioned[s.i];
        for (int iter = 0; iter < maxIterations && norm > target; ++iter) {
            eachStencil([&](int begin, int end) {
                for (int k = begin; k < end; ++k) {
                    const auto &s = m_stencils[k];
                    if (highContrast) {
                        float value = s.diagonal * m_direction[s.i];
                        value -= s.l * m_direction[s.left];
                        value -= s.r * m_direction[s.right];
                        value -= s.u * m_direction[s.up];
                        value -= s.d * m_direction[s.down];
                        m_product[s.i] = value;
                        m_reduction[k] = static_cast<double>(m_direction[s.i]) * value;
                        continue;
                    }
                    const double center = m_direction[s.i];
                    const float anchor = s.diagonal - (s.l + s.r + s.u + s.d);
                    const double value = anchor * center + s.l * (center - m_direction[s.left]) +
                                         s.r * (center - m_direction[s.right]) + s.u * (center - m_direction[s.up]) +
                                         s.d * (center - m_direction[s.down]);
                    m_product[s.i] = static_cast<float>(value);
                    m_reduction[k] = static_cast<double>(m_direction[s.i]) * value;
                }
            });
            const double dot = std::accumulate(m_reduction.begin(), m_reduction.end(), 0.0);
            if (!(dot > 0.0)) break;
            const float alpha = static_cast<float>(rz / dot);
            eachStencil([&](int begin, int end) {
                for (int k = begin; k < end; ++k) {
                    const auto &s = m_stencils[k];
                    const int i = s.i;
                    m_pressure[i] += alpha * m_direction[i];
                    m_residual[i] -= alpha * m_product[i];
                    m_reduction[k] = static_cast<double>(m_residual[i]) * m_residual[i];
                }
            });
            const double nextNorm = std::accumulate(m_reduction.begin(), m_reduction.end(), 0.0);
            const double nextRz = precondition();
            const float beta = static_cast<float>(nextRz / rz);
            eachStencil([&](int begin, int end) {
                for (int k = begin; k < end; ++k) {
                    const auto &s = m_stencils[k];
                    m_direction[s.i] = m_preconditioned[s.i] + beta * m_direction[s.i];
                }
            });
            norm = nextNorm;
            rz = nextRz;
            result.iterations = iter + 1;
        }
        result.converged = norm <= target;
    }
    result.coarseNodes = m_coarseCount;
    auto p = [&](int i) { return active(i) ? m_pressure[i] : 0.0f; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x <= W; ++x) {
            const int f = y * (W + 1) + x;
            u[f] -= dt * m_faceX[f] * (p(x < W ? y * W + x : -1) - p(x > 0 ? y * W + x - 1 : -1));
        }
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x < W; ++x) {
            const int f = y * W + x;
            v[f] -= dt * m_faceY[f] * (p(y < H ? y * W + x : -1) - p(y > 0 ? (y - 1) * W + x : -1));
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (active(y * W + x)) {
                const double d = divergence(x, y);
                result.divergenceAfter += d * d;
            }
    if (count > 0) {
        result.divergenceBefore = std::sqrt(result.divergenceBefore / count);
        result.divergenceAfter = std::sqrt(result.divergenceAfter / count);
    }
    return result;
}

} // namespace sim
