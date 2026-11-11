# Simulatore Fisico di Particelle 2D Per-Pixel (C++20)

Motore di simulazione fisica 2D in C++20 in cui **ogni singolo pixel** della griglia (`480x270` di default = `129.600` pixel, configurabile) viene calcolato e simulato ad ogni tick su più strati fisici accoppiati, con interfaccia grafica cross-platform **SDL3 + Dear ImGui + OpenGL 3.3** (Windows, Linux, macOS) e core di simulazione/test puro C++20 privo di dipendenze esterne.

## Architettura Per-Pixel

Ogni pixel `(x, y)` partecipa a tutti i passaggi di simulazione ad ogni step:

1. **Campo Fluido Euleriano (`src/engine/FluidSolver.cpp`)**:
   - Velocità dell'aria `(airVx, airVy)` e pressione scalare `P` su ogni cella.
   - Spinta di galleggiamento termico (approssimazione di Boussinesq), peso idrostatico dei liquidi, accoppiamento aerodinamico con le particelle mobili.
   - Avvezione semi-Lagrangiana, confinement di vorticità, divergenza solid-aware e risolutore di Poisson Red-Black Gauss-Seidel con condizioni al contorno di Neumann sui solidi.
2. **Termodinamica ed Elettromagnetismo (`src/engine/ThermoElectric.cpp`)**:
   - Conduzione termica di Fourier simmetrica con conservazione esatta dell'energia termica $\sum c_i T_i$ e raffreddamento radiativo.
   - Conduzione elettrica implicita (Red-Black SOR) attraverso metalli, batterie, acqua salata, acidi e plasma con riscaldamento Joule ($I^2 R$).
3. **Transizioni di Fase e Chimica (`src/engine/Reactions.cpp`)**:
   - Fusione/solidificazione (`Ghiaccio ↔ Acqua`, `Sabbia → Vetro Fuso ↔ Vetro`, `Pietra ↔ Lava ↔ Ossidiana`, `Metallo ↔ Metallo Fuso`).
   - Ebollizione/condensazione (`Acqua ↔ Vapore`, `Acqua Salata → Vapore + Sale`).
   - Combustione, esplosioni (`Polvere da Sparo`, `Nitroglicerina`, `Metano`, `Olio`, `Legno`, `Pianta`), corrosione esotermica da `Acido`, dissoluzione del `Sale`, idratazione delle piante e archi elettrici dielettrici.
4. **Cinematica delle Particelle (`src/engine/ParticlePhysics.cpp`)**:
   - Integrazione sub-pixel `(fx, fy)` e raycast DDA multi-step (anti-tunneling per particelle veloci).
   - Gradiente di pressione (`-∇P / ρ`), resistenza aerodinamica dall'aria, gravità configurabile (`Terra`, `Zero-G`, `Inversa`, `Radiale al Centro`).
   - Angolo di riposo per polveri, dispersione idrostatica per liquidi, scambio per densità (`Metallo Fuso > Lava > Sabbia > Acido > Acqua Salata > Acqua > Olio > Neve > Gas`) e trasferimento di quantità di moto negli urti.
5. **Trasporto Radiativo della Luce e Rendering (`src/engine/ThermoElectric.cpp`, `src/engine/Renderer.cpp`, `src/app/GlShaderPipeline.cpp`)**:
   - Diffusione RGB per-pixel modulata dalla trasmittanza di ciascun materiale e radiazione di corpo nero in funzione della temperatura `T` (Kelvin).
   - Texture procedurali per tutti i 28 materiali con sfondo atmosferico, normali 2.5D e proprietà ottiche condivise (`include/sim/MaterialOptics.hpp`).
   - In modalità `1 Fisico`, pipeline shader multi-pass **OpenGL 3.3 Core** (ombre 2D direzionali/locali, rifrazione di Snell, assorbimento Beer-Lambert, caustiche, heat-haze, bloom HDR e tone mapping ACES Filmic) con fallback automatico su renderer CPU (`Renderer.cpp`).
   - 5 modalità di visualizzazione in tempo reale: `1 Fisico`, `2 Termico (Inferno)`, `3 Pressione`, `4 Velocità`, `5 Luce/Carica`.

## Materiali & Strumenti

- **28 Materiali**: Aria, Sabbia, Acqua, Acqua Salata, Sale, Pietra, Legno, Metallo, Metallo Fuso, Vetro, Vetro Fuso, Ghiaccio, Neve, Pianta, Olio, Acido, Lava, Ossidiana, Polvere da Sparo, Nitroglicerina, Fuoco, Fumo, Vapore, Metano, Scintilla, Plasma, Batteria (4V), Muro Indistruttibile.
- **8 Strumenti Fisici**: Pittura, Vento, Calore, Freddo, Scintilla, Attrattore, Repulsore, Gomma.
- **6 Preset**: Laboratorio (`lab`), Vulcano (`volcano`), Idrodinamica (`hydro`), Esplosivi (`explosives`), Circuito Elettrico (`electric`), Orbita Zero-G (`zerog`).

## Controlli (Interfaccia SDL3 + Dear ImGui + OpenGL 3.3)

- **Click sinistro / trascinamento**: applica strumento o materiale selezionato
- **Click destro / trascinamento**: cancella ad aria (Gomma)
- **`Shift + Drag` (sinistro o destro)**: traccia una linea retta con anteprima geometrica (`Esc` annulla)
- **Click centrale o `Alt + Click` sinistro**: pipetta (campiona materiale sotto il cursore)
- **Rotella mouse o `[` / `]`**: dimensione pennello (`0..35 px`)
- **`Spazio`**: Pausa / Riprendi | **`N`**: Passo +1 tick (pausa) | **`C`**: Pulisci | **`R`**: Reset preset
- **`S`**: Esporta snapshot PPM (`snapshot_tick_<tick>.ppm`)
- **`1..5`**: cambia modalità di visualizzazione (`1 Fisico`, `2 Termico`, `3 Pressione`, `4 Velocita`, `5 Luce`)
- **`F6`**: attiva/disattiva la pipeline shader OpenGL 3.3 Core (`Shader GL3: ON` / `OFF`) in vista `1 Fisico`
- **Sezione `SHADER & LUCE (F6)` (sidebar)**: toggle `Shader GL3`, `Ombre 2D`, `Rifrazione`, `Bloom HDR` e slider `Luce Amb.` (`0.05..1.00`), `Sole` (`0.00..1.50`), `Bloom` (`0.00..1.50`)
- **`G`**: cicla modalità gravità (`Terra`, `Zero-G`, `Inversa`, `Centro`) | **`B`**: bordi chiusi/aperti | **`Esc`**: annulla linea attiva o chiude l'app

## Compilazione ed Esecuzione

Richiede CMake (`>= 3.21`; `>= 3.24` su macOS), Ninja e un compilatore C++20/C (Clang/LLVM-MinGW, GCC, MSVC o AppleClang).
Con `ELEMENTA_BUILD_GUI=ON` (default), CMake scarica e compila automaticamente tramite `FetchContent` **SDL3** (`release-3.4.20`, statico) e **Dear ImGui** (`v1.93.0-docking`), collegandosi al driver **OpenGL** di sistema (`opengl32` su Windows, `OpenGL.framework` su macOS, Mesa/GLVND + X11/Wayland dev headers su Linux). I target `sim_core` e `pixel_sim_tests` restano interamente privi di dipendenze SDL/ImGui/OpenGL.

```powershell
# Configurazione e compilazione tramite CMakePresets (es. Windows Clang/LLVM-MinGW Release)
cmake --preset win-clang-release
cmake --build --preset win-clang-release
```

Preset disponibili in `CMakePresets.json`:
- Windows: `win-clang-release`, `win-clang-debug`, `win-msvc-release`, `win-msvc-debug`
- Linux: `linux-gcc-release`, `linux-clang-release`, `linux-asan-ubsan`
- macOS: `macos-clang-release`

Su Ubuntu/Debian, installare prima i pacchetti di sviluppo X11/Wayland/OpenGL per SDL3:

```bash
sudo apt-get install -y ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libgl1-mesa-dev libegl1-mesa-dev libwayland-dev libdecor-0-dev libdbus-1-dev
```

Per compilare una build solo CLI headless senza scaricare SDL3/Dear ImGui né richiedere OpenGL:

```powershell
cmake -B build-cli -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DELEMENTA_BUILD_GUI=OFF
cmake --build build-cli
```

### Eseguire i test fisici

```powershell
ctest --preset win-clang-release
```

### Avvio GUI o modalità CLI

```powershell
# Avvia il simulatore interattivo SDL3 + Dear ImGui + OpenGL 3.3
.\build\win-clang-release\pixel_sim.exe

# Opzioni CLI (benchmark headless o snapshot PPM, disponibili su tutte le piattaforme)
.\build\win-clang-release\pixel_sim.exe --benchmark 150
.\build\win-clang-release\pixel_sim.exe --width 640 --height 360 --preset volcano
.\build\win-clang-release\pixel_sim.exe --preset lab --view thermal --dump-ppm 100 snapshot.ppm
```
