#ifdef _WIN32
#include "Win32App.hpp"
#endif
#include "sim/SimWorld.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

sim::PresetId parsePreset(const std::string& s) {
    if (s == "volcano" || s == "vulcano") return sim::PresetId::Volcano;
    if (s == "hydro" || s == "idrodinamica") return sim::PresetId::Hydrodynamics;
    if (s == "explosives" || s == "esplosivi") return sim::PresetId::Explosives;
    if (s == "electric" || s == "elettrico") return sim::PresetId::ElectricGrid;
    if (s == "zerog" || s == "orbita") return sim::PresetId::ZeroGOrb;
    return sim::PresetId::Laboratory;
}

sim::ViewMode parseView(const std::string& s) {
    if (s == "thermal" || s == "termico") return sim::ViewMode::Thermal;
    if (s == "pressure" || s == "pressione") return sim::ViewMode::Pressure;
    if (s == "velocity" || s == "velocita") return sim::ViewMode::Velocity;
    if (s == "light" || s == "luce") return sim::ViewMode::EnergyLight;
    return sim::ViewMode::Physical;
}

void printUsage() {
    std::cout
        << "Simulatore Fisico di Particelle 2D Per-Pixel (C++20)\n"
        << "Uso:\n"
        << "  pixel_sim [opzioni]\n\n"
        << "Opzioni:\n"
        << "  --width <N>               Larghezza griglia in pixel (default: 480)\n"
        << "  --height <N>              Altezza griglia in pixel (default: 270)\n"
        << "  --preset <nome>           lab | volcano | hydro | explosives | electric | zerog\n"
        << "  --view <modo>             physical | thermal | pressure | velocity | light\n"
        << "  --benchmark [ticks]       Esegue N tick headless e stampa metriche (default: 100)\n"
        << "  --dump-ppm <ticks> <file> Esegue N tick e salva immagine PPM per ispezione\n"
        << "  --threads <N>             Numero thread simulazione (0 = auto)\n"
        << "  --help                    Mostra questo messaggio\n";
}

} // namespace

int main(int argc, char** argv) {
    int width = 480;
    int height = 270;
    unsigned threads = 0;
    sim::PresetId preset = sim::PresetId::Laboratory;
    sim::ViewMode view = sim::ViewMode::Physical;
    int benchmarkTicks = 0;
    int dumpTicks = -1;
    std::string dumpPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--width" && i + 1 < argc) {
            width = std::max(32, std::atoi(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            height = std::max(32, std::atoi(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::max(0, std::atoi(argv[++i])));
        } else if (arg == "--preset" && i + 1 < argc) {
            preset = parsePreset(argv[++i]);
        } else if (arg == "--view" && i + 1 < argc) {
            view = parseView(argv[++i]);
        } else if (arg == "--benchmark") {
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                benchmarkTicks = std::max(1, std::atoi(argv[++i]));
            } else {
                benchmarkTicks = 100;
            }
        } else if (arg == "--dump-ppm" && i + 2 < argc) {
            dumpTicks = std::max(0, std::atoi(argv[++i]));
            dumpPath = argv[++i];
        }
    }

    sim::SimWorld world(width, height, threads);
    world.loadPreset(preset);

    if (benchmarkTicks > 0) {
        const auto t0 = std::chrono::steady_clock::now();
        double sumFluid = 0.0, sumThermo = 0.0, sumReact = 0.0, sumPart = 0.0, sumLight = 0.0;
        for (int t = 0; t < benchmarkTicks; ++t) {
            world.step();
            world.render(view);
            const auto& st = world.stats();
            sumFluid += st.fluidMs;
            sumThermo += st.thermoMs;
            sumReact += st.reactionMs;
            sumPart += st.particleMs;
            sumLight += st.lightMs;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double totalMs = std::max(1e-6, std::chrono::duration<double, std::milli>(t1 - t0).count());
        const double avgMs = totalMs / static_cast<double>(benchmarkTicks);
        const double mpixPerSec = (static_cast<double>(world.pixelCount()) * benchmarkTicks) / (totalMs * 1000.0);

        std::cout << "Benchmark completato:\n"
                  << "  Griglia:     " << width << "x" << height << " (" << world.pixelCount() << " pixel simulati/tick)\n"
                  << "  Tick:        " << benchmarkTicks << "\n"
                  << "  Tempo tot:   " << totalMs << " ms\n"
                  << "  Media/tick:  " << avgMs << " ms (" << (1000.0 / avgMs) << " FPS teorici)\n"
                  << "  Throughput:  " << mpixPerSec << " MPixel/s\n"
                  << "  Breakdown medio: Fluido=" << (sumFluid / benchmarkTicks)
                  << "ms | TermoElettr=" << (sumThermo / benchmarkTicks)
                  << "ms | Reazioni=" << (sumReact / benchmarkTicks)
                  << "ms | Particelle=" << (sumPart / benchmarkTicks)
                  << "ms | Luce=" << (sumLight / benchmarkTicks) << "ms\n";
        return 0;
    }

    if (dumpTicks >= 0) {
        for (int t = 0; t < dumpTicks; ++t) {
            world.step();
        }
        if (!world.savePPM(dumpPath, view)) {
            std::cerr << "Errore nel salvataggio di " << dumpPath << "\n";
            return 1;
        }
        std::cout << "Salvato snapshot PPM (" << width << "x" << height << ") al tick "
                  << dumpTicks << " in " << dumpPath << "\n";
        return 0;
    }

#ifdef _WIN32
    sim::app::Win32App app(world, view, preset);
    if (!app.create(1280, 780)) {
        std::cerr << "Impossibile inizializzare la finestra Win32.\n";
        return 1;
    }
    return app.run();
#else
    std::cerr << "Modalita' GUI disponibile solo su Windows; usa --benchmark o --dump-ppm.\n";
    printUsage();
    return 1;
#endif
}
