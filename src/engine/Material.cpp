#include "sim/Material.hpp"

#include <algorithm>

namespace sim {

void infernoColormap(float t01, uint8_t& outR, uint8_t& outG, uint8_t& outB) noexcept {
    const float t = std::clamp(t01, 0.0f, 1.0f);
    struct Stop { float t; float r, g, b; };
    static constexpr Stop stops[] = {
        {0.00f,  10.0f,  12.0f,  34.0f},
        {0.18f,  42.0f,  24.0f, 118.0f},
        {0.38f, 134.0f,  34.0f, 108.0f},
        {0.58f, 214.0f,  74.0f,  48.0f},
        {0.78f, 250.0f, 168.0f,  18.0f},
        {1.00f, 252.0f, 254.0f, 178.0f}
    };
    for (int i = 0; i < 5; ++i) {
        if (t <= stops[i + 1].t) {
            const float u = (t - stops[i].t) / (stops[i + 1].t - stops[i].t);
            outR = static_cast<uint8_t>(stops[i].r + u * (stops[i + 1].r - stops[i].r));
            outG = static_cast<uint8_t>(stops[i].g + u * (stops[i + 1].g - stops[i].g));
            outB = static_cast<uint8_t>(stops[i].b + u * (stops[i + 1].b - stops[i].b));
            return;
        }
    }
    outR = 252; outG = 254; outB = 178;
}

} // namespace sim
