# Simulatore fisico 2D: fluidi continui e particelle (C++20)

Motore di simulazione fisica 2D in C++20 in cui **ogni singolo pixel** della griglia (`480x270` di default = `129.600` pixel, configurabile) viene calcolato e simulato ad ogni tick su più strati fisici accoppiati, con interfaccia grafica cross-platform **SDL3 + Dear ImGui + OpenGL 3.3** (Windows, Linux, macOS) e core di simulazione/test puro C++20 privo di dipendenze esterne.

Gas e liquidi usano di default campi continui di concentrazione e volume:
più specie possono coesistere nella stessa cella e muoversi per frazioni.
Solidi, polveri ed energia mantengono il modello a particelle. La griglia
numerica non rappresenta più particelle indivisibili di liquido o gas.

## Architettura

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
   - In modalità predefinita gestisce polveri ed energia; gas e liquidi sono trasportati dai campi continui in `ContinuumWorld.cpp`.
   - Integrazione sub-pixel `(fx, fy)` e raycast DDA multi-step (anti-tunneling per particelle veloci).
   - Gradiente di pressione (`-∇P / ρ`), resistenza aerodinamica dall'aria, gravità configurabile (`Terra`, `Zero-G`, `Inversa`, `Radiale al Centro`).
   - Angolo di riposo per polveri e trasferimento di quantità di moto negli urti. La dispersione a particelle dei liquidi è disponibile solo nel modello legacy.
5. **Trasporto Radiativo della Luce e Rendering (`src/engine/ThermoElectric.cpp`, `src/engine/Renderer.cpp`, `src/app/GlShaderPipeline.cpp`)**:
   - Diffusione RGB per-pixel modulata dalla trasmittanza di ciascun materiale e radiazione di corpo nero in funzione della temperatura `T` (Kelvin).
   - Texture procedurali per tutti i 28 materiali con sfondo atmosferico, normali 2.5D e proprietà ottiche condivise (`include/sim/MaterialOptics.hpp`).
   - In modalità `1 Fisico`, pipeline shader multi-pass **OpenGL 3.3 Core** (ombre 2D direzionali/locali, rifrazione di Snell, assorbimento Beer-Lambert, caustiche, heat-haze, bloom HDR e tone mapping ACES Filmic) con fallback automatico su renderer CPU (`Renderer.cpp`).
   - 5 modalità di visualizzazione in tempo reale: `1 Fisico`, `2 Termico (Inferno)`, `3 Pressione`, `4 Velocità`, `5 Luce/Carica`.

## Materiali & Strumenti

Tutti i liquidi (inclusi lava e materiali fusi) e i gas usano quantità
frazionarie, trasporto conservativo, calore ed età trasportati con la miscela.
I gas seguono il vento, si disperdono e rispondono al galleggiamento termico.
I liquidi hanno velocità di faccia, proiezione di pressione a densità variabile,
superficie libera, viscosità e separazione per densità lungo la gravità.
Il loro trasporto ricostruisce un'interfaccia lineare a volume finito (PLIC): il flusso
condiviso delle specie è limitato dal volume disponibile nelle celle riceventi.
La pressione atmosferica è applicata alla superficie effettiva del liquido.
Questo permette caduta e raccolta sul fondo senza diluire l'acqua in una nube;
il raccordo grafico morbido resta separato dal trasporto fisico.

In vista Fisico CPU e GPU interpolano colore premoltiplicato e copertura
derivati dalle concentrazioni; non interpolano gli identificatori dei materiali.
Il raccordo visivo esclude solidi e polveri e rispetta le pareti. La GPU esegue
il rendering OpenGL; la simulazione fisica usa la CPU con più thread.
`--legacy-particles` permette il confronto con il vecchio modello discreto;
`--continuum` seleziona esplicitamente il modello già predefinito.

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

La suite include controlli su fumo caldo/freddo con gravità terrestre, inversa e radiale,
contenimento del fumo dietro pareti spesse un pixel, durata del fumo, conservazione
di acqua/olio e stratificazione dei liquidi con gravità terrestre e inversa.
La suite continua controlla concentrazioni, conservazione, pareti, vortici,
diffusione senza vento, pennacchi termici, viscosità, superficie libera,
separazione per densità e reazioni dei componenti minoritari delle miscele.
Una regressione dedicata verifica caduta, raccolta e compattezza di 64 unità
d'acqua in una vasca, con gravità terrestre e inversa. Un secondo caso
trasla una miscela acqua/olio conservandone corpo, quantità, calore ed età;
due correnti opposte verificano il limite di riempimento della stessa cella.
Il fumo ha una concentrazione continua e una durata trasportata: non viene
ringiovanito quando entra in una nuova cella. Il test esteso esegue tutti i
sei preset a 480×270 per 60 tick e controlla il riempimento dei liquidi.

```powershell
ctest --preset win-clang-release
```

Per il controllo prolungato dei sei preset (600 passi ciascuno a 480×270):

```powershell
.\build\win-clang-release\continuum_tests.exe --long-run
```

Questa prova CPU verifica convergenza della pressione, limite di riempimento
e validità dello stato finale. Non misura gli FPS né sostituisce il confronto
con casi fisici di riferimento.

Il test `ContinuumRotationAccuracy` confronta la forma di un disco liquido
dopo un giro completo con la forma iniziale, a due risoluzioni. Il trasporto
include lo spostamento trasversale durante il passo e una stima della normale
su 3×3 celle, preservando le superfici rettilinee. I risultati e i limiti della
verifica sono in [DEBUG_AUDIT_REPORT.md](DEBUG_AUDIT_REPORT.md).

### Avvio GUI o modalità CLI

```powershell
# Avvia il simulatore interattivo SDL3 + Dear ImGui + OpenGL 3.3
.\build\win-clang-release\pixel_sim.exe

# Opzioni CLI (benchmark headless o snapshot PPM, disponibili su tutte le piattaforme)
.\build\win-clang-release\pixel_sim.exe --benchmark 150
.\build\win-clang-release\pixel_sim.exe --width 640 --height 360 --preset volcano
.\build\win-clang-release\pixel_sim.exe --preset lab --view thermal --dump-ppm 100 snapshot.ppm

# Esporta la vista fisica GPU (richiede una sessione grafica OpenGL)
.\build\win-clang-release\pixel_sim.exe --preset hydro --dump-gpu-ppm 60 hydro-gpu.ppm
```

Avviare la build nella cartella del preset scelto: altre copie di `pixel_sim.exe`
in `build/` possono appartenere a compilazioni precedenti. Le esportazioni
`--dump-ppm` usano il renderer CPU, mentre `--dump-gpu-ppm` cattura la pipeline
GPU della GUI e può quindi mostrare illuminazione ed effetti diversi.

La copia locale `build/pixel_sim.exe` è stata sincronizzata con la Release
verificata; il precedente eseguibile è conservato come
`build/pixel_sim.exe.pre-continuum.bak`. Risultati e limiti del modello sono
riportati in [CONTINUUM_PLAN.md](CONTINUUM_PLAN.md).

## Prestazioni

La GUI accelera automaticamente sulla GPU l'avvezione della velocità e la
viscosità, dopo una verifica numerica del driver. Pressione e trasporto delle
quantità restano sulla CPU. Se l'accelerazione fallisce, il calcolo riprende
sulla CPU; `--cpu-fluid` permette di disattivarla esplicitamente.
Il benchmark usa la CPU per impostazione predefinita: aggiungere `--gpu-fluid`
per misurare anche il percorso GPU, che richiede una sessione OpenGL.
`--test-gpu-momentum` confronta i risultati con la CPU e `--test-gpu-world`
controlla i sei preset a 480×270 per 60 passi ciascuno.
Lo script di confronto accetta `-GpuFluid` e `-BaselineGpuFluid` per scegliere
separatamente i due percorsi, anche usando lo stesso eseguibile.

La Release usa l'ottimizzazione tra unità di compilazione quando supportata
(`ELEMENTA_ENABLE_IPO=ON`). Con Clang/GCC è possibile ottimizzare anche per
il processore della macchina locale:

```powershell
cmake --preset win-clang-release -DELEMENTA_NATIVE_CPU=ON
cmake --build --preset win-clang-release
```

Per build destinate ad altri processori usare `ELEMENTA_NATIVE_CPU=OFF`,
valore predefinito del progetto. L'EXE locale è ottimizzato per il processore
su cui è stato verificato. La build precedente è conservata in
`build/pixel_sim.exe.before-performance.bak`.

Il confronto riproducibile mantiene la griglia 480×270 e salva tempi e hash:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\benchmark-performance.ps1 `
  -Exe .\build\win-clang-release\pixel_sim.exe `
  -BaselineExe .\build\pixel_sim.exe.before-performance.bak -Ticks 60
```

Lo script segnala regressioni superiori al 10%. I risultati includono
simulazione e rendering CPU headless; gli FPS della GUI dipendono anche dal
rendering OpenGL. Misure, verifiche e limiti sono in
[PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md).
