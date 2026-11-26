#include "sim/ContinuumInterface.hpp"

#include <stdexcept>
#include <utility>

namespace sim {

void reconstructSurface(int width, int height, std::span<const float> volume, std::span<const uint8_t> solid,
                        std::span<float> distance) {
    if (width < 1 || height < 1 || volume.size() != static_cast<size_t>(width) * height ||
        solid.size() != volume.size() || distance.size() != volume.size())
        throw std::invalid_argument("Invalid surface reconstruction grid");
    for (size_t i = 0; i < volume.size(); ++i) {
        if (!std::isfinite(volume[i]) || volume[i] < 0.0f) throw std::invalid_argument("Invalid surface volume");
        distance[i] = volume[i] >= 0.5f && !solid[i] ? -3.0f : 3.0f;
    }
    auto sample = [&](int x, int y, int center) {
        if (x < 0 || x >= width || y < 0 || y >= height || solid[y * width + x]) return volume[center];
        return volume[y * width + x];
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            if (solid[i] || volume[i] <= 0.0f) continue;
            if (volume[i] >= 1.0f) {
                // An exactly grid-aligned full/empty interface needs no
                // fractional seed: its location is the shared cell face.
                for (auto delta : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                    const int nx = x + delta.first, ny = y + delta.second;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const int j = ny * width + nx;
                    if (solid[j] || volume[j] > 0.0f) continue;
                    distance[i] = std::max(distance[i], -0.5f);
                    distance[j] = std::min(distance[j], 0.5f);
                }
                continue;
            }
            const float l = sample(x - 1, y, i), r = sample(x + 1, y, i);
            const float u = sample(x, y - 1, i), d = sample(x, y + 1, i);
            auto limited = [](float a, float b) {
                return a * b > 0.0f ? std::copysign(2.0f * std::min(std::abs(a), std::abs(b)), a) : 0.0f;
            };
            double nx = limited(volume[i] - l, r - volume[i]), ny = limited(volume[i] - u, d - volume[i]);
            if (nx == 0.0 && ny == 0.0) {
                nx = r - l;
                ny = d - u;
            }
            const double length = std::hypot(nx, ny);
            if (length == 0.0) {
                // An isolated symmetric fraction supplies no interface normal.
                // Retain its center classification without inventing a direction.
                distance[i] = 0.5f - volume[i];
                continue;
            }
            const double offset = planeOffset(nx, ny, volume[i]);
            distance[i] = static_cast<float>(offset / length);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                    const int j = sy * width + sx;
                    if (solid[j] || (volume[j] > 0.0f && volume[j] < 1.0f)) continue;
                    const float phi = static_cast<float>((offset - nx * dx - ny * dy) / length);
                    if ((phi < 0.0f) != (volume[j] >= 1.0f)) continue;
                    if (std::abs(phi) < std::abs(distance[j])) distance[j] = phi;
                }
        }
}

} // namespace sim
