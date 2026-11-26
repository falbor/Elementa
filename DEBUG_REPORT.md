# Debug e correzioni — 11 novembre 2026

## Correzioni

- `ThermoElectric.cpp`: separati calcolo del calore Joule e decadimento dei
  potenziali. Il passaggio termico legge un campo di carica stabile; nessun
  thread modifica i potenziali mentre altri leggono i vicini. Conservate le
  costanti di decadimento di conduttori e isolanti.
- `SimWorld.cpp`: temperatura, velocità, carica e temperatura ambiente rifiutano
  NaN e infinito; i valori finiti restano limitati agli intervalli del motore.
- `SimWorld.cpp`: l'esportazione PPM verifica anche gli errori di chiusura e
  svuotamento del buffer del file.
- `main.cpp`: benchmark ed esportazione richiesti insieme producono un errore
  esplicito, senza ignorare silenziosamente l'esportazione.

## Verifiche completate

- Build Windows Clang Debug e Release con GUI SDL3/OpenGL.
- Suite CTest Debug e Release: test fisici, help CLI, rifiuto delle modalità
  incompatibili e rifiuto della larghezza non numerica.
- Confronto dei campi elettrici e termici tra esecuzione con uno e quattro thread.
- Build headless Windows con `ELEMENTA_WARNINGS_AS_ERRORS=ON`.
- Cppcheck: sorgenti del motore, applicazione, header e test; categorie warning,
  performance e portability, con error-exitcode abilitato. Nessuna segnalazione.
- Tutte le 30 combinazioni tra sei preset e cinque viste: esportazione PPM su
  griglia minima CLI 32×32 dopo tre tick.
- Avvio GUI dei sei preset: shader attivi su NVIDIA GeForce RTX 4070 Ti,
  OpenGL 3.3, nessun errore shader nei log durante il breve avvio.
- `git diff --check` senza errori.

## Controllo Linux

Build headless GCC 12.2 in un container Debian 12 temporaneo su Docker Desktop
locale (`desktop-linux`, endpoint `npipe:////./pipe/dockerDesktopLinuxEngine`).
Sorgenti montati in sola lettura; build e dipendenze nel container.
AddressSanitizer e UndefinedBehaviorSanitizer abilitati; controllo leak attivo.
Suite completata: 4/4 target CTest superati in 119,75 secondi, senza segnalazioni
ASan/UBSan. Include il caso Linux `/dev/full` per gli errori di chiusura PPM.
Il test fisico registra 31 casi (il caso `/dev/full` viene eseguito solo su Linux).

La prima esecuzione GCC/ASan ha prodotto un ciclo di `DEADLYSIGNAL` senza stack
trace utile ed è stata interrotta. La verifica riuscita usa una build separata
con `CMAKE_CXX_FLAGS=-fno-pie`, `CMAKE_EXE_LINKER_FLAGS=-no-pie` e
`ASAN_OPTIONS=handle_segv=0:detect_leaks=1:halt_on_error=1`;
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. Queste opzioni sono limitate
alla verifica nel container e non modificano i preset di produzione. Non è
stata determinata la causa del problema del runtime nella prima configurazione.
L'immagine locale `simulazioni-debug:local` conserva la toolchain di verifica.

## Limiti delle verifiche

Gli avvii GUI verificano inizializzazione, compilazione shader e rendering
iniziale; non sostituiscono una prova manuale prolungata di ogni interazione.
Non eseguite build native macOS o MSVC. Le modifiche preesistenti relative a
fluidi, fumo e resa visiva sono state conservate nel worktree.
