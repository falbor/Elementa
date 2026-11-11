#pragma once

#include "sim/SimWorld.hpp"

#include <memory>

namespace sim::app {

class SdlApp {
public:
    explicit SdlApp(SimWorld &world, ViewMode initialView = ViewMode::Physical,
                    PresetId initialPreset = PresetId::Laboratory);
    ~SdlApp();

    SdlApp(const SdlApp &) = delete;
    SdlApp &operator=(const SdlApp &) = delete;

    bool create(int windowWidth = 1480, int windowHeight = 900);
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sim::app
