#ifdef ELEMENTA_WITH_GUI
#include "SdlApp.hpp"
#include "GlMomentum.hpp"
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
              << "  --continuum               Fluidi continui (predefinito)\n"
              << "  --legacy-particles        Modello a particelle per confronto/debug\n"
              << "  --dump-gpu-ppm <N> <file> Esporta la vista fisica prodotta dalla GPU\n"
              << "  --test-gpu-momentum       Verifica numerica e tempi del moto GPU\n"
              << "  --test-gpu-world          Verifica i sei preset con fisica GPU\n"
              << "  --gpu-fluid               Usa il moto GPU anche nei benchmark\n"
              << "  --cpu-fluid               Disabilita il moto GPU per confronto\n"
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
    bool continuum = true;
    bool gpuDump = false;
    bool gpuMomentumTest = false;
    bool gpuWorldTest = false;
    bool gpuFluid = false, cpuFluid = false;
    std::string dumpPath;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--test-gpu-momentum") {
            gpuMomentumTest = true;
        } else if (arg == "--test-gpu-world") {
            gpuWorldTest = true;
        } else if (arg == "--gpu-fluid") {
            gpuFluid = true;
        } else if (arg == "--cpu-fluid") {
            cpuFluid = true;
        } else if (arg == "--continuum") {
            continuum = true;
        } else if (arg == "--legacy-particles") {
            continuum = false;
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
        } else if (arg == "--dump-ppm" || arg == "--dump-gpu-ppm") {
            gpuDump = arg == "--dump-gpu-ppm";
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

    if ((gpuFluid && cpuFluid) || (gpuFluid && !continuum) ||
        ((gpuMomentumTest || gpuWorldTest) && (benchmarkTicks > 0 || dumpTicks >= 0)) ||
        (gpuMomentumTest && gpuWorldTest)) {
        std::cerr << "Opzioni GPU/CPU o modalita' di verifica incompatibili.\n";
        return 1;
    }
    if (benchmarkTicks > 0 && dumpTicks >= 0) {
        std::cerr << "--benchmark e --dump-ppm sono modalita' alternative.\n";
        return 1;
    }

    if (gpuMomentumTest || gpuWorldTest) {
#ifdef ELEMENTA_WITH_GUI
        return sim::app::validateGpuMomentum(gpuWorldTest);
#else
        std::cerr << "La verifica GPU richiede una build con OpenGL.\n";
        return 1;
#endif
    }
    sim::SimWorld world(width, height, threads);
    world.loadPreset(preset);
    if (continuum) world.enableContinuum();

    if (benchmarkTicks > 0) {
#ifdef ELEMENTA_WITH_GUI
        std::unique_ptr<sim::app::SdlApp> gpuSession;
        if (gpuFluid) {
            gpuSession = std::make_unique<sim::app::SdlApp>(world, view, preset);
            if (!gpuSession->create(640, 480, true, true)) return 1;
            if (!gpuSession->gpuMomentumActive()) {
                std::cerr << "Benchmark GPU non disponibile: backend non attivo.\n";
                return 1;
            }
        }
#else
        if (gpuFluid) { std::cerr << "Il benchmark GPU richiede una build OpenGL.\n"; return 1; }
#endif
        const auto t0 = std::chrono::steady_clock::now();
        double sumFluid = 0.0, sumThermo = 0.0, sumReact = 0.0, sumPart = 0.0, sumLight = 0.0;
        double sumMomentum = 0.0, sumPressure = 0.0, sumTransport = 0.0, sumView = 0.0;
        for (int t = 0; t < benchmarkTicks; ++t) {
            world.step();
            world.render(view);
            const auto &st = world.stats();
            sumFluid += st.fluidMs;
            sumThermo += st.thermoMs;
            sumReact += st.reactionMs;
            sumPart += st.particleMs;
            sumLight += st.lightMs;
            sumMomentum += st.continuumMomentumMs; sumPressure += st.continuumPressureMs;
            sumTransport += st.continuumTransportMs; sumView += st.continuumViewMs;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double totalMs = std::max(1e-6, std::chrono::duration<double, std::milli>(t1 - t0).count());
        const double avgMs = totalMs / static_cast<double>(benchmarkTicks);
        const double mpixPerSec = (static_cast<double>(world.pixelCount()) * benchmarkTicks) / (totalMs * 1000.0);
#ifdef ELEMENTA_WITH_GUI
        if (gpuSession && !gpuSession->gpuMomentumActive()) {
            std::cerr << "Benchmark invalidato: il backend GPU e' passato al fallback CPU.\n";
            return 1;
        }
#endif

        std::cout << "Benchmark completato:\n"
                  << "  Griglia:     " << width << "x" << height << " (" << world.pixelCount()
                  << " pixel simulati/tick)\n"
                  << "  Tick:        " << benchmarkTicks << "\n"
                  << "  Tempo tot:   " << totalMs << " ms\n"
                  << "  Media/tick:  " << avgMs << " ms (" << (1000.0 / avgMs) << " tick/s, rendering CPU)\n"
                  << "  Throughput:  " << mpixPerSec << " MPixel/s\n"
                  << "  Breakdown medio: Fluido=" << (sumFluid / benchmarkTicks)
                  << "ms | TermoElettr=" << (sumThermo / benchmarkTicks)
                  << "ms | Reazioni=" << (sumReact / benchmarkTicks) << "ms | Particelle=" << (sumPart / benchmarkTicks)
                  << "ms | Luce=" << (sumLight / benchmarkTicks) << "ms\n";
        if (continuum)
            std::cout << "  Continuum: Momento=" << sumMomentum / benchmarkTicks << "ms | Pressione=" << sumPressure / benchmarkTicks
                      << "ms | Trasporto=" << sumTransport / benchmarkTicks << "ms | Vista=" << sumView / benchmarkTicks << "ms\n";
        return 0;
    }

    if (dumpTicks >= 0) {
        if (gpuDump) {
#ifdef ELEMENTA_WITH_GUI
            sim::app::SdlApp capture(world, sim::ViewMode::Physical, preset);
            if (!capture.create(1280, 780, !cpuFluid, true)) return 1;
            for (int t = 0; t < dumpTicks; ++t) world.step();
            return capture.saveGpuPPM(dumpPath) ? 0 : 1;
#else
            std::cerr << "Esportazione GPU non disponibile nella build headless.\n";
            return 1;
#endif
        }
        if (gpuFluid) { std::cerr << "Per il moto GPU usa --dump-gpu-ppm.\n"; return 1; }
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
    if (!app.create(1280, 780, !cpuFluid)) {
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
