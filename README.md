# librabridge — librashader support for AGS (native Linux, no Wine)

Bridge minimo per far girare gli shader RetroArch (via librashader) sul
motore di Adventure Game Studio, senza Wine. Vedi `THIRD-PARTY-NOTICES.md`
per le licenze di librashader e AGS.

## Struttura del repo

- `librashader-integration.patch` — applicalo a un clone di
  `adventuregamestudio/ags`
- `ags-patch-files/` — le stesse modifiche come file standalone (comodo se
  il patch non applica pulito sulla tua revisione di AGS)
- `agssetup.py` — GUI di setup (sostituisce winsetup.exe)
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
- `agssetup.py` — GUI PySide6 che sostituisce winsetup.exe: modifica
  `acsetup.cfg` e aggiunge un selettore di preset shader.

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
   (`_librashaderTarget`), della dimensione dello schermo, con la STESSA
   funzione (`CreateRenderTargetDDB`) che AGS già usa per `_nativeSurface`.
   `libra_gl_filter_chain_frame()` scrive lì dentro.
4. **Presentazione.** Il blit finale che AGS già faceva
   (`RenderTexture(_nativeSurface, ...)`) viene semplicemente puntato
   sulla nuova texture quando lo shader è attivo — una sola riga di
   differenza, zero codice nuovo per il blit stesso.

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
- `agssetup.py` è stato testato funzionalmente (headless, `QT_QPA_PLATFORM=
  offscreen`): carica un `acsetup.cfg` vero, modifica campi, salva,
  rilegge, lancia un eseguibile fittizio e verifica che
  `AGS_LIBRASHADER_PRESET` arrivi correttamente nell'ambiente del
  processo figlio. Ho trovato e corretto un bug reale (`QComboBox.
  findText` con firma sbagliata) proprio grazie a questo test.

NON verificato (serve una macchina vera, con GPU):
- La compilazione **completa** del motore AGS con CMake (in questo
  sandbox non c'è una toolchain Rust abbastanza recente per compilare
  librashader — vedi sotto — quindi non ho potuto linkare/eseguire il
  motore end-to-end).
- Il rendering effettivo di un preset (`crt-royale.slangp` o altro) su
  un gioco reale — nessun output grafico verificabile qui.
- Il ramo GLES2 (mobile) di `ali3dogl.cpp`: la patch lo lascia
  sintatticamente intatto (la nuova chiamata è dietro `#if
  !AGS_OPENGL_ES2`) ma non l'ho compilato con quel flag.

## Un limite del mio ambiente, non del progetto

`librashader` ora richiede Rust ≥ 1.85 (edition 2024). Il container in
cui ho lavorato ha solo Ubuntu 24.04 via apt (rustc 1.75) e non ha
accesso di rete a rustup.rs per prenderne uno più recente — quindi non
sono riuscito a compilare `librashader.so` qui per testarlo a runtime.
Sulla tua macchina CachyOS questo non è un problema: `pacman -S rust`
installa una versione corrente.

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
AGS_LIBRASHADER_PRESET=/path/a/crt-royale.slangp ./nomegioco
```
oppure usa `agssetup.py <cartella_gioco>` per farlo dal menu.

## Prossimi passi ragionevoli

1. Compilare davvero (CMake completo) e verificare che non ci siano
   errori che il syntax-check non poteva vedere (link, macro non
   testate).
2. Provare un preset semplice prima (es. `crt-easymode.slangp`) prima di
   uno pesante come crt-royale (multi-pass con feedback).
3. Se qualcosa non torna a runtime, il primo sospetto è quasi sempre la
   versione del contesto GL negoziata da SDL — un log utile è
   `glGetString(GL_VERSION)` subito dopo `InitLibrashaderIfConfigured()`.
