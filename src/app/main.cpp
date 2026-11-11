#ifdef ELEMENTA_WITH_GUI
#include "SdlApp.hpp"
#endif
#include "sim/SimWorld.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool parseIntStrict(const char *str, int &out) noexcept {
    if (!str || str[0] == '\0') return false;
    const char *end = str + std::strlen(str);
    int val = 0;
    const auto [ptr, ec] = std::from_chars(str, end, val);
    if (ec != std::errc{} || ptr != end) return false;
    out = val;
    return true;
}

bool parsePreset(std::string_view s, sim::PresetId &out) noexcept {
    if (s == "lab" || s == "laboratorio") {
        out = sim::PresetId::Laboratory;
        return true;
    }
    if (s == "volcano" || s == "vulcano") {
        out = sim::PresetId::Volcano;
        return true;
    }
    if (s == "hydro" || s == "idrodinamica") {
        out = sim::PresetId::Hydrodynamics;
        return true;
    }
    if (s == "explosives" || s == "esplosivi") {
        out = sim::PresetId::Explosives;
        return true;
    }
    if (s == "electric" || s == "elettrico") {
        out = sim::PresetId::ElectricGrid;
        return true;
    }
    if (s == "zerog" || s == "orbita") {
        out = sim::PresetId::ZeroGOrb;
        return true;
    }
    return false;
}

bool parseView(std::string_view s, sim::ViewMode &out) noexcept {
    if (s == "physical" || s == "fisico") {
        out = sim::ViewMode::Physical;
        return true;
    }
    if (s == "thermal" || s == "termico") {
        out = sim::ViewMode::Thermal;
        return true;
    }
    if (s == "pressure" || s == "pressione") {
        out = sim::ViewMode::Pressure;
        return true;
    }
    if (s == "velocity" || s == "velocita") {
        out = sim::ViewMode::Velocity;
        return true;
    }
    if (s == "light" || s == "luce") {
        out = sim::ViewMode::EnergyLight;
        return true;
    }
    return false;
}

void printUsage() {
    std::cout << "Simulatore Fisico di Particelle 2D Per-Pixel (C++20)\n"
              << "Uso:\n"
              << "  pixel_sim [opzioni]\n\n"
              << "Opzioni:\n"
              << "  --width <N>               Larghezza griglia in pixel (32..4096, default: 480)\n"
              << "  --height <N>              Altezza griglia in pixel (32..4096, default: 270)\n"
              << "  --preset <nome>           lab | volcano | hydro | explosives | electric | zerog\n"
              << "  --view <modo>             physical | thermal | pressure | velocity | light\n"
              << "  --benchmark [ticks]       Esegue N tick headless e stampa metriche (default: 100)\n"
              << "  --dump-ppm <ticks> <file> Esegue N tick e salva immagine PPM per ispezione\n"
              << "  --threads <N>             Numero thread simulazione (0 = auto)\n"
              << "  --help                    Mostra questo messaggio\n";
}

} // namespace

int main(int argc, char **argv) {
    constexpr int kMinGridDim = 32;
    constexpr int kMaxGridDim = 4096;
    constexpr int kMaxCliTicks = 10000000;

    int width = 480;
    int height = 270;
    unsigned threads = 0;
    sim::PresetId preset = sim::PresetId::Laboratory;
    sim::ViewMode view = sim::ViewMode::Physical;
    int benchmarkTicks = 0;
    int dumpTicks = -1;
    std::string dumpPath;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--width") {
            int val = 0;
            if (i + 1 >= argc || !parseIntStrict(argv[++i], val) || val <= 0 || val > kMaxGridDim) {
                std::cerr << "Valore non valido o mancante per --width (atteso intero positivo <= " << kMaxGridDim
                          << ").\n";
                return 1;
            }
            width = std::max(kMinGridDim, val);
        } else if (arg == "--height") {
            int val = 0;
            if (i + 1 >= argc || !parseIntStrict(argv[++i], val) || val <= 0 || val > kMaxGridDim) {
                std::cerr << "Valore non valido o mancante per --height (atteso intero positivo <= " << kMaxGridDim
                          << ").\n";
                return 1;
            }
            height = std::max(kMinGridDim, val);
        } else if (arg == "--threads") {
            int val = 0;
            if (i + 1 >= argc || !parseIntStrict(argv[++i], val) || val < 0 || val > 1024) {
                std::cerr << "Valore non valido o mancante per --threads (atteso intero >= 0).\n";
                return 1;
            }
            threads = static_cast<unsigned>(val);
        } else if (arg == "--preset") {
            if (i + 1 >= argc || !parsePreset(argv[i + 1], preset)) {
                std::cerr << "Preset non valido o mancante per --preset.\n";
                return 1;
            }
            ++i;
        } else if (arg == "--view") {
            if (i + 1 >= argc || !parseView(argv[i + 1], view)) {
                std::cerr << "Modalita' non valida o mancante per --view.\n";
                return 1;
            }
            ++i;
        } else if (arg == "--benchmark") {
            if (i + 1 < argc) {
                const std::string_view next = argv[i + 1];
                if (next != "-h" && next.rfind("--", 0) != 0) {
                    int val = 0;
                    if (!parseIntStrict(argv[++i], val) || val < 1 || val > kMaxCliTicks) {
                        std::cerr << "Valore non valido per --benchmark (atteso intero >= 1).\n";
                        return 1;
                    }
                    benchmarkTicks = val;
                    continue;
                }
            }
            benchmarkTicks = 100;
        } else if (arg == "--dump-ppm") {
            int val = 0;
            if (i + 2 >= argc || !parseIntStrict(argv[++i], val) || val < 0 || val > kMaxCliTicks) {
                std::cerr << "Argomenti non validi o mancanti per --dump-ppm <ticks> <file>.\n";
                return 1;
            }
            const std::string_view pathArg = argv[++i];
            if (pathArg.empty() || pathArg.rfind("--", 0) == 0) {
                std::cerr << "Percorso file mancante o non valido per --dump-ppm <ticks> <file>.\n";
                return 1;
            }
            dumpTicks = val;
            dumpPath = std::string(pathArg);
        } else {
            std::cerr << "Opzione sconosciuta: " << arg << "\n";
            printUsage();
            return 1;
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
            const auto &st = world.stats();
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
                  << "  Griglia:     " << width << "x" << height << " (" << world.pixelCount()
                  << " pixel simulati/tick)\n"
                  << "  Tick:        " << benchmarkTicks << "\n"
                  << "  Tempo tot:   " << totalMs << " ms\n"
                  << "  Media/tick:  " << avgMs << " ms (" << (1000.0 / avgMs) << " FPS teorici)\n"
                  << "  Throughput:  " << mpixPerSec << " MPixel/s\n"
                  << "  Breakdown medio: Fluido=" << (sumFluid / benchmarkTicks)
                  << "ms | TermoElettr=" << (sumThermo / benchmarkTicks)
                  << "ms | Reazioni=" << (sumReact / benchmarkTicks) << "ms | Particelle=" << (sumPart / benchmarkTicks)
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
        std::cout << "Salvato snapshot PPM (" << width << "x" << height << ") al tick " << dumpTicks << " in "
                  << dumpPath << "\n";
        return 0;
    }

#ifdef ELEMENTA_WITH_GUI
    sim::app::SdlApp app(world, view, preset);
    if (!app.create(1280, 780)) {
        std::cerr << "Impossibile inizializzare la finestra SDL3/OpenGL.\n";
        return 1;
    }
    return app.run();
#else
    std::cerr << "Modalita' GUI disabilitata in questa build (ELEMENTA_BUILD_GUI=OFF); usa --benchmark o --dump-ppm.\n";
    printUsage();
    return 1;
#endif
}
