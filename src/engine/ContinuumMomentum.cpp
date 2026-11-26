#include "sim/ContinuumMomentum.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sim {

ContinuumMomentum::ContinuumMomentum(int width, int height, ThreadPool *pool)
    : m_width(width), m_height(height), m_pool(pool) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096) throw std::invalid_argument("Invalid momentum grid");
    for (auto *field : {&m_u, &m_rhsU, &m_tmpU})
        field->resize(static_cast<size_t>(width + 1) * height);
    for (auto *field : {&m_v, &m_rhsV, &m_tmpV})
        field->resize(static_cast<size_t>(height + 1) * width);
    m_blockU.resize(m_u.size());
    m_blockV.resize(m_v.size());
    m_nuU.resize(m_u.size());
    m_nuV.resize(m_v.size());
}

void ContinuumMomentum::advance(std::span<float> u, std::span<float> v, std::span<const float> viscosity,
                                std::span<const uint8_t> solid, float dt, bool closed) {
    const int W = m_width, H = m_height;
    auto rows = [&](int count, auto &&fn) {
        if (m_pool)
            m_pool->parallelFor(0, count, fn);
        else
            fn(0, count);
    };
    if (u.size() != m_u.size() || v.size() != m_v.size() || viscosity.size() != static_cast<size_t>(W) * H ||
        solid.size() != viscosity.size() || !std::isfinite(dt) || dt < 0.0f || dt > 1.0f)
        throw std::invalid_argument("Invalid momentum fields");
    for (auto field : {u, v})
        for (float value : field)
            if (!std::isfinite(value) || std::abs(value) > 12.0f)
                throw std::invalid_argument("Invalid momentum velocity");
    for (float value : viscosity)
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) throw std::invalid_argument("Invalid viscosity");
    // A failed accelerator must leave the input fields intact for this fallback.
    if (m_backend && m_backend(u, v, viscosity, solid, dt, closed)) return;
    auto wall = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return closed;
        return solid[y * W + x] != 0;
    };
    auto originalFaceBlocked = [&](int x, int y, bool vertical) {
        return vertical ? wall(x, y - 1) || wall(x, y) : wall(x - 1, y) || wall(x, y);
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x <= W; ++x)
            m_u[y * (W + 1) + x] =
                (m_blockU[y * (W + 1) + x] = originalFaceBlocked(x, y, false)) ? 0.0f : u[y * (W + 1) + x];
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x < W; ++x)
            m_v[y * W + x] = (m_blockV[y * W + x] = originalFaceBlocked(x, y, true)) ? 0.0f : v[y * W + x];
    auto faceBlocked = [&](int x, int y, bool vertical) {
        return vertical ? m_blockV[y * W + x] != 0 : m_blockU[y * (W + 1) + x] != 0;
    };
    auto sample = [&](const std::vector<float> &field, float x, float y, bool vertical) {
        const int fw = vertical ? W : W + 1, fh = vertical ? H + 1 : H;
        x = std::clamp(x + (vertical ? 0.0f : 0.5f), 0.0f, static_cast<float>(fw - 1));
        y = std::clamp(y + (vertical ? 0.5f : 0.0f), 0.0f, static_cast<float>(fh - 1));
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        float sum = 0.0f, weight = 0.0f;
        for (int dy = 0; dy <= 1; ++dy)
            for (int dx = 0; dx <= 1; ++dx) {
                const int sx = std::min(ix + dx, fw - 1), sy = std::min(iy + dy, fh - 1);
                if (faceBlocked(sx, sy, vertical)) continue;
                const float w = (dx ? x - ix : 1.0f - (x - ix)) * (dy ? y - iy : 1.0f - (y - iy));
                sum += field[sy * fw + sx] * w;
                weight += w;
            }
        return weight > 1e-6f ? sum / weight : 0.0f;
    };
    auto backtrace = [&](float &x, float &y) {
        const float mx = x - 0.5f * dt * sample(m_u, x, y, false);
        const float my = y - 0.5f * dt * sample(m_v, x, y, true);
        const float dx = -dt * sample(m_u, mx, my, false), dy = -dt * sample(m_v, mx, my, true);
        const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)) * 4.0f)));
        const float startX = x, startY = y;
        for (int step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / steps;
            const float nx = startX + dx * t, ny = startY + dy * t;
            const int cx = static_cast<int>(std::floor(x + 0.5f)), cy = static_cast<int>(std::floor(y + 0.5f));
            const int tx = static_cast<int>(std::floor(nx + 0.5f)), ty = static_cast<int>(std::floor(ny + 0.5f));
            if (wall(tx, ty) || (tx != cx && ty != cy && (wall(tx, cy) || wall(cx, ty)))) break;
            x = nx;
            y = ny;
        }
    };
    auto advect = [&](std::vector<float> &rhs, const std::vector<float> &source, bool vertical) {
        const int fw = vertical ? W : W + 1, fh = vertical ? H + 1 : H;
        rows(fh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < fw; ++x) {
                    if (faceBlocked(x, y, vertical)) {
                        rhs[y * fw + x] = 0.0f;
                        continue;
                    }
                    if (source[y * fw + x] == 0.0f) {
                        bool stationary = true;
                        for (int a = 0; a < 2; ++a)
                            for (int b = 0; b < 2; ++b) {
                                const float transverse =
                                    vertical ? m_u[std::clamp(y - 1 + b, 0, H - 1) * (W + 1) + std::clamp(x + a, 0, W)]
                                             : m_v[std::clamp(y + b, 0, H) * W + std::clamp(x - 1 + a, 0, W - 1)];
                                stationary &= transverse == 0.0f;
                            }
                        if (stationary) {
                            rhs[y * fw + x] = 0.0f;
                            continue;
                        }
                    }
                    float px = x - (vertical ? 0.0f : 0.5f), py = y - (vertical ? 0.5f : 0.0f);
                    backtrace(px, py);
                    rhs[y * fw + x] = sample(source, px, py, vertical);
                }
        });
    };
    advect(m_rhsU, m_u, false);
    advect(m_rhsV, m_v, true);
    auto diffuse = [&](std::span<float> output, const std::vector<float> &rhs, std::vector<float> &current,
                       std::vector<float> &next, bool vertical) {
        const int fw = vertical ? W : W + 1, fh = vertical ? H + 1 : H;
        current = rhs;
        next = rhs;
        auto nu = [&](int x, int y) { return viscosity[std::clamp(y, 0, H - 1) * W + std::clamp(x, 0, W - 1)]; };
        auto &faceNu = vertical ? m_nuV : m_nuU;
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x)
                faceNu[y * fw + x] = 0.5f * (nu(x, y) + (vertical ? nu(x, y - 1) : nu(x - 1, y)));
        m_diffusion.clear();
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                const int i = y * fw + x;
                if (faceBlocked(x, y, vertical)) continue;
                DiffusionStencil stencil{};
                stencil.i = i;
                float denominator = 1.0f;
                for (int direction = 0; direction < 4; ++direction) {
                    const int nx = x + (direction == 0 ? -1 : direction == 1 ? 1 : 0);
                    const int ny = y + (direction == 2 ? -1 : direction == 3 ? 1 : 0);
                    stencil.neighbor[direction] = i;
                    if (nx < 0 || ny < 0 || nx >= fw || ny >= fh || faceBlocked(nx, ny, vertical)) continue;
                    stencil.neighbor[direction] = ny * fw + nx;
                    stencil.coefficient[direction] = dt * 0.5f * (faceNu[i] + faceNu[ny * fw + nx]);
                    denominator += stencil.coefficient[direction];
                }
                if (denominator > 1.0f) {
                    stencil.inverseDiagonal = 1.0f / denominator;
                    m_diffusion.push_back(stencil);
                }
            }
        for (int iter = 0; iter < 12; ++iter) {
            rows(static_cast<int>(m_diffusion.size()), [&](int first, int last) {
                for (int k = first; k < last; ++k) {
                    const auto &stencil = m_diffusion[k];
                    float sum = rhs[stencil.i];
                    for (int d = 0; d < 4; ++d)
                        sum += stencil.coefficient[d] * current[stencil.neighbor[d]];
                    next[stencil.i] = sum * stencil.inverseDiagonal;
                }
            });
            current.swap(next);
        }
        std::copy(current.begin(), current.end(), output.begin());
    };
    diffuse(u, m_rhsU, m_u, m_tmpU, false);
    diffuse(v, m_rhsV, m_v, m_tmpV, true);
}

} // namespace sim
