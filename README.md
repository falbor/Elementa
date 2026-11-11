# Simulatore Fisico di Particelle 2D Per-Pixel (C++20)

Motore di simulazione fisica 2D in C++20 in cui **ogni singolo pixel** della griglia (`480x270` di default = `129.600` pixel, configurabile) viene calcolato e simulato ad ogni tick su più strati fisici accoppiati, senza dipendenze esterne su Windows.

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
5. **Trasporto Radiativo della Luce e Rendering (`src/engine/ThermoElectric.cpp`, `src/engine/Renderer.cpp`)**:
   - Diffusione RGB per-pixel modulata dalla trasmittanza di ciascun materiale e radiazione di corpo nero in funzione della temperatura `T` (Kelvin).
   - 5 modalità di visualizzazione in tempo reale: `1 Fisico`, `2 Termico (Inferno)`, `3 Pressione`, `4 Velocità`, `5 Luce/Carica`.

## Materiali & Strumenti

- **28 Materiali**: Aria, Sabbia, Acqua, Acqua Salata, Sale, Pietra, Legno, Metallo, Metallo Fuso, Vetro, Vetro Fuso, Ghiaccio, Neve, Pianta, Olio, Acido, Lava, Ossidiana, Polvere da Sparo, Nitroglicerina, Fuoco, Fumo, Vapore, Metano, Scintilla, Plasma, Batteria (4V), Muro Indistruttibile.
- **8 Strumenti Fisici**: Pittura, Vento, Calore, Freddo, Scintilla, Attrattore, Repulsore, Gomma.
- **6 Preset**: Laboratorio (`lab`), Vulcano (`volcano`), Idrodinamica (`hydro`), Esplosivi (`explosives`), Circuito Elettrico (`electric`), Orbita Zero-G (`zerog`).

## Controlli (Interfaccia Win32)

- **Click sinistro / trascinamento**: applica strumento o materiale selezionato
- **Click destro / trascinamento**: cancella ad aria
- **Click centrale o `Alt + Click`**: pipetta (campiona materiale sotto il cursore)
- **Rotella mouse o `[` / `]`**: dimensione pennello (`0..35 px`)
- **`Spazio`**: Pausa / Riprendi | **`N`**: Avanza di 1 tick | **`C`**: Pulisci | **`R`**: Ricarica preset
- **`1..5`**: cambia modalità di visualizzazione | **`G`**: modalità gravità | **`B`**: bordi chiusi/aperti

## Compilazione ed Esecuzione

Richiede CMake (>= 3.20) e un compilatore C++20 su Windows (es. LLVM-MinGW `clang++` + Ninja):

```powershell
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Eseguire i test fisici

```powershell
ctest --test-dir build --output-on-failure
```

### Avvio GUI o modalità CLI

```powershell
# Avvia il simulatore interattivo Win32
.\build\pixel_sim.exe

# Opzioni CLI (benchmark headless o snapshot PPM)
.\build\pixel_sim.exe --benchmark 150
.\build\pixel_sim.exe --width 640 --height 360 --preset volcano
.\build\pixel_sim.exe --preset lab --view thermal --dump-ppm 100 snapshot.ppm
```
