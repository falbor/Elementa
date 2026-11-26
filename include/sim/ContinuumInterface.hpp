#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace sim {

// Reconstruct a signed distance near the volume-fraction interface. Negative
// values mark pressure sample centers inside liquid; quantities are untouched.
void reconstructSurface(int width, int height, std::span<const float> volume, std::span<const uint8_t> solid,
                        std::span<float> distance);

// Area of the part of a centered rectangle satisfying nx*x+ny*y >= offset.
// The convolution of two uniform distributions gives an exact quadratic /
// linear area function, including axis-aligned normals without division by 0.
inline double planeFraction(double nx, double ny, double offset, double width = 1.0, double height = 1.0) {
    const double a = std::max(std::abs(nx) * width, std::abs(ny) * height);
    const double b = std::min(std::abs(nx) * width, std::abs(ny) * height);
    if (a == 0.0) return offset <= 0.0 ? 1.0 : 0.0;
    const double distance = 0.5 * (a + b) - std::abs(offset);
    double tail = 0.0;
    if (distance > 0.0) tail = b > 0.0 && distance < b ? distance * distance / (2.0 * a * b) : (distance - 0.5 * b) / a;
    tail = std::clamp(tail, 0.0, 0.5);
    return offset >= 0.0 ? tail : 1.0 - tail;
}

// Invert the exact area function. The normal points into the liquid.
inline double planeOffset(double nx, double ny, double fill) {
    const double a = std::max(std::abs(nx), std::abs(ny));
    const double b = std::min(std::abs(nx), std::abs(ny));
    const double q = std::clamp(fill, 0.0, 1.0);
    const double tail = std::min(q, 1.0 - q);
    const double distance = a > 0.0 && tail < b / (2.0 * a) ? std::sqrt(2.0 * a * b * tail) : a * tail + 0.5 * b;
    const double offset = 0.5 * (a + b) - distance;
    return q <= 0.5 ? offset : -offset;
}

} // namespace sim
