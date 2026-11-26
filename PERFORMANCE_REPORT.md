# Ottimizzazione del simulatore — 12 novembre 2026

Misure su Intel Core i9-10900K, griglia **480×270**, modello continuo
predefinito. Ogni prova esegue simulazione e rendering CPU headless;
il valore in tick/s non rappresenta gli FPS della GUI OpenGL.

## Aggiornamento del 14 novembre: pressione in memoria contigua

Il risolutore delle regioni di liquido disconnesse usa buffer locali contigui e
coefficienti precalcolati per ridurre accessi indiretti e calcoli ripetuti.
Risoluzione, precisione richiesta e rendering restano quelli precedenti.

Mediane di tre esecuzioni alternate, 60 tick a 480?270, CPU:

| Preset | Prima, ms/tick | Dopo, ms/tick | Aumento passi/s |
|---|---:|---:|---:|
| Idrodinamica | 65,7570 | 66,2854 | -0,8% |
| Laboratorio | 78,4213 | 74,0668 | +5,9% |
| Zero-G | 85,2835 | 75,3695 | +13,2% |

Idrodinamica non attiva il percorso ottimizzato: la differenza resta sotto l'1%.
Il miglioramento non basta ancora per raggiungere 60 passi/s nelle scene pesanti.
Dati e hash: `build/compact-pressure-performance.json`. Non sono misure FPS GUI.
La GPU resta disponibile per il momento dei fluidi e per la visualizzazione;
il risolutore della pressione ottimizzato lavora sulla CPU anche in modalit? GPU.

## Aggiornamento del 13 novembre: correzione della capacità

Con il trasporto accurato già introdotto, la correzione cerca prima spazi liberi
superiori a 1e-4 celle e usa poi la soglia fine originale per l'eccesso residuo.
La tolleranza di volume rimane 2e-6; non vengono eliminate quantità di liquido.

Mediane di tre esecuzioni alternate, 60 tick a 480×270:

| Preset | Prima, ms/tick | Dopo, ms/tick |
|---|---:|---:|
| Idrodinamica | 72,3162 | 64,3518 |
| Laboratorio | 79,8721 | 78,1105 |
| Zero-G | 85,9023 | 85,7047 |

Il beneficio è circa l'11% in Idrodinamica, il 2% in Laboratorio e trascurabile
in Zero-G. Questi tempi restano troppo alti per una simulazione interattiva
a 60 passi al secondo. Dati: `build/spare-threshold-performance.json`.
Le verifiche di precisione e lo storico delle prove scartate sono in
[DEBUG_AUDIT_REPORT.md](DEBUG_AUDIT_REPORT.md).

## Confronto iniziale sulla stessa macchina (12 novembre)

| Preset, 60 tick | Prima, ms/tick | Dopo, ms/tick | Accelerazione |
|---|---:|---:|---:|
| Idrodinamica | 161,722 | 74,8567 | 2,16× |
| Laboratorio | 161,762 | 108,608 | 1,49× |
| Zero-G | 144,020 | 132,024 | 1,09× |

Nella prova più lunga di Idrodinamica, a **120 tick**, il costo medio scende
da **197,986 a 77,9193 ms/tick**, circa **2,54×**. La crescita progressiva del
costo del limitatore era particolarmente visibile dopo i primi tick.

Dati completi, hash degli eseguibili e budget di regressione:
`build/performance-comparison.json`. Il dettaglio della prova lunga è in
`build/performance-hydro-120.log`. La copia di riferimento è conservata in
`build/pixel_sim.exe.before-performance.bak`, con SHA256
`6ADDB2FA58D0CDDF3BE977494D6E4321C00F68DEB6A31BE488456461F209F6FB`.

## Cambiamenti mantenuti

- Il limitatore di volume usa instradamento dei residui con push/relabel e
  aggiornamento delle distanze. Le correzioni raggiungono la capacità libera
  senza ripassare proporzionalmente milioni di volte sugli stessi circuiti.
  I flussi corretti trasferiscono quantità, calore ed età delle specie.
- L'aggiornamento dei liquidi raccoglie i contributi in doppia precisione
  prima di memorizzarli nei campi float; le righe sono elaborate in parallelo.
- Il trasporto lavora nel rettangolo occupato, esteso di un numero di celle
  sufficiente per tutti i sottopassi CFL. Il rettangolo comprende anche le
  tracce positive: non viene applicata una soglia che elimini massa.
- La pressione usa MIC(0) e una correzione di Galerkin su basi bilineari
  indipendenti per componente connessa. Il sistema grossolano è limitato a
  128 incognite e alle 16 componenti grandi più estese; l'equazione fine
  continua a comprendere tutte le celle. Spazi mal condizionati usano il
  solo precondizionamento locale. Per forti contrasti di densità rimane il
  percorso SSOR. Il riferimento per MIC è costituito dalle
  [note di Robert Bridson](https://www.cs.ubc.ca/~rbridson/fluidsimulation/fluids_notes.pdf).
- Maschere e coefficienti della viscosità sono costruiti una volta per passo
  e riutilizzati nelle **12 iterazioni**. Le facce ferme e le zone senza
  diffusione evitano il lavoro ripetuto.
- Le reazioni riutilizzano gli indirizzi dei canali di tutte le specie,
  compresi i prodotti inizialmente assenti. Restano attive le reazioni dei
  componenti minoritari delle miscele.
- La Release abilita l'ottimizzazione tra unità di compilazione, quando
  supportata dalla toolchain. La build locale usa anche
  `ELEMENTA_NATIVE_CPU=ON` per questo processore. Il valore predefinito nel
  progetto è `OFF`, adatto alle build destinate ad altri processori.

La griglia, il passo temporale e la resa grafica sfumata sono mantenuti.
I controlli continuano a verificare compattezza, quantità, calore, pareti,
gravità inversa, reazioni e positività. Il limite originale di tolleranza
della pressione resta disponibile per le miscele; nelle altre condizioni
il runtime usa una tolleranza più stretta.

## Verifica

- Release Windows: **9/9 target CTest superati**, inclusi i sei preset
  a 480×270 per 60 tick. Log:
  `build/win-clang-release/Testing/Temporary/LastTest.log`.
- Build generica con `ELEMENTA_WARNINGS_AS_ERRORS=ON`, IPO e tuning nativo
  disattivati: **8/8 test non estesi superati**.
- ASan/UBSan, GCC 12, Docker Desktop locale `desktop-linux`, sorgenti in sola
  lettura: `ContinuumTransportTests` superato in **6,07 secondi**.
  Log: `build/performance-sanitizers.log`.
- Aggiunti casi con grandi camere disconnesse e contrasti di densità; il test
  verifica che il risolutore grossolano sia effettivamente esercitato.
- Aggiunta una vasca completamente piena soggetta a correnti arbitrarie,
  con parete di una cella: volume e specie conservati, risultato seriale e
  parallelo identico per quantità, calore ed età.
- Catture della Release su RTX 4070 Ti, shader OpenGL 3.3 attivi:
  [Idrodinamica a 120 tick](build/performance-hydro.png) e
  [Laboratorio a 60 tick](build/performance-lab.png), ispezionate dopo la build.
- `build/pixel_sim.exe` e `build/win-clang-release/pixel_sim.exe` sono
  sincronizzati: SHA256 identico e avvio CLI della copia principale verificato.

## Ripetere il confronto

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\benchmark-performance.ps1 `
  -Exe .\build\win-clang-release\pixel_sim.exe `
  -BaselineExe .\build\pixel_sim.exe.before-performance.bak -Ticks 60
```

Lo script esegue le prove in sequenza, salva il JSON e restituisce un errore
se una scena peggiora di oltre il **10%** rispetto all'eseguibile di riferimento.
La policy PowerShell viene impostata soltanto per quel processo.

## Limiti rimasti

Zero-G con molte specie e reazioni resta pesante. La GPU gestisce il rendering
OpenGL; la fisica è ancora sulla CPU. Queste misure non promettono 60 FPS.
I risultati dipendono da scena, numero di tick, hardware e carico della macchina.
