# Gas e liquidi continui

Obiettivo: gas e liquidi devono avere aspetto e movimento continui come il
campo di vento e le vampe termiche. La griglia resta un dominio numerico:
una cella non rappresenta una particella indivisibile di fluido.

L'ottimizzazione successiva, i confronti riproducibili e i limiti prestazionali
sono documentati in [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md).

## Requisiti e verifica (aggiornamento 12 novembre 2026)

1. Quantità frazionarie e miscele per tutte le specie liquide e gassose.
   `ContinuumTransport` mantiene quantità, momento termico ed età per specie.
   L'aria trasporta la propria energia. Verificati frazioni, coesistenza,
   modifica tramite pennello/gomma e ricostruzione della vista dominante.
2. Trasporto conservativo con velocità di faccia, sottopassi CFL, positività,
   pareti impermeabili e uscita dai bordi aperti.
   Test su tutte le specie, miscele ad alto Courant, pareti sottili e outflow;
   confronto seriale/parallelo di quantità, calore ed età.
3. Gas trasportati dal vento con diffusione conservativa e convezione termica.
   Il movimento discreto dei fluidi è escluso dal ciclo delle particelle.
   Test di vortice con posizione confrontata con la rotazione analitica,
   diffusione senza vento e pennacchio caldo/freddo a 80 tick:
   baricentri verticali 29,9543 / 41,5; quantità di fumo 32 conservata.
4. Liquidi con superficie libera, gravità, viscosità e separazione per densità.
   Quantità di moto MAC con avvezione RK2 e viscosità implicita;
   pressione a densità variabile risolta con CG, MIC(0) e spazio grossolano
   separato per componente connessa; SSOR per forti contrasti di densità.
   Le camere chiuse hanno riferimenti di pressione e compatibilità Neumann.
   La pressione atmosferica è applicata alla superficie effettiva del liquido.
   Il trasporto liquido usa ricostruzione geometrica PLIC e limita i flussi
   condivisi al volume ricevente disponibile, preservando specie e momenti.
   La correzione di capacità usa push/relabel per raggiungere lo spazio
   disponibile, evitando la propagazione ripetuta lungo circuiti pieni.
   Il limite uniforme delle velocità conserva il vincolo di divergenza.
   Test di equilibrio idrostatico, camere separate, viscosità, pareti,
   apertura di colonna d'acqua (80 tick, volume 104 conservato),
   stratificazione con gravità terrestre e inversa. Verificata anche caduta
   e raccolta compatta di 64 unità in una vasca con entrambe le gravità,
   traslazione di una miscela senza diluizione e correnti opposte senza
   sovrariempimento del ricevente.
5. Calore, transizioni di fase, combustione, esplosioni e prodotti solidi
   preservano le quantità frazionarie e gli altri componenti della miscela.
   Le reazioni visitano anche i componenti minoritari, rinviando i nuovi
   prodotti al tick successivo; la vista dominante non possiede la massa.
   Verificati ebollizione frazionaria, solidificazione del metallo minoritario,
   combustione dell'olio e conservazione termica acqua/olio/aria
   (variazione +0,000689057 nel caso di 30 tick della Release locale).
   Corretto il quenching ripetuto della lava fredda che poteva sottrarre
   calore fino a temperature negative. Le emissioni interne e le esplosioni
   trasformano frazioni senza cancellare gli altri componenti.
6. Rendering CPU/GPU di colore premoltiplicato e copertura derivati dalle
   concentrazioni, con interpolazione lineare e raccordo spaziale.
   Gli identificatori dei materiali non vengono interpolati.
   Solidi, polveri ed energia conservano la propria resa.
   Verificata copertura frazionaria in test e ispezionate le catture GPU
   effettive a 480×270 su RTX 4070 Ti / OpenGL 3.3:
   `build/performance-hydro.png` (120 tick) e
   `build/performance-lab.png` (60 tick).
7. Build GUI Release aggiornata e modalità continua predefinita, senza flag.
   `--continuum` è esplicito ma facoltativo; `--legacy-particles` serve
   per confronto. CTest verifica entrambe le scelte.

## Comandi ed evidenze

- `cmake --build build/win-clang-release -j 4`: completato.
- `ctest --test-dir build/win-clang-release --output-on-failure`:
  9/9 target superati. Log:
  `build/win-clang-release/Testing/Temporary/LastTest.log`.
- `ContinuumNormalResolution`: tutti e sei i preset a 480×270 per 60 tick,
  controllo del riempimento ad ogni tick e finitezza/positività finale.
  Massimi finali liquidi, in ordine lab/volcano/hydro/explosives/electric/zerog:
  1,00006; 1,00008; 1,00004; 0; 1,00001; 1,00001.
  Tolleranza del test 1,02; non si tratta di una garanzia per ogni scena futura.
- Build `build/strict-cli` con `ELEMENTA_WARNINGS_AS_ERRORS=ON`:
  compilazione generica senza IPO completata, 8/8 test non estesi superati.
- ASan + UBSan: `ContinuumTransportTests` superato in Docker Desktop locale,
  contesto `desktop-linux`, endpoint Docker Desktop verificato.
  Immagine `simulazioni-debug:local`, sorgenti montati in sola lettura.
  GCC 12, RelWithDebInfo, `-fno-pie` / `-no-pie`;
  `ASAN_OPTIONS=handle_segv=0:detect_leaks=1:halt_on_error=1`,
  `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.
  Le opzioni evitano il problema del gestore SIGSEGV ASan/PIE osservato
  nel runtime locale, mantenendo attivi i controlli strumentati.
  Verifica finale: 6,07 secondi, nessun errore;
  log persistente `build/performance-sanitizers.log`.
- Catture aggiornate dalla GUI reale dopo l'ottimizzazione:
  `pixel_sim.exe --preset hydro --dump-gpu-ppm 120 build/performance-hydro.ppm`
  e preset `lab` a 60 tick. Verificati i PNG ottenuti con sola conversione PPM.
- Benchmark finale, stessa macchina, hydro 480×270, 120 tick, include rendering
  CPU: 77,9193 ms/tick rispetto a 197,986 della build precedente.
  Momento 7,71 ms, pressione 13,94 ms, trasporto 35,87 ms, vista 0,68 ms.
  Il confronto a 60 tick di tutti i preset misurati è nel report prestazionale.
  La GUI limita a un tick continuo per frame per evitare raffiche di
  recupero che blocchino l'interfaccia.

La copia `build/pixel_sim.exe` è sincronizzata con la Release verificata:
hash SHA256 identici e avvio CLI controllato. La versione precedente alla
correzione è conservata in `build/pixel_sim.exe.before-liquid-surface-fix.bak`;
la versione prima dell'ottimizzazione è in `build/pixel_sim.exe.before-performance.bak`.

## Limiti del modello

La GPU accelera il rendering OpenGL e, dopo una verifica numerica iniziale,
il moto e la viscosità dei liquidi. Pressione e trasporto delle quantità
restano sulla CPU; il calcolo del moto torna alla CPU se l'acceleratore fallisce.
Le prestazioni dipendono dalla scena; Zero-G resta particolarmente pesante.
Il benchmark misura simulazione e rendering CPU; non promette 60 FPS della GUI.
La pressione è iterativa e ha tolleranza numerica finita.
I volumi equivalenti acqua/vapore restano quelli del modello ludico:
non è implementata l'espansione fisica da densità di fase.
Solidi, polveri ed energia mantengono cinematica discreta; i loro prodotti
frazionari preservano la quantità nei campi di stato.
Il trasporto generico delle concentrazioni consente accumuli in un campo
convergente. I liquidi del runtime usano il percorso dedicato `advectLiquids`,
con volume limitato, insieme alla proiezione di pressione.

## Regressione: liquidi simili a gas

La prova di caduta nella vasca mancava nella prima verifica: il vecchio
trasporto conservava la quantità ma la diluiva, e la fascia di pressione
estesa nell'aria la sosteneva. Dopo 160 tick soltanto 6,14684 delle 64 unità
erano nei quattro strati sul fondo. Rimossa la fascia e introdotto il
trasporto a interfaccia con flussi limitati, la nuova prova di assestamento
a 240 tick conserva circa 63,9999 unità e le raccoglie tutte sul fondo.
Verificati anche gravità inversa e differenza tra altezze delle colonne
inferiore a 1,1 celle.

Una seconda regressione riproduce l'uscita simultanea da quattro facce di
una specie presente in quantità subnormale: sottrazioni float separate
potevano produrre un volume di -1,4013e-45 in Zero-G. Il nuovo aggiornamento
somma contributi non negativi e conserva la positività anche in questo caso.
