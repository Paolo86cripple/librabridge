# librabridge — librashader support for AGS (native Linux, no Wine)

Bridge minimo per far girare gli shader RetroArch (via librashader) sul
motore di Adventure Game Studio, senza Wine. Vedi `THIRD-PARTY-NOTICES.md`
per le licenze di librashader e AGS.

## Struttura del repo

- `librashader-integration.patch` — applicalo a un clone di
  `adventuregamestudio/ags`
- `ags-patch-files/` — le stesse modifiche come file standalone (comodo se
  il patch non applica pulito sulla tua revisione di AGS)
- `agssetup.cpp` — GUI di setup e launcher, C++/Qt6 (sostituisce winsetup.exe)
- `librashader-capi-headers/` — header C di librashader (vendorizzati, MIT,
  identici a quelli in `ags-patch-files/`), usati SOLO dalla preview shader di
  `agssetup.cpp`: la GUI apre `librashader.so` per conto suo, in un processo
  separato dal motore, per non dover avviare il gioco solo per controllare uno
  shader
- `build_librashader.sh` — compila `librashader.so` da sorgente

## Cosa c'è qui

- `librashader-integration.patch` — patch contro `adventuregamestudio/ags`
  (branch corrente al 17/09/2026). Aggiunge:
  - `Engine/gfx/librashader_gl.h` / `.cpp` — bridge minimo, ~140 righe.
    Non implementa NESSUNA logica di shader: carica `librashader.so` a
    runtime (dlopen, via l'header ufficiale `librashader_ld.h`) e chiama
    la sua API C per creare la filter chain da un preset e farla girare
    frame per frame. Tutta la logica reale (parsing `.slangp`/`.glslp`,
    pass multipli, feedback, history, LUT, parametri) è di librashader,
    non nostra.
  - `Engine/gfx/librashader/` — header ufficiali di librashader (MIT),
    vendorizzati invariati, solo per compilare il bridge.
  - Modifiche chirurgiche a `Engine/gfx/ali3dogl.h`/`.cpp` (il renderer
    OpenGL di AGS): ~90 righe, tutte dietro un controllo — se non è
    configurato nessun preset, il comportamento è bit-per-bit identico a
    prima.
- `build_librashader.sh` — compila `librashader.so` da sorgente (nessun
  binario precompilato incluso).
- `agssetup.cpp` — GUI C++/Qt6 che sostituisce winsetup.exe e funziona da
  launcher: si punta a dati di gioco già esistenti con **Browse...**, si
  regolano grafica/audio/preset shader e si avvia il gioco con il motore
  compilato in dotazione (vedi "Usare la GUI").

## Come funziona (l'architettura, non solo il "cosa")

Ho letto il sorgente vero di AGS e di librashader (non la sola
documentazione) per trovare l'punto di aggancio giusto:

1. **Il contesto GL.** AGS chiede di default un contesto OpenGL 2.1
   compatibility. librashader (runtime GL) richiede 3.3+. La patch alza
   la richiesta a 3.3 **solo** se la variabile d'ambiente
   `AGS_LIBRASHADER_PRESET` è impostata — altrimenti resta 2.1 come
   sempre. Buona notizia verificata leggendo il codice: il renderer OGL
   di AGS usa già shader GLSL e VBO (via glad), non fixed-function, quindi
   il salto di versione non rompe nulla del suo rendering.
2. **Input dello shader.** AGS ha già una modalità "render to texture" in
   cui disegna il frame a risoluzione nativa del gioco dentro
   `_nativeSurface` (una texture+FBO), poi la ridisegna scalata sullo
   schermo reale. La patch forza questa modalità quando un preset è
   attivo — è esattamente l'immagine "a risoluzione nativa del core" che
   una filter chain in stile RetroArch si aspetta in input.
3. **Output dello shader.** Viene creata una seconda texture+FBO
   (`_librashaderTarget`), della dimensione dell'area di gioco sullo
   schermo (`_dstRect`, quindi le proporzioni si mantengono), con la STESSA
   funzione (`CreateRenderTargetDDB`) che AGS già usa per `_nativeSurface`.
   `libra_gl_filter_chain_frame()` scrive lì dentro.
4. **Presentazione.** Il blit finale che AGS già faceva
   (`RenderTexture(...)`) viene puntato sulla nuova texture quando lo
   shader è attivo, con una proiezione della dimensione della texture
   stessa (quella di `_screenBackbuffer` è una griglia della risoluzione
   nativa del gioco): zero codice nuovo per il blit in sé.

Il vecchio prototipo (LD_PRELOAD + renderchain scritta a mano) doveva
reimplementare tutta la parte 4 di RetroArch da zero: alias, parametri,
LUT, feedback, FBO float/sRGB. Qui quella parte non esiste proprio,
la fa librashader.

## Cosa ho verificato per davvero, e cosa no

Verificato in questo ambiente (container sandbox, niente GPU/display):
- Il patch **compila** con `g++ -fsyntax-only` contro l'albero sorgente
  reale di AGS (glm, allegro, glad, SDL2 vendorizzati inclusi) — sia il
  bridge nuovo che `ali3dogl.cpp`/`.h` modificati.
- Le firme della API C di librashader (struct `libra_image_gl_t`,
  `filter_chain_gl_opt_t`, campi di `libra_instance_t`) sono state lette
  dal sorgente Rust reale (`librashader-capi/src/runtime/gl/filter_chain.rs`),
  non dalla sola documentazione (che in un punto è disallineata dal
  codice attuale).
- `agssetup.cpp` è stato testato funzionalmente (headless,
  `QT_QPA_PLATFORM=offscreen`) con un motore fittizio al posto di `ags`:
  seleziona un gioco (cartella o file `.ags`), parte dal suo `acsetup.cfg`
  senza modificarlo, salva la config per-gioco, lancia il motore fittizio e
  verifica argomenti (`--conf <cfg> <gioco> ...`), cartella di lavoro,
  `LD_LIBRARY_PATH` e `AGS_LIBRASHADER_PRESET`, e che le cartelle dei giochi
  restino byte-per-byte invariate. Con una config ricca (46 chiavi, anche non
  gestite e con valori fuori lista o fuori range) un salvataggio senza
  modifiche le conserva tutte, e un `acsetup.cfg` in Latin-1 (come quelli dei
  vecchi giochi Windows) non perde gli accenti; cambiando ogni opzione,
  ognuna finisce nella chiave giusta.
  Ogni chiave che la GUI scrive è una che il parser del motore
  (`Engine/main/config.cpp`) legge davvero, e le 29 chiavi che winsetup
  scrive sono tutte coperte. Il motore vero non è stato eseguito, quindi le
  voci del menu Diagnostics (`--tell-*`) non sono verificate con esso.
  Stessa batteria ripetuta per Recent/Profile/Scan a folder/Diagnostics
  annullabile: `recentGames()` trova il gioco appena salvato, un profilo
  esportato non contiene il percorso del gioco e riapplicato ripristina
  esattamente i campi salvati, `looksLikeGame()` distingue correttamente
  cartelle valide da vuote/inesistenti, e annullare una diagnostica bloccata
  (motore fittizio che non risponde) interrompe il processo invece di
  aspettare i 20 secondi. ASan/UBSan puliti su tutti questi percorsi.
- La **preview shader** è stata provata con un contesto OpenGL vero (Xvfb +
  `QT_QPA_PLATFORM=xcb`, Mesa software) e lo stesso `librashader.so`
  compilato da sorgente usato per il resto di questo lavoro: carica preset
  veri (stock, crt-potato, history, feedback), mostra il messaggio d'errore
  vero di librashader per un preset non valido (via `libra_error_write`, non
  solo un messaggio generico), e passare da un preset a un altro nella
  stessa sessione non lascia artefatti. Da lì sono usciti due bug, corretti:
  `gl_filter_chain_frame` lascia legati il proprio framebuffer e viewport
  invece di ripristinare quelli del chiamante, per cui il blit successivo
  finiva altrove o in un angolo del widget; e su una sessione senza vero
  supporto OpenGL (riprodotto con `QT_QPA_PLATFORM=offscreen`, dove va in
  crash perfino un `QOpenGLWidget` vuoto — bug di Qt, non di questo codice)
  la GUI ora non crea affatto il widget GL, mostrando un messaggio al posto
  della preview invece di trascinare giù tutta la finestra delle opzioni.
  Verificato pulito anche sotto ASan/UBSan (un leak riportato è dentro
  `libdbus`, non nel codice di questo progetto).

NON verificato (serve una macchina vera, con GPU):
- L'esecuzione del motore AGS vero: la CI lo compila e lo linka, ma io
  non l'ho mai avviato con un gioco.
- Il rendering su una GPU vera. Ho verificato solo in software (Mesa
  llvmpipe su Xvfb) con un piccolo programma di prova che replica la
  sequenza GL di AGS (texture nativa → librashader → blit) usando il
  bridge vero e un `librashader.so` compilato da master. Da lì sono
  usciti quattro bug, tutti corretti nel patch: la texture di input, con un
  solo livello mip, risultava incompleta per il sampler di librashader e
  faceva uscire nero l'intera catena; il formato `GL_RGBA` (non
  dimensionato) faceva fallire i preset che usano la history dei frame;
  il blit finale usava la proiezione della griglia nativa del gioco per
  una texture in pixel dello schermo, mostrando solo un angolo
  ingrandito; e la versione GLSL era fissa a 330, per cui i preset che
  usano funzioni più recenti (es. `packUnorm4x8`, GLSL 4.00) non
  compilavano: ora si usa la versione massima del contesto GL.
- Il ramo GLES2 (mobile) di `ali3dogl.cpp`: la patch lo lascia
  sintatticamente intatto (la nuova chiamata è dietro `#if
  !AGS_OPENGL_ES2`) ma non l'ho compilato con quel flag.

## Versione minima di Rust

`librashader` richiede Rust ≥ 1.87 (una sua dipendenza, `naga` 30, si
rifiuta di compilare con compilatori più vecchi). Su CachyOS basta
`pacman -S rust`. Su Ubuntu 24.04 il `rustc` di default è troppo vecchio,
ma `apt install rustc-1.91 cargo-1.91` funziona (i binari stanno in
`/usr/lib/rust-1.91/bin`).

## Come procedere sul tuo repo

```
git clone https://github.com/adventuregamestudio/ags.git
cd ags
git apply /path/to/librashader-integration.patch
./build_librashader.sh Engine/  # o dove finisce il binario dopo la build cmake
# build normale di AGS con cmake, come sempre
```

Poi lancia un gioco con:
```
LD_LIBRARY_PATH=/cartella/con/librashader.so \
AGS_LIBRASHADER_PRESET=/path/a/crt-royale.slangp ./ags /path/al/gioco
```
oppure usa la GUI (sotto), che imposta tutto da sola.

## Revisioni fissate e canary notturno

Le build su `push` e quelle lanciate a mano compilano revisioni **fissate** di
AGS e librashader (`AGS_PINNED` e `LIBRASHADER_PINNED` in
`.github/workflows/build.yml`): il patch è scritto contro quell'AGS e il bridge
chiama l'API C di librashader tramite un header copiato nel repo, quindi le due
cose devono muoversi insieme al patch. Ogni notte la CI compila invece la punta
di `master` di entrambi e pubblica un artifact separato,
`ags-librashader-linux-x86_64-canary`: se diventa rosso, qualcosa a monte è
cambiato (il patch non applica più, o l'ABI di librashader è cambiata) e lo
vedi prima che ti arrivi una build rotta.

Per aggiornare le revisioni: guarda cosa ha cambiato il canary, verifica che
`git apply --check librashader-integration.patch` passi sulla nuova revisione di
AGS (rifacendo il patch se serve) e che la build e un gioco funzionino, poi
cambia i due SHA nel workflow. Da *Actions → Run workflow* puoi anche compilare
una revisione qualsiasi senza toccare il file (campi `ags_ref` e `librashader_ref`).

## Release

Un tag `v*` (es. `git tag v1.0.0 && git push origin v1.0.0`) fa compilare **sia**
la build stabile **che** il canary nello stesso run, e pubblica una GitHub
Release con entrambi i tarball allegati — scaricabili direttamente, senza dover
essere loggati su GitHub come serve invece per gli artifact di Actions. Se il
canary fallisce (il suo scopo è poter fallire quando qualcosa a monte cambia),
la release parte comunque con la sola build stabile; fallisce solo se non è
disponibile nessuna delle due.

## Usare la GUI (agssetup)

La CI produce un artifact `ags-librashader-linux-x86_64`: uno zip che contiene
`ags-librashader-linux-x86_64.tar.gz`. Estrailo con `tar xf` (il tarball conserva
i permessi di esecuzione, lo zip no) e ottieni la cartella `ags-librashader/` con
`ags` (il motore con il bridge), `librashader.so`, `agssetup` e `BUILD_INFO.txt`
(quali revisioni di AGS e librashader sono state compilate). Tieni i file
insieme nella stessa cartella.

1. Avvia `./agssetup`.
2. **Browse...** → *Game folder...*, *Game data file...* (`.ags`, `.exe`,
   `ac2game.dat`) oppure *Scan a folder for games...* (utile per una libreria
   con tanti giochi: cerca nella cartella scelta e un livello sotto, poi fai
   scegliere quale aprire). **Recent...**, accanto a Browse, riapre in un
   clic un gioco già configurato prima.
3. Regola le opzioni nelle schede, poi **Save && Play**.

Le schede coprono tutto ciò che offre winsetup.exe e altro:

- **Graphics** — monitor, renderer (OpenGL/Software), filtro di scaling,
  finestra (dimensione e scaling), schermo intero (modo e scaling),
  refresh, vsync, sprite a risoluzione schermo, antialias, **contatore FPS**.
- **Shader** — preset librashader, con interruttore per escluderlo senza
  perdere la selezione, e una **Preview**: un riquadro OpenGL indipendente dal
  gioco e dal motore (apre `librashader.so` per conto proprio) che applica il
  preset a un pattern di prova, con **Refresh preview** per aggiornarlo. Utile
  per controllare che un preset carichi senza dover avviare il gioco; se fallisce
  mostra l'errore vero e proprio di librashader. Su una sessione senza OpenGL
  (rarissimo su un desktop reale) la preview si disattiva da sola con un
  messaggio, invece di piantarsi.
- **Audio** — suono, driver, voice pack, cache dei suoni e soglia di
  caricamento.
- **Controls** — mouse (blocco automatico, velocità, quando il motore ne
  prende il controllo, unità della velocità) e touch.
- **Game** — traduzione (dai `.tra` nella cartella del gioco), cartelle
  personalizzate per salvataggi e dati condivisi, compressione dei salvataggi,
  caricamento dell'ultimo salvataggio.
- **Accessibility** — salto di parlato e testo, velocità di lettura,
  modalità del parlato, attesa del testo.
- **Advanced** — cache di sprite e texture, esecuzione in background,
  compatibilità per giochi vecchi (niente plugin, sistema operativo
  dichiarato agli script, upscale, gestione tasti nuova), argomenti extra
  per il motore, variabili d'ambiente extra (utili per provare opzioni del
  driver grafico, es. `MESA_GLTHREAD=false`) e chiusura della GUI all'avvio.

Sotto le schede:

- **Reset to game defaults** ricarica le opzioni dall'`acsetup.cfg` del gioco.
- **Profile** — esporta le opzioni correnti (tranne quali gioco sono) su un
  file per backup o per portarle su un'altra macchina, le importa da un file,
  oppure le salva come profilo con nome (es. "TV", "monitor con scanline") da
  riapplicare a qualsiasi gioco in un clic, senza rifare tutti i campi a mano.
- **Diagnostics** mostra il riepilogo dell'ultimo avvio (modo video realmente
  impostato, renderer, shader: il posto dove controllare se il motore ha
  davvero preso "Start windowed"), l'ultimo log completo o, con le opzioni
  correnti anche non salvate, `--tell-config`, `--tell-data` e
  `--tell-gameproperties` — con un **Cancel** se il motore non risponde,
  invece di un'attesa fissa.

Le chiavi che la GUI non gestisce restano esattamente come erano.

Una cosa che il motore fa e che sorprende: una dimensione come `x2` per la
finestra (o `x3` per lo schermo intero) viene usata solo con lo scaling
"Round". Con "Proportional" o "Stretch" il motore la ignora e apre la finestra
più grande possibile (`Engine/main/graphics_mode.cpp`, `get_game_frame_from_screen_size`).
La GUI lo segnala sotto il campo. `native` e `LxA` funzionano con qualsiasi scaling.

Il gioco parte sempre con l'`ags` che sta accanto ad `agssetup`. Le
impostazioni sono per-gioco, in `~/.config/agssetup/games/`; la prima volta
partono dall'`acsetup.cfg` del gioco (letto, mai modificato) e da lì in poi il
motore legge solo il file per-gioco (`--conf`). Nella stessa cartella,
`<nome>-<hash>.log` contiene l'output dell'ultimo avvio del motore.

Per compilare solo la GUI: `make` (qmake6, in `build/`) oppure `make cmake`.

## Prossimi passi ragionevoli

1. Compilare davvero (CMake completo) e verificare che non ci siano
   errori che il syntax-check non poteva vedere (link, macro non
   testate).
2. Provare un preset semplice prima (es. `crt-easymode.slangp`) prima di
   uno pesante come crt-royale (multi-pass con feedback).
3. Se qualcosa non torna a runtime, il primo sospetto è quasi sempre la
   versione del contesto GL negoziata da SDL — un log utile è
   `glGetString(GL_VERSION)` subito dopo `InitLibrashaderIfConfigured()`.
