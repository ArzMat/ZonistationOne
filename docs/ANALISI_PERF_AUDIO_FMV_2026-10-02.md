# Analisi: prestazioni, audio nel cluster, FMV e audio delle cinematiche 3D

Data: 2026-10-02. Codice analizzato: `stable_branch` a `bbc1c7e`.
Specifica: psx-spx di Martin "nocash" Korth, clone di `github.com/psx-spx/psx-spx.github.io` al
commit `00d5dcb` (2026-10-01).

Quattro richieste:

1. analisi delle prestazioni;
2. analisi del bitrate audio nel cluster (il deploy Kubernetes in `deploy/`);
3. verifica incrociata con psx-spx dei difetti grafici durante gli FMV;
4. verifica incrociata con psx-spx dell'audio mancante nelle cinematiche 3D in-engine.

Ogni voce ha una pseudo-soluzione, il rischio per l'accuratezza e il modo di verificarla. I dettagli
completi, con tutte le righe di confronto doc/codice, sono negli allegati in `docs/analisi_2026-10-02/`.

---

## Come leggere questo documento

**Metodo.** Analisi statica. In questo ambiente non ci sono BIOS, dischi, GPU né SDL3/GLEW, quindi
nulla è stato eseguito contro un gioco. L'unica eccezione è il decoder MDEC: il vero `src/core/mdec.c`
è stato compilato in un banco di prova fuori dal repository e interrogato direttamente (sezione 3).
Ogni numero di tempo che non viene da un commento o da un documento del repository è una stima, ed è
marcato come tale.

**Etichette.**

| Etichetta | Significato |
|---|---|
| CERTO | letto nel codice, con `file:riga` |
| STIMA | ragionamento sul costo, con il ragionamento esplicito; non è una misura |
| DA MISURARE | serve un profilo o una sonda sulla macchina reale |
| CONFORME / DIFFORME | confrontati una riga di doc e una riga di codice: coincidono / no |
| NON VERIFICATO | la doc non decide, oppure manca una misura |

Vale la regola che il repository già usa negli audit: nessuna voce è CONFORME senza una riga di doc e
una di codice.

**Citazioni della doc.** Il clone attuale di psx-spx non ha più il layout piatto a cui puntano i
commenti del codice (`DOCS/graphicsprocessingunitgpu.md:687`). I file sono sotto `docs/ps1/` e i
numeri di riga sono cambiati. Qui le citazioni sono `ps1/<percorso>:<riga>` su quel clone:

| Vecchio `DOCS/...` | Nuovo `ps1/...` |
|---|---|
| `macroblockdecodermdec.md` | `cpu/mdec/macroblockdecodermdec.md` |
| `soundprocessingunitspu.md` | `spu/soundprocessingunitspu.md` |
| `cdromdrive.md`, `cdromformat.md` | `cdr/cdromdrive.md`, `cdr/cdromformat.md` |
| `dmachannels.md` | `system/dmachannels.md` |
| `graphicsprocessingunitgpu.md` | spezzato in `gpu/*.md` (`display-control-commands-gp1.md`, `memory-transfer-commands.md`, `video-memory-vram.md`, ...) |

Per riprodurre: `git clone https://github.com/psx-spx/psx-spx.github.io && git -C psx-spx.github.io checkout 00d5dcb`.

**Allegati** (scritti da quattro analisi parallele, ciascuna verificata a campione riga per riga prima
di entrare qui; le voci riportate in questo documento sono quelle verificate):

- `docs/analisi_2026-10-02/A_prestazioni.md`
- `docs/analisi_2026-10-02/B_audio_cluster.md`
- `docs/analisi_2026-10-02/C_fmv.md`
- `docs/analisi_2026-10-02/D_audio_cinematiche_3d.md`

---

## 0. Sintesi

| Area | Cosa non va, in una riga | Primo intervento | Rischio |
|---|---|---|---|
| Prestazioni | Il thread di emulazione aspetta lo swap del frame precedente *prima* di emulare (`src/main.c:875`); su GL si caricano 2 MB di VRAM inutili a ogni campo; su Vulkan ogni primitiva diventa un batch con una barriera | spostare l'attesa dopo `system_run_frame`; early return in `glr_upload_vram`; confronto prima del flush nei setter Vulkan | nullo |
| Audio nel cluster | Il bitrate non è il problema (Opus 96 kbit/s, 136 sul filo, circa il 5% della sessione WAN). Lo sono Chrome che decodifica in mono, una coda che scarta in silenzio, frame da 10 ms con il 42% di overhead e circa 111 ms di buffer nell'emulatore che mettono l'audio dietro al video | munging SDP `stereo=1`; parametri audio da variabili d'ambiente con 20 ms sulla WAN; A/B con `ZS1_SPU_NO_STRETCH=1` nel pod | basso |
| FMV | Il decoder è fedele; i difetti sono intorno: il reset MDEC azzera le tabelle (frame grigi), MDEC(0)/(4..7) inghiottono parametri (frame persi), il fill GP0(02) arriva allo schermo tagliato e mascherato, il clip a 9 bit è nel punto sbagliato | R1 e R2 della sezione 3, due funzioni in `mdec.c` | basso |
| Audio cinematiche 3D | I settori XA di altri canali arrivano alla CPU come dati con INT1; l'IRQ di voce SPU confronta l'indirizzo sbagliato; un Loop End senza Loop Start spegne la voce; lo sweep del volume principale non esiste | sonda di classificazione (4.6), poi A1 e A4 | medio |

Tre osservazioni trasversali:

- **Il modello di costo dei DMA è incoerente** (sezione 5.1). La riduzione di `downcount` che dovrebbe
  rappresentare lo stallo della CPU viene sovrascritta al dispatch successivo, e intanto ogni parola
  letta dalla RAM da un DMA addebita alla CPU 3 cicli di stallo di load. Tocca sia le prestazioni sia
  il ritmo degli FMV, e va deciso prima di ottimizzare i loop DMA.
- **La consegna dei settori XA filtrati** (A1 in sezione 4, R8 in sezione 3) è lo stesso difetto visto
  da due lati: rompe l'audio delle scene con banchi XA multicanale e aggiunge INT1 spurii agli STR.
- **`CLAUDE.md` è in parte superato** (sezione 5.3): la larghezza del display da GP1(06) è già
  corretta nel codice, e diverse voci di "Next up" sono chiuse.

---

## 1. Prestazioni

### 1.1 Contesto numerico (solo dal repository)

| Fonte | Dato |
|---|---|
| `Makefile:40-45` | thread emu 3,710 ms/campo (gcc-14, senza LTO), 3,225 ms (gcc-13, LTO); CPI 1,618 invariato |
| `Makefile:31-38` | in un profilo perf i controlli del debugger valevano il 2,92% dei campioni, `cpu_reg` + `mask_region` l'1,90% |
| `src/core/bus.c:512-515` | `interconnect_load32` da sola il 4,3% dei campioni |
| `src/gpu/renderer_gl.c:2031-2034` | un readback dell'intera VRAM costa circa 2,3 ms |

STIMA derivata: un campo PAL sono 680823 cicli (`src/main.c:47-49`), con CPI 1,618 circa 420 mila
istruzioni guest; 3,225 ms / 420 mila = circa 7,7 ns per istruzione, cioè 35-45 cicli host. Il thread
di emulazione sta quindi al 16-18% del budget di 20 ms sulla macchina del proprietario: **gli stalli
tra thread pesano più dei cicli per istruzione**. Questi ultimi contano su macchine più lente e per un
eventuale fast-forward.

### 1.2 Classifica

| # | Finding | Natura | Impatto atteso | Rischio | Sforzo |
|---|---|---|---|---|---|
| P1 | L'emulazione aspetta lo swap del campo precedente | CERTO il meccanismo, entità DA MISURARE | fino a un periodo di refresh per campo quando scatta | nullo | basso |
| P2 | GL: upload completi della VRAM che nessuno legge | CERTO | circa 4 MB/campo di copie in meno su due thread | nullo | basso |
| P3 | Vulkan: ogni primitiva è un batch con una barriera | CERTO | batch giù di 1-2 ordini di grandezza | nullo | basso |
| P4 | Readback sincroni per GP0(80) e GP0(C0) | CERTO il meccanismo, frequenza DA MISURARE | elimina picchi da ms | medio-basso | medio |
| P5 | Interprete: lavoro fisso per istruzione | CERTO + STIMA | 15-35% del tempo emu (STIMA) | da nullo a medio-basso | medio |
| P6 | LTO spento quando gcc e g++ hanno major diverse; nessun PGO | CERTO | -7,7% misurato nel repo per LTO | nullo | basso |
| P7 | Bus: percorso RAM con watchpoint, tabella e chiamate per ogni accesso | CERTO + STIMA | 5-10% del tempo emu (STIMA) | basso, vedi 5.1 | medio |
| P8 | Logging: ogni macro è una chiamata variadica anche se filtrata | CERTO + STIMA | 1-3% a INFO; molto di più in DEBUG | nullo | basso |
| P9 | GL: costo fisso per batch, `glBufferSubData` sempre all'offset 0, barriera incondizionata | CERTO | 30-70% del tempo CPU del thread GPU in scene 3D (STIMA) | nullo | medio |
| P10 | Probe Lua: `emu.on_event` riceve ogni notifica (per op GTE, per poligono) | CERTO | invalida le misure prese con una sonda attiva | nullo | basso |

Le voci minori (GP0(A0) per pixel, timer, lettore CD, IDCT a 64 bit, scheduler, SPU, viewer VRAM, GTE,
punti ciechi di `ZS1_FRAME_PROFILE`) sono nell'allegato A, F10-F19.

### P1. L'emulazione aspetta lo swap del campo precedente

**Dove (CERTO).**
- `src/main.c:875`: `renderer_wait_frame_done()` è chiamata *prima* di `system_run_frame()` (`:892`).
- GL: l'attesa è su `frames_pending`, azzerato dal thread GPU solo dopo `SDL_GL_SwapWindow`
  (`src/gpu/renderer_gl.c:2236`, `:2248-2251`).
- Vulkan: `VK_PRESENT_MODE_FIFO_KHR` (`src/gpu/vk/vk_device.c:348`).
- Nel tree non c'è nessuna chiamata a `SDL_GL_SetSwapInterval`: l'intervallo è il default del driver.
- `ZS1_FRAME_PROFILE` prende `t0` dopo l'attesa (`src/main.c:889`), quindi l'attesa non compare nel
  profilo.

**Cosa succede.** Il periodo di un campo diventa `max(pacing, tempo del thread GPU incluso lo swap) +
emulazione + UI + upload`. Se lo swap blocca sul vblank (60 Hz di pannello contro 50 Hz di contenuto),
l'eccesso si somma al campo, il ring audio scende e interviene lo stretcher
(`src/spu/spu_mixing.c:650-698`). È un candidato, **non dimostrato**, per il "-30% drift with
underruns" che `CLAUDE.md` non è mai riuscito ad attribuire.

L'attesa ha una ragione reale: ImGui riusa i propri draw data a ogni `NewFrame`, e il thread GPU li sta
disegnando. Quella protezione serve prima di `debug_ui_render()`, non prima dell'emulazione:
l'emulazione scrive solo nello slot di scrittura del renderer.

**Pseudo-soluzione.**
```c
/* main.c, nuovo ordine nel while */
poll_events();                                  /* invariato */
if (s_prof) t0 = now();
system_run_frame(&inter, &cpu);                 /* emula mentre la GPU chiude il campo prima */
if (s_prof) t1 = now();
renderer_wait_frame_done(&inter.gpu.renderer);  /* ora protegge solo ImGui */
if (s_prof) t_wait = now();                     /* bucket nuovo nel profilo */
debug_ui_render(&cpu, &inter);
renderer_upload_vram(...);                      /* vedi P2 */
renderer_submit_frame(...);
```
Inoltre: `SDL_GL_SetSwapInterval(0)` esplicito, perché il clock è già il ring audio
(`src/main.c:970-984`); `VK_PRESENT_MODE_MAILBOX_KHR` quando disponibile; un interruttore
`ZS1_VSYNC=0|1` per rendere l'A/B ripetibile.

**Rischio.** Nessuno sul tempo emulato. Lato host, tearing se si spegne il vsync.

**Verifica.** A/B senza toccare il codice, con le variabili dei driver:
```sh
__GL_SYNC_TO_VBLANK=1 ZS1_GPU=nvidia ZS1_FRAME_PROFILE=1 ./ZoniStation_One ...
__GL_SYNC_TO_VBLANK=0 ZS1_GPU=nvidia ZS1_FRAME_PROFILE=1 ./ZoniStation_One ...
vblank_mode=0 ZS1_GPU=intel ZS1_FRAME_PROFILE=1 ./ZoniStation_One ...
# dove dorme il main thread
perf record -e sched:sched_switch --call-graph fp -t "$(pidof ZoniStation_One)" -o offcpu.data -- sleep 20
perf report -i offcpu.data --stdio --no-children --sort symbol | head -60
```
Conferma: con il sync acceso `glr_wait_frame_done` / `vkr_wait_frame_done` compare negli stack del
main thread e gli underrun (vista Audio) crescono; con il sync spento spariscono.

### P2. GL: upload completi della VRAM che nessuno legge

**Dove (CERTO).**
- `src/main.c:924`: `renderer_upload_vram()` dell'intera VRAM a ogni campo.
- `src/gpu/renderer_gl.c:1262-1266`: `glr_upload_vram` registra un update da 1024x512, cioè una copia
  da 2 MB nel pool del thread di emulazione e un `glTexSubImage2D` da 2 MB sul thread GPU
  (`:1960-1967`).
- `src/gpu/gpu_commands.c:145-150`: `upload_vram_if_dirty()` ne fa un altro alla prima primitiva
  texturizzata dopo ogni fill.
- Quell'upload scrive solo il mirror R16UI `vram_texture`. Il mirror viene campionato solo quando
  manca `ARB_texture_barrier`: con la barriera il batch lega `vram_tex` (`:1360-1365`).
- Vulkan ha già risolto la stessa cosa rendendo l'upload un no-op voluto, e il commento lo spiega
  (`src/gpu/vk/renderer_vk.c:927-949`).

**Effetto collaterale (CERTO il meccanismo).** Il pool è 16 MB per slot (`renderer_gl.c:34`) e ogni
upload completo ne consuma 2. In una scena con molti fill alternati a primitive texturizzate si può
esaurire, e allora i rettangoli *veri* vengono scartati con "VRAM pool full" (`:1219-1223`): una
texture mancante che sembrerebbe un difetto grafico.

**Pseudo-soluzione.**
```c
void glr_upload_vram(GlRenderer* r, const uint16_t* d) {
    if (!r->initialized) return;
    lua_debug_notify("vram_full_upload");   /* gli script che lo ascoltano restano validi */
    if (s_texture_barrier) return;          /* nessuno campiona il mirror */
    glr_record_vram_update(r, d, 0, 0, 1024, 512, false);
}
/* glr_execute_one_vram_update: il TexSubImage R16UI solo se !s_texture_barrier */
```

**Rischio.** Nessuno dove c'è la barriera: comportamento allineato a Vulkan. Senza barriera il percorso
resta identico.

**Verifica.** Bucket `vram_upload` di `ZS1_FRAME_PROFILE` prima e dopo (contiene anche
`debug_ui_render`, vedi allegato A, F19); `emu.gpu_pool()` (`src/core/lua_debug.c:367-375`) per gli
skip del pool.

### P3. Vulkan: ogni primitiva diventa un batch con una barriera

**Dove (CERTO).**
- `src/gpu/vk/renderer_vk.c:796-806`: ogni setter fa `VKR_FLUSH` senza confrontare il valore nuovo con
  quello attuale. Ogni primitiva chiama 3-4 setter prima del push (`src/gpu/gpu_commands.c:534-538`,
  `:623-626`), quindi il primo setter della primitiva N chiude il batch della N-1.
- `src/gpu/vk/renderer_vk.c:1365-1371`: tra due batch qualsiasi `vkCmdEndRendering`, barriera di
  feedback e `vkCmdBeginRendering`.
- Tetto a 8192 batch per campo, oltre il quale i vertici vengono scartati (`renderer_vk.c:701-704`).
- Il backend GL confronta prima del flush (`src/gpu/renderer_gl.c:1136-1149`).

**Pseudo-soluzione.**
```c
void vkr_set_dither_mode(VkRenderer* r, bool e) {
    if (r->dither_enabled == e) return;      /* come il backend GL */
    VKR_FLUSH(r); r->dither_enabled = e;
}
/* idem per tutti i setter */

/* replay: barriera solo se il batch successivo legge la VRAM */
bool reads_vram = b->textured || b->mask_test;
if (rendering && reads_vram && written_since_barrier) {
    vkr_end_vram_rendering(cmd, &rendering);
    vkr_barrier_vram_feedback(cmd, &r->vram);
    written_since_barrier = false;
}
```
Aggiungere anche il conteggio dei batch per campo nella vista Pipeline, che oggi è alimentata solo da
GL (`src/debug_ui.cpp:2109`, `src/gpu/renderer_gl.c:1447`).

**Rischio.** Nessuno se i confronti coprono tutti i campi copiati nello snapshot del batch
(`renderer_vk.c:715-732`). Verifica pixel per pixel con `ZS1_DUMP_FRAME`, che produce dump confrontabili
tra i due backend.

### P4. Readback sincroni per GP0(80) e GP0(C0)

**Dove (CERTO).** `src/gpu/gpu_commands.c:1254` (GP0(80)) e `:1399` (GP0(C0)) leggono il rettangolo
sorgente dalla GPU. Su GL si attende la fine del campo precedente, swap compreso
(`src/gpu/renderer_gl.c:2421-2443`); su Vulkan c'è un `vkQueueWaitIdle` per chiamata
(`src/gpu/vk/renderer_vk.c:1217`). Succede anche quando il rasterizzatore non ha mai toccato quel
rettangolo, cioè quando `gpu.vram.data` è già autorevole.

**Pseudo-soluzione.** Mappa a tile (64x32 tile da 16x16, 2048 bit) dei pixel scritti dal rasterizzatore:
```c
/* draw: marcare la bounding box clippata alla drawing area (conservativo)
 * upload/fill CPU: azzerare i tile interamente coperti */
if (vram_rect_touches_gpu_tiles(gpu, src_x, src_y, w, h))   /* gestire il wrap */
    renderer_read_vram_rect(&gpu->renderer, vram, src_x, src_y, w, h);
vram_tiles_clear(gpu, src_x, src_y, w, h);                  /* ora la CPU è autorevole */
```
Su Vulkan, un command buffer e una fence riutilizzati invece di allocazione e `vkQueueWaitIdle`.

**Rischio.** Medio-basso: una scrittura GPU non marcata produce un dato vecchio. Mitigazioni:
marcatura conservativa, `ZS1_FORCE_READBACK=1` per l'A/B, e il confronto già disponibile con
`emu.vram_compare` (`src/core/lua_debug.c:779`).

**Nota di accuratezza (CERTO).** Il readback Vulkan non riesegue le op pendenti dello slot di scrittura
(`renderer_vk.c:1578-1597`), mentre GL sì (`renderer_gl.c:2018-2028`): su Vulkan GP0(80) e GP0(C0)
leggono la VRAM senza le primitive del campo corrente.

### P5. Interprete: lavoro fisso per istruzione

Tutte CERTO nel codice; il guadagno è STIMA.

| Sotto-voce | Dove | Pseudo-soluzione | Rischio |
|---|---|---|---|
| Ring di trace da 64 KB scritto a ogni istruzione, più grande della L1D | `include/cpu.h:197-199`, `src/cpu/cpu_execution.c:128-134` | `EXEC_TRACE_SIZE` a 1024, oppure registrare solo i target dei salti, oppure attivarlo solo con la workspace di debug | nessuno sul guest |
| Debugger interrogato a ogni istruzione con una chiamata cross-TU | `src/cpu/cpu_execution.c:155-160` | `if (__builtin_expect(dbg->breakpoint_count \| dbg->step_skip_bp, 0))` prima della chiamata | nullo |
| Read-modify-write di Cause a ogni istruzione | `src/cpu/cpu_execution.c:22-49` | scrivere `cause` solo se il bit 10 cambia | quasi nullo |
| Doppio dispatch indiretto per SPECIAL | `src/cpu/cpu_decode.c:92-102` | `switch` sull'opcode con handler `static inline` nello stesso TU | nullo |
| `cpu_reg`/`cpu_set_reg` fuori linea con controllo `index >= 32` morto | `src/cpu/cpu_registers.c:15-46` | `static inline` negli header, senza controllo | nullo |
| Una chiamata cross-TU per istruzione dal driver del campo | `src/core/system.c:64-71` | `cpu_run_frame()` in `cpu_execution.c` con loop interno su `downcount > 0` | nullo |

STIMA complessiva: 15-35% del tempo emu, 0,5-1,2 ms/campo sulla macchina del proprietario.

**Verifica.** Il `CPI=` della riga `[PROF]` deve restare identico (`Makefile:44-45`). Per il ring di
trace: `perf stat -e cpu_core/L1-dcache-load-misses/ -t <tid main>` prima e dopo.

### P6. Build: LTO condizionale, nessun PGO

`Makefile:57-65` attiva LTO solo se `gcc` e `g++` hanno la stessa major; il commento a `:47-50` dice
che la macchina descritta ha gcc 14.2 e g++ 13.3, quindi un `make` semplice costruisce senza LTO. LTO
vale il 7,7% misurato (`Makefile:40-45`).

**Pseudo-soluzione.**
1. LTO solo sul C: togliere `$(LTO)` da `CXXFLAGS` e linkare con `$(CC) ... $(LTO) -lstdc++`.
   L'interprete, il bus e la GPU sono tutti C; ImGui e `debug_ui.cpp` restano oggetti normali.
2. Spostare gli helper minuscoli negli header come `static inline` (P5), così le prestazioni non
   dipendono più da LTO.
3. Target `make pgo-gen` / `make pgo-use` con `-fprofile-generate` e `-fprofile-use
   -fprofile-partial-training`, su 60 s di una scena fissa. Guadagno DA MISURARE.

### P7. Bus: percorso RAM

`src/core/bus.c:571-609` (`interconnect_load32`): allineamento, `debugger_check_read_watchpoint` con
due hash anche senza watchpoint, `mask_region`, `bus_charge_cpu_load`, poi `ram_load32` in un altro TU
con controllo di limiti (`src/core/ram.c:21-44`).

**Pseudo-soluzione.**
```c
static inline bool bus_try_ram_load32(Interconnect* in, uint32_t a, uint32_t* out) {
    uint32_t phys = a & REGION_MASK[a >> 29];           /* identico a mask_region */
    if (__builtin_expect((a & 3) | g_dbg_watch_active | (phys >= 0x00800000u), 0))
        return false;                                   /* percorso lento invariato */
    in->cpu_mem_stall_cycles += g_ram_load_stall;       /* stesso costo di oggi */
    memcpy(out, &in->ram->data[phys & (RAM_SIZE - 1)], 4);
    return true;
}
```
**Attenzione:** gli stessi accessori servono anche i DMA parola per parola. Sostituirli nei loop DMA
cambia il tempo emulato (sezione 5.1): il fast path va usato solo nelle istruzioni della CPU.

### P8. Logging

`include/log.h:99-205`: ogni `LOG_*` è una chiamata incondizionata a `log_print`, e il filtro di livello
sta dentro la funzione variadica (`src/utils/log.c:233-245`). Ci sono DEBUG con 16 argomenti per ogni
quad texturizzato (`src/gpu/gpu_commands.c:853-859`) e TRACE per primitiva e per operazione GTE.
`CLAUDE.md` dice "TRACE never in default builds", ma il codice non lo compila via.

**Pseudo-soluzione.**
```c
extern LogLevel current_log_level;
#ifndef ZS1_LOG_MAX_LEVEL
#define ZS1_LOG_MAX_LEVEL LOG_LEVEL_DEBUG          /* TRACE compilato via di default */
#endif
#define LOG_AT(cat, lvl, ...) do {                                         \
    if ((lvl) <= ZS1_LOG_MAX_LEVEL &&                                      \
        __builtin_expect((int)(lvl) <= (int)current_log_level, 0))         \
        log_print((cat), (lvl), __VA_ARGS__);                              \
} while (0)
```
Le variabili usate solo nei log vanno spostate dentro gli argomenti, altrimenti la regola "zero
warning" si rompe quando la macro sparisce.

### P9 e P10

- **P9, GL per batch** (`src/gpu/renderer_gl.c:1318-1420`): quattro `glBufferSubData` all'offset 0 per
  batch (`:1376-1384`), `glTextureBarrier` sempre (`:1360`), circa 25 chiamate GL. Pseudo-soluzione:
  un upload incrementale per campo e `glDrawArrays(prim, b->vertex_start, b->vertex_count)`; barriera
  solo se il batch campiona la VRAM o testa la maschera; cache delle uniform.
- **P10, sonde Lua** (`src/core/lua_debug.c:252-258`, `:960-966`): un solo callback senza filtro,
  invocato per ogni operazione GTE (`src/gte/gte.c:165`) e due volte per poligono. Anche
  `scripts/host_speed.lua` registra `on_event`, quindi perturba ciò che misura. Pseudo-soluzione:
  `emu.on_event(fn, "vblank", "mdec_macroblock")` con una bitmask di sottoscrizione controllata in C
  prima di entrare in Lua.

### 1.3 Piano di misura

Regole di `CLAUDE.md`, valide per ogni passo: nessuna misura con `ZS1_LOG_STDERR`, con sonde Lua o con
breakpoint; shell di gioco; tre run e mediana; controllare la data del binario prima di misurare.

```sh
# 1. build: verifica se LTO è acceso
make clean && make 2>&1 | grep '\[build\]'
# 2. baseline su un savestate fisso in una scena 3D (F8 per caricarlo), 30 s
for i in 1 2 3; do
  ZS1_FRAME_PROFILE=1 timeout 90 ./ZoniStation_One roms/bios-pal.bin --game="games/game.bin"
  grep '\[PROF\]' logs/System.log > prof_base_$i.txt
done
# 3. CPU per thread mentre gira
pidstat -t -p "$(pidof ZoniStation_One)" 1 30
# 4. hotspot del thread di emulazione
perf record -e cpu_core/cycles/ -F 1999 --call-graph fp -t "$(pidof ZoniStation_One)" -o emu.data -- sleep 30
perf report -i emu.data --stdio --no-children --sort symbol --percent-limit 0.3
```
Su una CPU ibrida, fissare il processo sui core P con `taskset` e usare gli eventi `cpu_core/...`.
I passi per P1 sono nella sua voce; quelli per P2-P4, P9 (uprobe e conteggi) nell'allegato A, sezione 4.

---

## 2. Audio nel cluster

Il perimetro è `deploy/session/`: un pod per sessione, PulseAudio con un null sink dentro il container
e due consumatori del suo monitor, la pipeline WebRTC (`webrtc_server.py`) e lo stream WebM/Opus su
HTTP (`entrypoint.sh`). L'immagine è `ubuntu:24.04` (`deploy/session/Dockerfile:13`, `:40`), quindi
GStreamer 1.24.2, PulseAudio 16.1, ffmpeg 6.1.1, libopus 1.4; SDL è compilato da `release-3.2.24`
(`Dockerfile:26`).

**Fonti esterne.** I siti della documentazione ufficiale (gstreamer.freedesktop.org, ffmpeg.org,
rfc-editor.org, w3.org) non erano raggiungibili da questo ambiente. Le proprietà sono state verificate
sui sorgenti da cui quella documentazione è generata, alle versioni installate nell'immagine, letti da
GitHub. Ho ricontrollato di persona i quattro punti su cui poggiano le raccomandazioni: il bug di SDL
3.2.24 (C6), l'unità di `fragment_size` in ffmpeg 6.1.1 (C5), l'enum `restricted-lowdelay` in
`gstopusenc.c` 1.24.2 (C3) e il default mono di libwebrtc (C1). L'allegato B ha gli URL esatti.

### 2.1 Bitrate e formato a ogni stadio

```
disco XA / CD-DA ──> decoder XA, zigzag 37,8 -> 44,1 kHz ──> FIFO CD ──┐
                                                                        ├─> SPU 44,1 kHz s16 stereo
24 voci SPU + riverbero ────────────────────────────────────────────────┘
   ──> ring SPU + stretcher WSOLA ──> SDL3 (S16, 2 ch, 44100) ──> PulseAudio, null sink "zs1"
   ──> zs1.monitor (48 kHz) ──┬─> pulsesrc ─> queue leaky ─> opusenc 96k 10 ms ─> RTP/SRTP ─> browser
                              └─> ffmpeg libopus 96k 10 ms ─> WebM ─> HTTP :6081 ─> <audio>
```

| Stadio | Formato | Bitrate | Latenza | Stato | Fonte |
|---|---|---|---|---|---|
| XA su disco, 37,8 kHz 4 bit stereo | 18,75 settori/s (1/8 a 2x) | 345,6 kbit/s di payload | - | CERTO (calcolo) | `ps1/cdr/cdromformat.md:683`, `:750-773` |
| XA, 18,9 kHz 4 bit mono (parlato) | 4,69 settori/s (1/32) | 86,4 kbit/s | - | CERTO (calcolo) | `cdromformat.md:686` |
| CD-DA | 75 settori/s | 1411,2 kbit/s | - | CERTO | |
| FIFO CD | 44,1 kHz s16 stereo | - | 7-48 ms misurati, massimo 213 ms | CERTO | `include/cdrom_audio.h:21`, `CHANGELOG.md:918` |
| Uscita SPU | 44,1 kHz s16 stereo, 768 cicli per campione | 1411,2 kbit/s | - | CERTO | `include/spu.h:25-26` |
| Ring SPU + stretcher WSOLA | idem | idem | obiettivo 4893 frame = **111 ms** (banda 89-133 ms) | CERTO (codice) | `src/spu/spu_mixing.c:634-638` |
| Dispositivo SDL3 | S16, 2 ch, 44100; hint 512 frame | idem | 11,6 ms chiesti, probabilmente 23,2 ms (C6) | CERTO il codice, INFERITO l'effetto | `src/main.c:449-453` |
| PulseAudio | conversione 44,1 -> 48 kHz (speex-float-1), una sola | 1536 kbit/s PCM | qualche ms | INFERITO, DA MISURARE | `entrypoint.sh:47-48` |
| pulsesrc + coda leaky | 48 kHz, buffer da 10 ms | - | al massimo 20 ms in coda | CERTO | `webrtc_server.py:107-109` |
| opusenc | CELT, constrained-VBR | **96 kbit/s** di payload | 10 ms di frame + 2,5 ms di lookahead | CERTO | `webrtc_server.py:111` |
| Sul filo, IPv4 + UDP + RTP + SRTP-80 | 100 pacchetti/s, 120 B di payload | **136 kbit/s** (42% di overhead) | rete + jitter buffer del browser | CERTO (calcolo) | allegato B, 4.6 |
| Video, per confronto | H.264 NVENC CBR | 2500 kbit/s sulla WAN | - | CERTO | `deploy/session/sessions.yaml:81-82` |

Il rapporto di compressione è circa 14,7:1 (1411,2 contro 96 kbit/s) e l'audio è circa il 5% del
traffico media della sessione su WAN. **Il bitrate audio non è il problema di banda**; i problemi
sono la qualità percepita (C1), la robustezza (C2, C3) e la latenza (C4, C7).

Overhead per configurazione, IPv4 senza TURN:

| Opus | Pacchetti/s | Sul filo | Overhead |
|---|---|---|---|
| 96k, 10 ms (attuale) | 100 | 136,0 kbit/s | 42% |
| 96k, 20 ms | 50 | 116,0 kbit/s | 21% |
| 64k, 20 ms | 50 | 84,0 kbit/s | 31% |
| 128k, 20 ms | 50 | 148,0 kbit/s | 16% |

Le dimensioni degli header (RTP 12 B, tag SRTP 10 B, UDP 8 B, IPv4 20 B) sono valori di protocollo
standard; il tag SRTP da 10 byte segue dall'unico profilo offerto da GStreamer,
`SRTP_AES128_CM_SHA1_80`. Con TURN e su IPv6 i numeri salgono (allegato B, 4.6).

### 2.2 Finding

#### C1. Chrome decodifica quasi certamente in mono

- **CERTO (codice).** libwebrtc usa 2 canali solo se il `a=fmtp` di Opus contiene `stereo=1`;
  altrimenti `GetDefaultNumChannels()` vale 1, salvo un field trial
  (`api/audio_codecs/opus/audio_decoder_opus.cc:31-56`). `deploy/session/webrtc.html:147-151` passa la
  risposta SDP senza toccarla. Firefox invece mette `stereo=1` di default.
- **INFERITO:** che Chrome stable non abiliti quel field trial.
- **Effetto:** la musica SPU stereo arriva mono a chi usa Chrome. È l'unico difetto di qualità udibile
  di tutto il percorso.
- **Pseudo-soluzione**, in `webrtc.html` tra `createAnswer()` e `setLocalDescription()`:
  ```js
  function preferStereo(sdp) {
    const m = sdp.match(/a=rtpmap:(\d+) opus\/48000\/2/i);
    if (!m) return sdp;
    const pt = m[1];
    const re = new RegExp(`a=fmtp:${pt} ([^\\r\\n]*)`);
    return re.test(sdp)
      ? sdp.replace(re, (l, p) => /stereo=/.test(p) ? l : `a=fmtp:${pt} ${p};stereo=1;sprop-stereo=1`)
      : sdp.replace(m[0], `${m[0]}\r\na=fmtp:${pt} stereo=1;sprop-stereo=1`);
  }
  const answer = await pc.createAnswer();
  answer.sdp = preferStereo(answer.sdp);
  await pc.setLocalDescription(answer);
  ```
- **Rischio:** basso; in Firefox non cambia nulla.
- **Verifica:** in `chrome://webrtc-internals`, lo `sdpFmtpLine` del codec audio deve contenere
  `stereo=1`; test L/R su una scena con panning evidente.

#### C2. Parametri audio fissi e frame da 10 ms

- **CERTO.** `ZS1_WEBRTC_BITRATE_KBPS` agisce solo sul video (`webrtc_server.py:31`, `:79-82`);
  l'audio è fisso a `bitrate=96000 frame-size=10` (`:111`).
- **Effetto:** a 96 kbit/s i frame da 10 ms raddoppiano i pacchetti (overhead 42% contro 21%) per
  risparmiare 10 ms su una catena che, lato sorgente, ne vale circa 165 (C7). Sulla WAN e su un relay
  DERP conta anche il numero di pacchetti.
- **Pseudo-soluzione:**
  ```python
  # webrtc_server.py, accanto a BITRATE
  A_KBPS  = int(os.environ.get("ZS1_WEBRTC_AUDIO_KBPS", "96"))
  A_FRAME = os.environ.get("ZS1_WEBRTC_AUDIO_FRAME_MS", "20")     # 2.5|5|10|20|40|60
  A_TYPE  = os.environ.get("ZS1_WEBRTC_AUDIO_TYPE", "restricted-lowdelay")
  A_FEC   = os.environ.get("ZS1_WEBRTC_AUDIO_FEC", "0") == "1"
  if A_FEC and A_TYPE == "restricted-lowdelay":
      log("FEC requires audio-type=generic; switching")          # vedi C3
      A_TYPE = "generic"
  OPUS = (f"opusenc bitrate={A_KBPS*1000} frame-size={A_FRAME} audio-type={A_TYPE}"
          + (" inband-fec=true packet-loss-percentage=5" if A_FEC else ""))
  ```
  I nomi delle proprietà di `opusenc` sono quelli del sorgente 1.24.2; i nomi delle variabili
  d'ambiente sono una proposta.
- **Valori proposti:**

  | Rete | Bitrate | Frame | Sul filo |
  |---|---|---|---|
  | LAN | 96-128 kbit/s | 10 ms | 136-168 kbit/s, irrilevante contro 12 Mbit/s di video |
  | WAN, default dei manifest | 96 kbit/s | 20 ms | 116 kbit/s |
  | DERP o link stretti | 64 kbit/s | 20 ms | 84 kbit/s |

  Per musica stereo fullband il testo di RFC 7587 (sezione 3.1.1) indica 64-128 kbit/s con frame da
  20 ms. La scelta finale va fatta con un ascolto ABX su tre scene: musica SPU stereo, FMV con XA,
  parlato XA a 18,9 kHz.
- **Rischio:** 10 ms di latenza in più con i frame da 20 ms; 64 kbit/s può impoverire lo stereo.

#### C3. La coda audio scarta in silenzio, e la FEC non può funzionare

- **CERTO (meccanismo).** `queue max-size-buffers=2 leaky=downstream` (`webrtc_server.py:109`): con
  buffer da 10 ms ogni stallo a valle oltre circa 20 ms (CPU contesa, DTLS, garbage collector di
  Python) fa scartare il buffer più vecchio. Nessun contatore lo registra.
- **CERTO.** `audio-type=restricted-lowdelay` esiste in GStreamer 1.24.2 e forza Opus in modalità solo
  CELT. FEC in banda e DTX valgono solo per il livello LPC/SILK (`include/opus_defines.h` di libopus
  1.4), quindi con questa impostazione non possono funzionare. Oggi sono entrambi spenti (default).
- **Pseudo-soluzione:**
  ```
  pulsesrc device={SINK_MON} provide-clock=false latency-time=10000 buffer-time=100000
    ! audio/x-raw,channels=2,rate=48000
    ! queue name=aq leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=60000000
    ! audioconvert ! audioresample ! {OPUS} ! rtpopuspay pt=97
    ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=60000000 ! sendrecv.
  ```
  più un contatore sul segnale `overrun` della coda `aq`, stampato nel log della sessione. La FEC
  solo con `audio-type=generic` (C2).
- **Rischio:** fino a 40 ms di latenza in più, ma solo durante uno stallo. Che `overrun` venga emesso
  anche dalle code leaky è NON VERIFICATO.

#### C4. La frequenza del null sink dipende dall'ordine di avvio

- **CERTO il meccanismo, INFERITO l'esito.** Il null sink nasce senza `rate`
  (`entrypoint.sh:48`), quindi a 44100 Hz (default del demone), e PulseAudio lo riconfigura alla
  frequenza del primo client quando è inattivo. Il loop ffmpeg si collega al monitor a 48 kHz *prima*
  che l'emulatore apra SDL (`entrypoint.sh:79-98` contro `:114-118`). Quindi probabilmente il sink va a
  48 kHz e PulseAudio ricampiona lo stream SDL con `speex-float-1`; l'`audioresample` di GStreamer è
  un passthrough. Con `ZS1_AUDIO_STREAM=0` la conversione si sposta sul `pulsesrc`.
- C'è comunque **una sola** conversione 44,1 -> 48 kHz lato server. È inevitabile, perché Opus non
  accetta 44,1 kHz; si può scegliere dove farla e con che qualità.
- **Pseudo-soluzione:**
  ```sh
  pulseaudio --daemonize=yes --exit-idle-time=-1 --resample-method=speex-float-5
  pactl load-module module-null-sink sink_name=zs1 rate=48000
  ```
- **Rischio:** un po' più di CPU in PulseAudio. I metodi `soxr-*` hanno qualità più alta ma secondo la
  pagina man possono aggiungere fino a circa 20 ms.
- **Verifica:** `pactl list sinks` (campo "Sample Specification") e `pactl list sink-inputs` (campo
  "Resample method").

#### C5. Lo stream ffmpeg gira sempre, con un commento sbagliato sull'unità

- **CERTO.** `-fragment_size 480` è in **byte** ("Specify the size in bytes of the minimal buffering
  fragment", `doc/indevs.texi:1293-1295` di FFmpeg 6.1.1): 480 B / 4 B per frame = 120 frame =
  **2,5 ms** a 48 kHz, non i 10 ms del commento di `entrypoint.sh:87`. Per 10 ms servono 1920 byte.
- **CERTO.** `ZS1_AUDIO_STREAM` vale 1 di default (`entrypoint.sh:78`) e nessun manifest lo imposta,
  quindi ffmpeg è collegato al monitor anche nelle sessioni servite solo via WebRTC.
- **INFERITO.** Mentre `-listen 1` aspetta un client, lo stream di registrazione Pulse è già aperto e
  non viene letto; la sua coda può accumulare fino a 4 MiB, circa 21,8 s di audio vecchio, che
  `play.html` maschera saltando al bordo live.
- **Effetti:** CPU sprecata; null sink forzato a blocchi da 2,5 ms (circa 400 risvegli al secondo);
  frequenza del sink fissata da ffmpeg (C4).
- **Pseudo-soluzione:** `ZS1_AUDIO_STREAM: "0"` in `sessions.yaml` per le sessioni solo WebRTC; dove lo
  stream HTTP serve ancora, `-fragment_size 1920` e commento corretto.

#### C6. SDL 3.2.24 raddoppia il buffer di PulseAudio

- **CERTO (codice upstream).** In `src/audio/pulseaudio/SDL_pulseaudio.c:735` di `release-3.2.24`:
  `device->buffer_size = (int) recording ? actual_bufattr->tlength : actual_bufattr->fragsize;`. La
  condizione è invertita: per la riproduzione adotta `fragsize`, che SDL chiede pari al doppio del
  buffer. In `release-3.4.0` la riga è `recording ? actual_bufattr->fragsize : actual_bufattr->tlength`
  (`:806`).
- **INFERITO:** buffer del dispositivo di 1024 frame (23,2 ms) invece dei 512 su cui è tarato
  `SPU_RING_TARGET_SAMPLES` (commento a `src/main.c:445-448`). Riguarda anche il desktop, non solo il
  cluster.
- **Pseudo-soluzione:** `ARG SDL_TAG=release-3.4.0` nel Dockerfile, e lo stesso tag nella procedura di
  `CLAUDE.md`. Non ho verificato il resto del changelog tra 3.2.24 e 3.4.0: va provato prima su
  desktop.
- **Verifica:** la riga di log `[SYSTEM] Audio: ... buf=` (`src/main.c:466-467`); `buf=1024` conferma
  il caso.

#### C7. La latenza audio è dominata dall'emulatore, e l'audio arriva dopo il video

- **CERTO (codice).** Ring SPU più stretcher WSOLA mirano a 111 ms (`src/spu/spu_mixing.c:634-635`).
  A tempo 1,0 lo stretcher è bit-esatto ma trattiene comunque il proprio blocco di lavoro, circa
  50-65 ms (`include/spu_stretch.h:39-53`).
- **INFERITO.** Lato sorgente l'audio accumula circa 127-215 ms dalla generazione al pacchetto Opus;
  il video circa 10-45 ms. Audio e video condividono CNAME e clock di pipeline, quindi il browser li
  allinea per istante di cattura: lo scarto nasce prima, nei buffer dell'emulatore. Stima: audio in
  ritardo di 80-200 ms, tipico circa 140 ms. La soglia di rilevabilità di ITU-R BT.1359 per l'audio in
  ritardo è indicata a -125 ms; il valore viene da risultati di ricerca, il testo della
  raccomandazione non è stato letto.
- **Nel pod lo stretcher non serve.** Il dispositivo è il null sink, che consuma su `CLOCK_MONOTONIC`,
  e la pipeline GStreamer usa lo stesso clock: la deriva che il WSOLA corregge è circa nulla.
- **Pseudo-soluzione**, in ordine:
  1. A/B con `ZS1_SPU_NO_STRETCH=1` nel Deployment (variabile già esistente,
     `src/spu/spu_mixing.c:646`): circa -50/-65 ms attesi.
  2. SDL 3.4.0 (C6): circa -12 ms.
  3. `SPU_RING_TARGET_SAMPLES` configurabile da variabile d'ambiente (oggi costante a
     `include/spu.h:35`), per esempio 1024 nel pod.
- **Rischio:** più underrun sotto contesa CPU (vedi C8).
- **Nota:** `docs/VULKAN_KUBERNETES_ARCHITECTURE.md:129`, `:151` promette "RTP Stream < 20ms" e un
  audio "synchronized" da uno "SPU worker thread" che non esiste più (`CHANGELOG.md:920`).

#### C8. Altri punti

| # | Voce | Stato | Pseudo-soluzione |
|---|---|---|---|
| C8a | I pod non hanno `requests`/`limits` di CPU, solo `nvidia.com/gpu` (`sessions.yaml:93-95`): QoS BestEffort, jitter di scheduling per emulatore, PulseAudio e GStreamer | CERTO | `resources.requests.cpu` uguale a `limits.cpu` (QoS Guaranteed) |
| C8b | I manifest impostano `ZS1_LOG_STDERR=1` (`sessions.yaml:66`, `:168`, `:279`), che `CLAUDE.md` vieta per qualsiasi misura di velocità | CERTO | toglierlo prima di misurare, per esempio `kubectl -n zs1 set env deploy/zs1-dino ZS1_LOG_STDERR-` |
| C8c | `webrtc.html` raccoglie statistiche solo del video (`:182`): nessuna metrica audio visibile | CERTO | leggere `inbound-rtp` con `kind == 'audio'`: `jitterBufferDelay`, `concealedSamples`, `packetsLost` |
| C8d | Se PulseAudio non parte, `SDL_AUDIODRIVER=dummy` (`entrypoint.sh:52`): il driver dummy dorme 11 ms interi invece di 11,61, quindi consuma circa il 5% più in fretta, e la macchina emulata, che segue il ring audio, gira circa il 5% più veloce | INFERITO | DA MISURARE con `scripts/clock_compare.lua` |
| C8e | Matrice ATV e Mute del CD non applicati: entrano nello stream (Dino Crisis è una delle tre sessioni) | CERTO | A10 della sezione 4 |
| C8f | Commenti fuorvianti: `entrypoint.sh:87` (unità), `:88` (il lookahead resta 2,5 ms), `webrtc_server.py:90-93` (code "one buffer deep", quella audio ne ha 2) | CERTO | correggerli insieme a C3 e C5 |

### 2.3 Piano di misura nel cluster

Senza `ZS1_LOG_STDERR` e senza sonde Lua per vblank (C8b).

```sh
P=$(kubectl -n zs1 get pod -l game=acecombat -o name)
kubectl -n zs1 exec $P -- pactl list sinks          # "Sample Specification": 44100 o 48000?
kubectl -n zs1 exec $P -- pactl list sink-inputs    # stream SDL: "Buffer Latency", "Resample method"
kubectl -n zs1 exec $P -- pactl list source-outputs # pulsesrc e ffmpeg
kubectl -n zs1 logs $P | grep 'Audio:'              # buf=512 o buf=1024 (C6)
```

Ripetere con e senza viewer WebRTC collegato, e con `ZS1_AUDIO_STREAM=0` (C4, C5). Le tre sessioni
hanno le etichette `game: acecombat`, `crash` e `dino` (`sessions.yaml:13`, `:113`, `:215`).

Nel pod, per GStreamer: `GST_DEBUG=opusenc:5,audiobasesrc:4,pulse:4,queue_dataflow:5` e cercare
"queue is full, leaking item" (C3). `queue_dataflow:5` è molto verboso: dare un nome alla coda audio
per filtrarlo.

Nel browser (`chrome://webrtc-internals` o `pc.getStats()`), sull'`inbound-rtp` audio:
`jitterBufferDelay` / `jitterBufferEmittedCount`, `concealedSamples`, `packetsLost`,
`bytesReceived` + `headerBytesReceived` (banda reale, da confrontare con la tabella 2.1); sul `codec`:
`sdpFmtpLine` e `channels` (C1).

Scarto A/V end to end: registrare lo schermo del client con audio durante un evento impulsivo (logo
di avvio del BIOS, esplosione in un FMV), misurare l'offset tra immagine e attacco del suono su dieci
eventi e prendere la mediana; ripetere con `ZS1_SPU_NO_STRETCH=1` (C7).

---

## 3. Difetti grafici durante gli FMV

### 3.1 Il decoder è fedele; i difetti sono intorno

Confrontati con `ps1/cpu/mdec/macroblockdecodermdec.md` e risultati CONFORMI: ordine dei blocchi Cr,
Cb, Y1..Y4, RLE, zigzag, q_scale e DC, saturazione dei coefficienti, struttura dell'IDCT, coefficienti
YUV, impacchettamento 15 e 24 bpp, bit 15 (STP). Anche lo scanout 24 bpp, lasciato UNVERIFIED dallo
studio del 2026-08-18, risulta CONFORME (`src/gpu/renderer_gl.c:930-937`,
`src/gpu/shaders/scanout.frag:35-42` contro `ps1/gpu/video-memory-vram.md:25-31`).

**Banco di prova.** Il vero `src/core/mdec.c` è stato incluso tale e quale in un programma C, con stub
per il log e per Lua, e interrogato direttamente. I risultati citati come "banco [n]":

| # | Prova | Risultato |
|---|---|---|
| [1] | zigzag contro la regola della doc | 0 discrepanze |
| [2] | tabelle caricate, poi reset (`80000000h` a 1F801824h) | `scale[0]` da `5a82` a `0000`, `iq_y[0]` da 2 a 0 |
| [3a] | MDEC(0) con conteggio 5 | resta in attesa di 10 halfword e mangia la parola MDEC(3) successiva |
| [3b] | MDEC(1) esatto, due parole `FE00FE00h` di padding, poi un altro MDEC(1) | `remaining_hw=130046`; il secondo frame produce **0 parole** invece di 128 |
| [4] | grigio Y=-60 in uscita signed 15 bpp | R corretto (24), G e B sbagliati (31) |
| [6] | IDCT del codice contro IDCT in virgola mobile, 20000 blocchi | errore medio 0,29, massimo 1 livello |
| [8] | percorso del codice contro `yuv_to_rgb` della doc, con rumore da ringing | sigma 4: 0,24% dei pixel diversi, max 12; sigma 8: 1,4%, max 23 |

Ho rieseguito tutte le prove del banco e i risultati si riproducono. Il banco non è incluso nel
repository; trasformarlo in un test unitario è il primo livello di `docs/TESTING_PLAN_2026-08-20.md`.

### 3.2 Classifica dei DIFFORMI per impatto visivo

#### V1. Il reset MDEC azzera le tabelle quant e scale

- **Doc:** `ps1/cpu/mdec/macroblockdecodermdec.md:116-117` "the Reset bit does NOT clear the scale
  matrix nor the quant tables, so they only need uploading once, not after every reset". Questo
  paragrafo non c'era nel layout piatto: cambia il verdetto dell'audit del 2026-08-17.
- **Codice:** `src/core/mdec.c:530` chiama `mdec_init()`, che fa `memset(m, 0, sizeof(Mdec))`
  (`:509`). Banco [2].
- **Sintomo:** frame grigio medio uniforme dopo un reset non seguito da una ricarica delle tabelle.
  Scenario tipico: un filmato interrotto o saltato con Start, poi quello successivo.
- **Pseudo-soluzione:**
  ```c
  static void mdec_soft_reset(Mdec* m) {
      uint8_t  iq_y[64], iq_uv[64];  int16_t scale[64];
      memcpy(iq_y, m->iq_y, 64); memcpy(iq_uv, m->iq_uv, 64);
      memcpy(scale, m->scale_table, sizeof scale);
      mdec_init(m);                         /* FIFO, comando, blocchi, stato 80040000h */
      memcpy(m->iq_y, iq_y, 64); memcpy(m->iq_uv, iq_uv, 64);
      memcpy(m->scale_table, scale, sizeof scale);
  }
  /* mdec_write(1F801824h): if (value & (1u << 31)) { mdec_soft_reset(m); return; } */
  /* mdec_init resta per l'accensione: tabelle a zero (doc :114-116) */
  ```
  I tre campi sono quelli di `include/mdec.h:81-85`; la struttura non cambia, i savestate restano
  compatibili.
- **Rischio:** nullo per chi ricarica comunque le tabelle.
- **Verifica:** sonda che segnala un macroblocco decodificato con scale table nulla:
  ```lua
  emu.on_event(function(n)
    if n == "mdec_macroblock" and emu.mdec_scale(0) == 0 then
      emu.log("MDEC con scale table nulla")
    end
  end)
  ```
  Scene: saltare con Start l'intro di Ace Combat 2 o di Crash Bandicoot 3 e lasciar partire il filmato
  successivo.

#### V2. MDEC(0) e MDEC(4..7) consumano parametri, anche dal padding FE00h

- **Doc:** `macroblockdecodermdec.md:119-126`: MDEC(0) riflette i bit 0-15 nello stato "without
  actually expecting any parameters"; MDEC(4..7) agiscono come MDEC(0). Il padding a FE00h per
  allineare ai blocchi DMA da 20h parole è documentato a `:77-79`.
- **Codice:** `src/core/mdec.c:442-446` (comando invalido: `num_words = cw & 0xFFFF`, stato
  `MDEC_ST_NOCOMMAND`) e `:490-498` (consuma quelle parole). Banco [3a] e [3b].
- **Sintomo:** due halfword `FE00h` arrivate dopo la fine di un MDEC(1) vengono lette come la parola
  `FE00FE00h`, cioè un MDEC(7) che chiede 65024 parole: **inghiotte il frame successivo**. FMV fermo,
  nero, o a scatti. La riga `[MDEC] Invalid command` (`mdec.c:443`) è un WARN, quindi compare già al
  livello di log predefinito: cercarla nei log di un FMV che si blocca.
- **Pseudo-soluzione:**
  ```
  case 0, 4, 5, 6, 7:
      status_low16 = cw AND FFFFh        // senza il "-1" (doc :121-123)
      decode_state = IDLE                // nessun parametro atteso
  bit 15-0 dello stato:
      MDEC(1/2/3) in corso -> parole rimanenti - 1
      altrimenti           -> status_low16 (FFFFh a fine MDEC(1/2/3), doc :43)
  ```
  `status_low16` può vivere in una variabile file-static come `g_mdec_macroblocks_out` (`mdec.c:94`)
  per non cambiare la dimensione della sezione di savestate.
- **Rischio:** basso. Corregge anche i bit 15-0 dello stato, che oggi valgono 0 invece di FFFFh a fine
  comando (`mdec.c:74-75`).

#### V3. Il fill GP0(02) arriva allo schermo come quad rasterizzato

- **Doc:** `ps1/gpu/i-o-ports-dma-channels-commands-vram.md:86-91` "Rectangle filling is not affected
  by the GP0(E6h) mask setting"; `ps1/gpu/memory-transfer-commands.md:38-47`, `:95-98` (mascheratura e
  wrap). Il fill non è soggetto alla drawing area.
- **Codice:** `src/gpu/gpu_commands.c:347` esegue `draw_rectangle()`, quindi sulla texture VRAM il fill
  passa per lo scissor della drawing area e per la maschera del batch. Il lato CPU (`:349-366`) è
  corretto: wrap, nessuna maschera, bit 15 a 0.
- **Sintomo:** in 24 bpp un buffer largo 480 halfword, pulito mentre la drawing area è larga 320,
  resta sporco nel terzo destro: bande vecchie nel letterbox. Con E6.0=1 il nero diventa `8000h`, verde
  in 24 bpp. `scripts/fmv_fill_watch.lua` sospettava già il taglio.
- **Pseudo-soluzione:**
  ```
  gp0_fill_rectangle:
      mascheratura come memory-transfer-commands.md:40-43
      scrittura lato CPU con wrap (già presente)
      per ogni pezzo ai bordi 1024/512: renderer_upload_vram_rect(pezzo)
          // fa già il flush delle primitive pendenti (renderer_gl.c:1280, renderer_vk.c:960)
      niente draw_rectangle
  ```
- **Rischio:** un fill a schermo intero diventa un upload; l'ordine resta garantito dal flush.
  Interagisce con P2: con P2 applicato, l'upload del fill va solo in `vram_tex`.
- **Verifica:** `ZS1_LUA_SCRIPT=scripts/fmv_fill_watch.lua` sull'FMV di Ace Combat 2 (letterbox) e sul
  clear più upload di Dino Crisis; poi `ZS1_DUMP_FRAME` e confronto con `emu.vram16(x,y)` sulle righe
  del letterbox.

#### V4. Il clip a 9 bit è nel punto sbagliato di yuv_to_rgb

- **Doc:** `macroblockdecodermdec.md:235-238`: `R=(Y+R) AND 1FFh` (clip a 9 bit con segno) *dopo* la
  somma, poi saturazione a -128..127.
- **Codice:** `src/core/mdec.c:198-200` applica il clip a 9 bit e la saturazione a ogni blocco
  all'uscita dell'IDCT, crominanza compresa; `:222-224` satura `Y+R` senza il clip a 9 bit.
- **Sintomo (banco [8]):** con ringing moderato cambia l'1-1,5% dei pixel, fino a 23 livelli, ai bordi
  delle zone chiare e sature: luci "piatte". È il sospetto che `CLAUDE.md` (voce G.20) annotava per i
  colori spenti negli FMV.
- **Pseudo-soluzione:**
  ```
  idct: nessun clip finale sui blocchi Cr/Cb
  y_to_mono: Y = sext9(Y AND 1FFh); clamp(-128,127); +128 se unsigned     (doc :250-251)
  yuv_to_rgb: per ogni canale C: C = sext9((Y + C) AND 1FFh); clamp(-128,127); +128 se unsigned
  ```
- **Rischio:** medio-basso, e da trattare con cautela. Preso alla lettera, il testo fa avvolgere a
  nero una somma oltre 255 (esempio del banco: Y=120, Cr=100 dà R=0 secondo la doc, 255 nel codice).
  Non ho una cattura da hardware che lo confermi. La parte sicura è togliere il pre-clip sui blocchi
  di crominanza e lasciare la saturazione finale; il clip dopo la somma va confermato con una cattura
  reale prima di adottarlo.
- **Verifica:** il banco come test unitario, poi A/B con `ZS1_DUMP_FRAME` su una scena satura.

#### V5. Il costo DMA addebitato alla CPU durante gli FMV

Vedi sezione 5.1: ogni parola letta dalla RAM da un DMA addebita 3 cicli di stallo alla CPU. Per un
frame 320x240 a 24 bpp (57600 parole verso la GPU) sono circa 172800 cicli, 5 ms di CPU emulata, contro
circa 61200 cicli di durata documentata del trasferimento (1 clk/word più un row-load ogni 16,
`ps1/system/dmachannels.md:205-207`, `:225-229`). Non corrompe l'immagine, ma toglie budget al decoder
VLC software del gioco: scatti e frame saltati. DA MISURARE con `ZS1_FRAME_PROFILE=1` sull'FMV di
Monsters & Co. prima e dopo.

#### Altri DIFFORMI (impatto minore o raro)

| Voce | Doc | Codice | Sintomo |
|---|---|---|---|
| MADR/BCR mai aggiornati durante e dopo il trasferimento | `ps1/system/dmachannels.md:27-29`, `:56-58` | `src/core/dma.c:241-247` | colonne duplicate se un player concatena i kick senza riscrivere MADR |
| Test di maschera degli upload sullo specchio CPU | `ps1/gpu/rendering-attributes.md:165-169` | `src/gpu/gpu_commands.c:134-137`, `:1635-1637` | overlay cancellati da un FMV con E6.1 |
| Settori audio+realtime filtrati consegnati come dati | `ps1/cdr/cdromdrive.md:604` | `src/cdrom/cdrom_commands.c:934-999` | INT1 in più negli STR multicanale; è A1 della sezione 4 |
| Read dopo Pause non riconsegna l'ultimo settore | `ps1/cdr/cdromdrive.md:811-814` | `src/cdrom/cdrom_commands.c:69-76` | un settore STR perso alla ripresa: frame saltato |
| Uscita MDEC signed: G e B saturano | `macroblockdecodermdec.md:39`, `:239` | `src/core/mdec.c:225-228` | colori sbagliati; raro (banco [4]) |
| GP0(A1h..BFh), (C1h..DFh) non decodificati | `ps1/gpu/memory-transfer-commands.md:3-5` | `src/gpu/gpu_commands.c:1567-1568` | frame interpretato come comandi; raro |
| Nessun wrap negli upload; lettura oltre la fine di `vram.data` se y+h>512 | `memory-transfer-commands.md:95-98` | `src/gpu/renderer_gl.c:1226-1230`, `src/gpu/vk/renderer_vk.c:897-899` | rettangoli a cavallo dei bordi persi |
| Lettura a 16 bit di 1F801822h/1F801826h torna 0 | `ps1/system/unpredictablethings.md:58` | `src/core/bus.c:383-387` | un `lhu` sulla metà alta dello stato legge busy=0 |

### 3.3 Aperti e NON VERIFICATI, con sintomo noto

| Voce | Perché non decidibile | Misura che decide |
|---|---|---|
| Fotogramma stirato a fine FMV | la doc non dice quando si applicano GP1(05)/(08); lo stato del display è campionato al submit, a fine campo (`src/gpu/renderer_gl.c:2398-2405`); il latch a inizio campo è stato provato e spento (`ZS1_DISPLAY_LATCH=1`) | contare le scritture GP1(05)/(08) per campo; `scripts/display_map_probe.lua` |
| Striscia di 8 righe negli FMV PAL di Monsters & Co. | il gioco carica a y=8 e il crop overscan oggi vale solo per NTSC (`src/gpu/gpu.c:74-81`) | `scripts/vram_display_survey.lua`, `scripts/fmv_upload_coverage.lua`; se quelle righe dovrebbero essere nere, cercare il fill mancante (V3) |

---

## 4. Audio mancante nelle cinematiche 3D

### 4.1 Perché un FMV suona e una cinematica in-engine no

Un FMV STR mette l'audio in un solo canale XA, alternato a settori video
(`ps1/cdr/cdromfileformats/streaming.md:776-781`). Il filtro trova sempre il suo canale, e i settori
dati sono quelli che il player si aspetta. Una cinematica in-engine usa invece uno di questi percorsi,
ed è esattamente lì che il codice diverge dalla doc:

| Meccanismo | Percorso hardware | Dove diverge il codice |
|---|---|---|
| (a) XA mentre il motore legge dati | Setmode bit 6 e 3, Setfilter file/canale, banchi XA con 8-32 canali (`ps1/cdr/cdromformat.md:698`), cambio canale per battuta | A1, A5, A6, A10 |
| (b) CD-DA | Play, report, AutoPause, SPUCNT.0, volume CD | A8, A10 |
| (c) streaming SPU-ADPCM | DMA4 in SPU RAM, ricarica su IRQ all'indirizzo IRQA, flag di loop, LSAX (`ps1/spu/soundprocessingunitspu.md:855-866` cita Metal Gear Solid e Tron Bonne) | A2, A3, A9 |
| (d) sequenze con voci | KON/KOFF, ADSR, sweep di volume, fade del volume principale | A4, A7, A11 |

**Il primo passo non è correggere ma classificare** (4.6): sapere quale dei quattro usa la scena
riduce la lista a due o tre voci.

### 4.2 Classifica dei DIFFORMI

#### A1. Settori XA scartati dal filtro consegnati alla CPU come dati

- **Doc:** `ps1/cdr/cdromdrive.md:594-608`, algoritmo di consegna:
  ```
  try_deliver_as_data_sector:
    reject if filter_enabled(setmode.3) AND submode is audio+realtime (bit2+bit6)
  ```
  e `:1132-1133` "If XA-ADPCM (and/or XA-Filter) is enabled via Setmode, then INT1 is generated only
  for non-ADPCM sectors".
- **Codice (CERTO):** `src/cdrom/cdrom_commands.c:874` manda al decoder solo i settori audio+RT del
  canale filtrato. Tutti gli altri, compresi quelli audio+RT di altri canali, finiscono nel ramo
  `else` (`:934-999`): latch di GetlocL (`:949-950`), buffer di lettura, **INT1** (`:986-992`). Manca
  anche il controllo MODE2 (`:597` della doc).
- **Perché separa FMV e cinematica.** Un FMV ha un canale audio e un demuxer che scarta i settori
  senza header video. Un banco di dialoghi a N canali produce N-1 INT1 spurii su N, con payload ADPCM,
  e GetlocL risponde con file/canale di un altro canale. È la stessa forma già vista su Monsters & Co.:
  `CLAUDE.md` registra che far entrare settori ADPCM nel latch di GetlocL "takes the game's demuxer off
  the video stream and its speech never plays". Un player che usa INT1 o GetlocL per capire dove si
  trova ferma la battuta (silenzio) o la riposiziona (ripetizione).
- **Pseudo-soluzione** (dalla doc, `:595-608`), in `cdrom_execute_drive`:
  ```c
  bool mode2    = (raw[15] == 2);                          /* :597 */
  bool audio_rt = mode2 && ((raw[18] & 0x44) == 0x44);     /* :600 */
  bool fc_ok    = raw[16] == xa_filter_file && raw[17] == xa_filter_channel;

  aggiorna_subq_e_head(current_lba);   /* per OGNI settore letto, rispettando lo SBI */

  if (!traccia_cdda && mode2 && xa_adpcm_enable && audio_rt &&
      (!xa_filter_enable || fc_ok)) {
      decodifica_xa(); avanza(); ripianifica(periodo); return;
  }
  if (xa_filter_enable && audio_rt) {   /* :604: scartato in silenzio */
      avanza(); ripianifica(periodo); return;   /* niente INT1, buffer, latch */
  }
  consegna_dati();                      /* come oggi */
  ```
  Spostare l'aggiornamento della SubQ (`:975-978`) prima della biforcazione è obbligatorio: oggi
  GetlocP durante l'XA funziona solo perché i settori degli altri canali passano dal ramo dati.
- **Rischio:** medio. Cambia il numero di INT1 visti da ogni titolo con XA filtrato, FMV multilingua
  compresi. Da rifare: boot BIOS, FMV di Ace Combat 2, nuova partita di Monsters & Co., menu di Dino
  Crisis (la protezione LibCrypt legge GetlocP, e il blocco SubQ si sposta).
- **Verifica:** INT1 per campo (`emu.on_event`, evento `"cdrom_int1"`) contro settori XA per campo
  (ottavo valore di `emu.cd_audio()`). Con il difetto, in un tratto di sola XA gli INT1 sono circa
  (N-1)/N dei settori letti; dopo, solo i settori dati. Le righe INFO "XA stream now file=... channel=..."
  (`cdrom_commands.c:912`) danno il canale senza log di debug.

#### A2. Streaming SPU: IRQ di voce sull'indirizzo sbagliato e IRQ spurii

- **Doc:** `ps1/spu/soundprocessingunitspu.md:824` "Triggers an IRQ when a voice reads ADPCM data from
  the IRQ address"; `:826-829` "all voices are permanently reading data from SPU RAM ... even if the
  ADSR pattern has finished the Release period - so even inaudible voices can trigger IRQs".
- **Codice (CERTO):**
  - `src/spu/spu_voice.c:203-207` confronta IRQA con `curr_addr + 16`, cioè con il blocco *successivo*
    a quello appena decodificato; `spu_check_irq` vuole l'uguaglianza esatta (`src/spu/spu_irq.c:20`).
  - `src/spu/spu_voice.c:244-247`: una voce spenta non legge più la RAM, quindi non genera IRQ.
  - `src/spu/spu_irq.c:41-56`: scrivere IRQA fa scattare subito l'IRQ se una voce o l'indirizzo di
    trasferimento coincidono. La doc lega l'IRQ solo alle letture delle voci e ai trasferimenti
    (`:824`, `:852`); scrivere il registro IRQA non è un accesso alla SPU RAM.
- **Sintomo.** Nello schema classico di doppio buffer l'IRQ si alterna tra inizio e metà dell'anello.
  Un IRQA sul primo blocco dell'anello non scatta mai, perché il blocco che lo precede in memoria non
  fa parte del loop. Il gioco non ricarica quella metà: la voce rigira i dati vecchi (**ripetizione**)
  oppure il gioco, che aspetta l'IRQ, non avanza lo stream (**silenzio**). L'IRQ immediato alla
  scrittura di IRQA fa invece ricaricare la metà in riproduzione: salto in avanti, candidato per il
  "**corre avanti**".
- **Pseudo-soluzione** (`:824`, `:826-834`, `:852`):
  ```c
  /* voice_decode_block, blocco letto all'indirizzo A = curr_addr (prima di avanzare) */
  if ((control & SPUCNT_IRQ9) && !irq9_flag) {
      uint32_t q = irq_addr * 8;
      if (q >= A && q < A + 16) alza_irq9();          /* dentro il blocco letto */
  }
  /* voci "spente": continuano a leggere blocchi e a seguire i loop, con inviluppo a 0
   * e senza contribuire al mix (:826-829); almeno quando IRQ9 è abilitato */
  /* spu_update_irq_addr: tenere solo l'azzeramento quando IRQ9 è disabilitato;
   * togliere i controlli su curr_addr e transfer_addr */
  /* scrittura di TSA (spu.c:375): nessun controllo IRQ */
  ```
- **Accoppiato:** `src/core/bus.c:253-257` azzera `irq9_flag` e STATX.6 quando il gioco fa l'ack di
  I_STAT, mentre la doc dice che il flag si riconosce solo portando SPUCNT.6 a 0 (`:635`, `:655`).
- **Rischio:** medio. Più IRQ per chi programma IRQA dentro un loop; costo host se le voci spente
  vengono decodificate sempre.
- **Verifica:** watchpoint di scrittura su IRQA (`0x1F801DA4` e `0xBF801DA4`, sonda 4.6). Con il
  difetto le scritture di IRQA cessano dopo una o due ricariche; corretto, la cadenza resta costante.

#### A3. Semantica dei loop: la voce di streaming si ferma dopo il primo giro

- **Doc:** `ps1/spu/soundprocessingunitspu.md:131` (KON copia lo start nell'indirizzo corrente);
  `:133-138` (LSAX cambia solo con un Loop Start o con una scrittura; il Loop End salta a LSAX "after
  playing the current ADPCM block"); `:163-173`, Code 3 = "jump to Loop-address, set ENDX flag" e
  Code 1 = "jump to Loop-address, set ENDX flag, Release, Env=0000h".
- **Codice (CERTO):**
  - `src/spu/spu.c:82-83`: il KON azzera `loop_addr_set` e `ignore_loop`, quindi dimentica un LSAX
    scritto dal gioco prima del KON.
  - `src/spu/spu_voice.c:217-223`: con il bit Loop End la voce salta solo se `(flags & 3) == 3` *e*
    `loop_addr_set`; altrimenti `curr_addr = 0xFFFFFFFF` e la voce si spegne.
  - `src/spu/spu.c:309`: una scrittura di LSAX imposta `ignore_loop`, che blocca i Loop Start dei
    blocchi successivi fino al KON seguente (`spu_voice.c:210`).
- **Sintomo.** Un anello senza Loop Start, con LSAX scritto prima del KON, sull'hardware torna
  all'inizio; qui si spegne al primo Loop End: **la cinematica sente il primo giro e poi silenzio**.
  Il terzo punto produce loop nel punto sbagliato (ripetizione).
- **Pseudo-soluzione:**
  ```c
  /* KON: curr_addr = start_address * 8; NON toccare repeat_address */
  /* voice_decode_block, blocco all'indirizzo A: */
  if (flags & 4) repeat_address = A >> 3;            /* sempre, :134-135 */
  if (flags & 1) {                                   /* :136-138, :163 */
      next = repeat_address * 8;  endx_dopo_il_blocco = true;
      if (!(flags & 2)) { adsr_state = RELEASE; EnvelopeVol = 0; }   /* Code 1 */
  } else next = A + 16;
  /* la voce resta attiva per le letture; sparisce solo dal mix */
  ```
  `loop_addr_set`, `ignore_loop` e `loop_addr` possono restare nella struttura inutilizzati, così
  `sizeof(Spu)` non cambia e i savestate restano caricabili.
- **Nota di provenienza.** L'intestazione di `spu_voice.c:10-11` dice che decodifica e mix delle voci
  sono portati "1:1" da pcsx-redux (GPL-2.0+). Il comportamento "Loop End senza loop impostato =
  stop" viene da quella derivazione, non dalla doc. Le correzioni qui sono scritte dalla doc e non
  copiano altro codice.
- **Rischio:** medio. `docs/study/SPU_2026-07-29.md` (finding 11) annota che un altro emulatore protegge
  una scrittura di LSAX fatta durante il primo blocco: la doc non lo descrive, ma è il primo posto da
  guardare se un titolo perde il proprio loop.
- **Verifica:** binding nuovo `emu.spu_voice(n)` (4.7) a ogni vblank: una voce con `on` che cade a
  false pochi frame dopo il KON mentre `repeat_address` è valido è A3 in azione. Ascolto con
  `ZS1_AUDIO_DUMP`.

#### A4. Lo sweep del volume principale non esiste

- **Doc:** `ps1/spu/soundprocessingunitspu.md:405-406` (MVOLL/MVOLR hanno lo stesso formato dei volumi
  voce) e `:414-432` (modo sweep).
- **Codice (CERTO):** `src/spu/spu.c:335-347`: solo il modo fisso aggiorna `main_vol_left_cur`; una
  scrittura con il bit 15 a 1 viene memorizzata e mai eseguita. Il volume principale si applica dopo
  la somma di voci, riverbero e CD (`src/spu/spu_mixing.c:404-408`).
- **Sintomo.** È l'unico difetto che **ammutolisce tutto insieme**, XA compreso. Una scena che azzera
  il volume in modo fisso e poi lo riporta su con un fade-in in sweep resta a zero per tutta la sua
  durata. Un fade-out in sweep non abbassa nulla, e la coda resta udibile oltre il cambio scena. Un
  gioco che aspetta la fine del fade leggendo MVOLX (`spu.c:228-229`) resta in attesa.
- **Pseudo-soluzione:** applicare a `main_vol_left/right`, una volta per campione, la stessa routine di
  sweep delle voci (`spu_voice_sweep_tick`, corretta per A11). I due contatori dello sweep possono
  vivere in `last_reverb_output[2]` (`include/spu.h:290`, oggi non letto da nessuno), così
  `sizeof(Spu)` non cambia. MVOLX restituisce allora il livello in sweep.
- **Rischio:** basso; il modo fisso non cambia.
- **Verifica:** watchpoint su `0x1F801D80`/`0x1F801D82`: scritture con il bit 15 a 1 all'ingresso
  della scena confermano il caso.

#### A5-A9. Altri DIFFORMI con effetto possibile sul silenzio

| # | Voce | Doc | Codice | Sintomo | Pseudo-soluzione | Rischio |
|---|---|---|---|---|---|---|
| A5 | Il drive si ferma per qualsiasi INT pendente, anche durante l'XA | `ps1/cdr/cdromdrive.md:759-762`, `:1953-1956` | `src/cdrom/cdrom.c:144-156`, riparte solo all'ack (`:412-427`) | buchi o silenzio se un ack tarda (anche l'INT3 di un GetlocP di polling) | con INT pendente, processare comunque i settori che vanno all'ADPCM o che vanno scartati (A1); trattenere solo la consegna dati | alto (struttura del drive) |
| A6 | ADPBUSY (HSTS bit 2) mai impostato | `cdromdrive.md:62`, `:1020-1021` | `src/cdrom/cdrom.c:290-301` | un gioco che legge "XA in riproduzione" vede sempre 0 e ferma o non avvia la battuta (ipotesi) | `if (audio_fifo.count > 0 && xa_adpcm_enable && drive_state == DRIVE_READING) st \|= 0x04;` | basso |
| A7 | SPUCNT.14 silenzia anche il CD | `ps1/spu/soundprocessingunitspu.md:631` "(Don't care for CD Audio)" | `src/spu/spu_mixing.c:404-405` (mute dopo la somma) | XA muto se il gioco silenzia le voci con il bit 14 durante la scena | applicare il mute a voci e invio al riverbero dalle voci, prima di sommare il CD | basso |
| A8 | AutoPause con `static prev_track` che sopravvive a Pause, Setloc e Play | `cdromdrive.md:1100`, `:1103-1104` | `src/cdrom/cdrom_commands.c:1041` | INT4 immediato e pausa: musica CD-DA della scena muta | niente `static`; inizializzare la traccia di riferimento all'avvio del Play | basso |
| A9 | FIFO di scrittura manuale scartata in modo Stop | `soundprocessingunitspu.md:683`, `:717-720` | `src/spu/spu.c:378-386`, `src/spu/spu_dma.c:89-90` | campioni caricati via I/O nella sequenza documentata persi: voci mute | buffer fino a 32 halfword riempito in modo 0, svuotato in SPU RAM al passaggio al modo 1 | basso per l'audio; nuovo campo, cambia la versione dei savestate |

#### A10 e A11. I due candidati per "ripete ai cambi scena"

`CLAUDE.md` registra su Dino Crisis (E) che l'audio delle cinematiche "repeats across some scene
changes, and runs ahead of the scene". Due difetti fanno sentire ciò che il gioco ha zittito:

- **A10. Mute, ADPMUTE, matrice ATV e volume CD a 0 non vengono mai applicati.**
  - **Doc:** `ps1/cdr/cdromdrive.md:1019-1022` "muting is just forcing the CD output volume to zero";
    `:1023` "Mute is used by Dino Crisis 1"; `:227-242` (matrice ATV, saturazione fino al doppio);
    `:251` (ADPMUTE); `ps1/spu/soundprocessingunitspu.md:442` (volume CD con segno).
  - **Codice (CERTO):** `cdrom_get_audio_frame()` (`src/cdrom/cdrom.c:471-492`), l'unica funzione che
    applica Mute e matrice, non è chiamata da nessuno: la SPU legge la FIFO direttamente
    (`src/spu/spu_mixing.c:510-511`). I commenti di `cdrom_audio.c:292` e `:330` dicono che il mute "is
    applied in cdrom_get_audio_frame()": non è vero da quando la SPU ha smesso di chiamarla. Inoltre
    un volume CD a 0 è trattato come volume pieno (`spu_mixing.c:520-521`).
  - **Sintomo:** il gioco abbassa il CD o manda Mute e riposiziona; noi lasciamo udibile la coda della
    battuta precedente e i settori della transizione.
  - **Pseudo-soluzione.** La logica esiste già in `cdrom_get_audio_frame()` (`cdrom.c:479-491`: mute,
    matrice, saturazione); va estratta in una funzione che lavora sul frame già letto, e chiamata dove
    la SPU legge la FIFO, senza perdere il "tieni l'ultimo campione" di oggi:
    ```c
    /* cdrom.c */
    void cdrom_apply_output_matrix(const Cdrom* cd, int16_t* l, int16_t* r) {
        if (cd->muted || cd->xa_mute) { *l = *r = 0; return; }       /* :479 */
        int32_t ol = ((int32_t)*l * cd->vol_ll + (int32_t)*r * cd->vol_lr) >> 7;
        int32_t or_ = ((int32_t)*l * cd->vol_rl + (int32_t)*r * cd->vol_rr) >> 7;
        *l = cdrom_sat16(ol); *r = cdrom_sat16(or_);                  /* :486-491 */
    }
    /* spu_mixing.c, spu_step, dopo il pop/hold di :507-516 */
    cdrom_apply_output_matrix(&inter->cdrom, &cl, &cr);
    int32_t cv_l = (int16_t)spu->cd_vol_left, cv_r = (int16_t)spu->cd_vol_right;
    /* togliere :520-521: un volume CD a 0 vale 0 (doc SPU :442) */
    ```
    Mute e ATV vanno applicati dopo l'hold, così un Mute zittisce anche il campione tenuto. La
    mappatura delle porte ATV è già stata corretta dopo l'audit di agosto (`cdrom.c:363`, `:371-372`,
    `:430-445`).
  - **Rischio:** medio. Il ramo "0 = pieno" esiste per un percorso che lascia il volume CD a 0: va
    trovato prima di toglierlo. Da rifare: lettore CD del BIOS e Ace Combat 2.
  - **Verifica:** watchpoint su `0x1F801DB0`/`0x1F801DB2` e sul comando Mute (0Bh); A/B con
    `ZS1_SPU_NO_CDAUDIO=1`: se la ripetizione sparisce, viene dal CD.
- **A11. Lo sweep lineare decrescente scende sotto zero.**
  - **Doc:** `ps1/spu/soundprocessingunitspu.md:431-432` "decreases to 0000h"; `:480-481`.
  - **Codice (CERTO):** `src/spu/spu_voice.c:364-371`, `:384-391` limitano a -8000h.
  - **Sintomo:** un fade-out lineare passa lo zero e risale fino a volume pieno con la fase invertita:
    l'audio della scena precedente "ritorna".
  - **Pseudo-soluzione:** nel decremento senza bit di fase, `v = max(v, 0)`; con il bit di fase,
    `clamp(v, -0x8000, 0)`.
  - **Rischio:** basso.

### 4.3 "Corre avanti alla scena"

- Il clock della SPU è conforme: 768 cicli CPU per campione (`include/spu.h:26`,
  `src/spu/spu_mixing.c:473-479` contro `ps1/spu/soundprocessingunitspu.md:1180-1181`), e la SPU genera
  campioni solo fino a `cpu_cycle_counter` (`spu_mixing.c:573-590`). **Da sola la SPU non può
  anticipare il clock emulato.**
- Restano due famiglie:
  1. dentro l'audio: ricariche anticipate di uno stream SPU per gli IRQ spurii di A2;
  2. fuori dall'audio: una scena che il gioco disegna più lentamente del reale, mentre l'audio va al
     ritmo giusto. Va misurata sull'asse dei campi emulati, come chiede `CLAUDE.md`: per la stessa
     cinematica, campi tra inizio e fine della battuta e flip del display per campo, contro una run di
     riferimento contata in `Now in v-blank`.

### 4.4 Voci aperte nei documenti precedenti

Rispetto a `docs/study/README.md`, `docs/CDROM_AUDIT_2026-08-17.md` e
`docs/DMA_IRQ_GTE_MDEC_AUDIT_2026-08-17.md`, il codice ha già chiuso: le porte ATV e ADPCTL, le letture
CDROM a 16/32 bit, i rifiuti di GetlocL e Pause in seek, il BCD di Setloc, l'Init ripetuto,
Reset/Sync/17h/18h, GetTD, le maschere del coding info, il rapporto 18900 Hz, la direzione dello sweep,
VxPitch, tutte le voci DICR/bus error/scritture parziali, il ritmo del DMA GPU.

Ancora aperte e rilevanti qui: applicazione della matrice CD (A10), IRQ SPU da voci inattive (A2),
sweep principale (A4), scrittura di LSAX (A3), ADPBUSY (A6), consegna dei settori filtrati e MODE2
(A1), modello di ritmo del drive (A5), AutoPause statico (A8), capture CD fuori dalla SPU RAM. La
tabella completa è nell'allegato D, sezione 5.

### 4.5 Strumenti che esistono davvero

- Variabili d'ambiente lette nel codice: `ZS1_AUDIO_DUMP` (`src/spu/spu_mixing.c:432`), `ZS1_XA_DUMP`
  (`src/cdrom/cdrom_audio.c:305`), `ZS1_SPU_NO_REVERB` (`spu_mixing.c:383`), `ZS1_SPU_NO_CDAUDIO`
  (`spu_mixing.c:358`), `ZS1_SPU_NO_STRETCH` (`spu_mixing.c:646`).
- Lua: `emu.cd_audio()` restituisce 8 valori, nell'ordine count, pushed, popped, dropped, starved,
  SPUCNT, settori letti, settori XA (`src/core/lua_debug.c:626-636`); `emu.spu_stats()`,
  `emu.audio_stats()`, `emu.on_event`, `emu.on_break`, `emu.add_write_watch`, `emu.add_read_watch`,
  `emu.reg`, `emu.disasm`, `emu.resume`, `emu.load_state`.
- `emu.read_u16` legge solo RAM, BIOS e scratchpad: non legge i registri SPU o CDROM.
- I watchpoint confrontano l'indirizzo virtuale: servono sia `0x1F80xxxx` sia `0xBF80xxxx`.
- **Due sonde esistenti leggono male `emu.cd_audio()`** (CERTO):
  - `scripts/audio_timeline.lua:23` salta "starved", quindi le colonne SPUCNT, settori e settori XA
    sono spostate di uno;
  - `scripts/spu_pop_capture.lua:67` prende il terzo valore come "drop", ma è `total_popped`.

  Vanno corrette prima di usarle per questa indagine.

### 4.6 Sonda di classificazione (solo API esistente)

Da eseguire su un savestate preso pochi secondi prima della cinematica (F5). Dice quale meccanismo usa
la scena e quale voce della 4.2 è in azione. Nessuna misura di velocità va presa con questa sonda
attiva.

```lua
-- scripts/cutscene_audio_classify.lua (da scrivere)
local STATE = "savestates/slot0.zst"
local loaded, f, int1 = false, 0, 0
local prev, hits = {}, {}
local WATCH = {                                    -- KUSEG e KSEG1
  [0x1F801DA4]="IRQA",   [0xBF801DA4]="IRQA",
  [0x1F801D80]="MVOLL",  [0xBF801D80]="MVOLL",
  [0x1F801DAA]="SPUCNT", [0xBF801DAA]="SPUCNT",
  [0x1F801DB0]="AVOLL",  [0xBF801DB0]="AVOLL",
  [0x1F8010C8]="DMA4",   [0xBF8010C8]="DMA4",
  [0x1F801801]="CDCMD",  [0xBF801801]="CDCMD",
}
for a in pairs(WATCH) do emu.add_write_watch(a) end

emu.on_break(function(reason)
  local wp = tonumber(reason:match("0x(%x+)"), 16)
  local name = WATCH[wp]
  if name then hits[name] = (hits[name] or 0) + 1 end
  -- il valore scritto si ricava da emu.disasm(pc) e emu.reg(rt), vedi allegato D, 6.3
  emu.resume()
end)

emu.on_event(function(ev)
  if ev == "cdrom_int1" then int1 = int1 + 1 return end
  if ev ~= "vblank" then return end
  f = f + 1
  if not loaded then loaded = true; emu.load_state(STATE); return end
  if f % 25 ~= 0 then return end
  local cnt, push, pop, drop, starve, ctrl, sect, xas = emu.cd_audio()
  local d = function(k, v) local x = v - (prev[k] or v); prev[k] = v; return x end
  emu.log(string.format(
    "[cls] f=%d sett=%d xa=%d int1=%d push=%d starve=%d SPUCNT=%04x IRQA=%d DMA4=%d MVOL=%d CDCMD=%d",
    f, d("s", sect), d("x", xas), d("i", int1), d("p", push), d("st", starve), ctrl,
    d("q", hits.IRQA or 0), d("m", hits.DMA4 or 0), d("v", hits.MVOLL or 0), d("c", hits.CDCMD or 0)))
end)
```

Lettura:

| Osservazione | Meccanismo | Voce |
|---|---|---|
| `xa` > 0 e `push` > 0 durante la battuta | (a) XA | - |
| `int1` vicino a `sett - xa` in un tratto senza dati attesi | (a) | A1 |
| `xa` = 0, DMA4 regolari, IRQA riscritto a cadenza costante | (c) streaming SPU | - |
| IRQA che smette di essere riscritto dopo una o due volte | (c) | A2 |
| scritture di MVOL all'ingresso della scena | (d) | A4 (bit 15 a 1) |
| SPUCNT con bit 14 a 0 mentre `push` cresce | (a) | A7 |
| AVOL a 0, o comando 0Bh nella transizione | (a)/(b) | A10 |
| `starve` in crescita durante la battuta con `xa` > 0 | (a) | A5 |

### 4.7 Binding Lua da aggiungere

Lettura diretta delle strutture, senza `spu_catch_up` e senza pop di FIFO (oggi non esistono):

- `emu.spu_voice(n)`: `on`, `curr_addr`, `repeat_address`, `loop_addr_set`, `ignore_loop`,
  `adsr_state`, `EnvelopeVol`, volumi, pitch, bit di ENDX;
- `emu.spu_irq()`: `irq_addr`, `irq9_flag`, `control`, `status`, `transfer_addr`, volumi principali
  correnti;
- `emu.cd_state()`: `drive_state`, `interrupt_flag`, `mode`, filtro file/canale, mute, coefficienti
  ATV, `last_header[0..7]`.

Con `emu.cd_state()` agganciato a `"cdrom_int1"`, un submode audio+RT (per esempio `64h`) con il filtro
attivo è la prova diretta di A1: un INT1 che l'hardware non avrebbe mai generato.

---

## 5. Punti in comune e correzioni ai documenti esistenti

### 5.1 Il modello di costo dei DMA è incoerente

Due fatti CERTI, emersi da due analisi diverse:

1. **La riduzione di `downcount` non ha effetto.** `src/core/bus.c:902` (GPU), `:1007` (MDEC) e `:1226`
   (SPU) sottraggono i cicli del DMA a `cpu->downcount`, i primi due dentro un handler di evento.
   Subito dopo il dispatch `downcount` viene ricalcolato da `cpu_cycle_counter`
   (`src/cpu/cpu_execution.c:221-227`), che non è avanzato: la sottrazione si perde. La CPU emulata
   non si ferma per la durata del DMA.
2. **Esiste uno stallo accidentale.** Ogni parola che un DMA legge dalla RAM passa per
   `interconnect_load32` (`bus.c:943`, `:1129`, `:815`, ...), che addebita 3 cicli di stallo di load
   alla CPU (`bus.c:550-555`, `:579`), consumati sull'istruzione successiva
   (`cpu_execution.c:214-218`).

La doc dice che la CPU continua a girare durante un DMA finché usa solo cache, scratchpad, COP0 e GTE,
e si ferma fino alla fine del DMA al primo accesso in lettura a RAM o I/O
(`ps1/system/dmachannels.md:231-238`). Oggi il costo DMA reale è quindi lo stallo accidentale, che vale
circa 3 volte la durata documentata del trasferimento (V5).

Conseguenze pratiche:
- l'ottimizzazione ovvia di P7 (letture dirette nei loop DMA) **cambia il CPI e i tempi emulati**;
- correggere V5 togliendo lo stallo accidentale rende la CPU emulata più veloce durante gli FMV.

Il modello va deciso esplicitamente, con la citazione della doc, e verificato con le pietre miliari
di boot in campi emulati prima di qualsiasi ottimizzazione su quei percorsi. Un modello coerente con la
doc: durante un DMA la CPU avanza; alla prima lettura di RAM o I/O mentre il DMA è attivo, si addebita
il tempo residuo del trasferimento.

### 5.2 La consegna dei settori XA filtrati è un difetto solo

A1 (sezione 4) e la riga "Settori audio+realtime filtrati consegnati come dati" (sezione 3.2) sono la
stessa riga di codice. Va corretta una volta, con il piano di verifica di A1, che copre anche gli FMV.

### 5.3 Correzioni a `CLAUDE.md` e ad altri documenti

- **"Display window is computed from the wrong register"** (Known Broken) è superato: la larghezza
  viene già da GP1(06) con la tabella dei divisori (`src/gpu/gpu.c:83-100`), l'altezza raddoppia solo
  con interlace a 480 righe (`gpu.c:129`), il modo 368 è decodificato (`gpu.c:42`). Resta vero che la
  posizione X1/Y1 non sposta l'immagine, quindi lo screen-shake via GP1(06)/(07) resta invisibile.
- **"Next up"**: le voci A.1, A.2, B.3, D.6, D.7, D.8, E.9, F.14 e F.15 risultano chiuse nel codice;
  C.4 è a metà (porte ATV corrette, matrice mai applicata, vedi A10). Dettaglio nell'allegato D,
  sezione 5, e nell'allegato C, sezione 2.
- **"TRACE never in default builds"**: il codice non compila via TRACE (P8).
- **Commenti in `src/cdrom/cdrom_audio.c:292` e `:330`**: dicono che il mute viene applicato in
  `cdrom_get_audio_frame()`, che nessuno chiama (A10).
- **Sonde Lua**: `scripts/audio_timeline.lua:23` e `scripts/spu_pop_capture.lua:67` leggono male
  `emu.cd_audio()` (4.5).
- **Citazioni `DOCS/...`**: i numeri di riga non corrispondono più al clone attuale di psx-spx (tabella
  in testa). Il testo MDEC attuale contiene due paragrafi nuovi che cambiano due verdetti dell'audit del
  2026-08-17 (V1 e V4).

---

## 6. Ordine di lavoro proposto

A lotti, come in `CLAUDE.md`: dentro un lotto le voci toccano lo stesso codice e si verificano con la
stessa run.

| Lotto | Voci | Perché in quest'ordine | Verifica |
|---|---|---|---|
| 1. Strumenti | correggere le due sonde (4.5); bucket `wait`, `ui`, `pace` in `ZS1_FRAME_PROFILE`; binding `emu.spu_voice`, `emu.spu_irq`, `emu.cd_state`; banco MDEC come test unitario | senza strumenti affidabili nessuna delle voci sotto si può confermare | build senza warning |
| 2. Classificazione | sonda 4.6 sulla cinematica che perde l'audio | riduce la sezione 4 a due o tre voci | un log `[cls]` |
| 3. Prestazioni a rischio nullo | P1, P2, P3, P5 (ring di trace, gate del debugger), P6, P8 | nessun effetto sul guest; il CPI deve restare identico | `ZS1_FRAME_PROFILE=1`, mediana di 3, `CPI=` invariato; `ZS1_DUMP_FRAME` identico su GL e Vulkan |
| 3b. Cluster | C1, C2, C3, C4, C5 (configurazione e pagina, nessun codice dell'emulatore); poi l'A/B di C7 e C6 | tutti fuori dal core: rischio nullo per il guest | piano 2.3, statistiche del browser prima e dopo |
| 4. MDEC | V1, V2, poi V4 nella sua parte sicura | due funzioni in `mdec.c`, verificabili dal banco | banco + FMV di Ace Combat 2 e Monsters & Co. |
| 5. Audio della cinematica | le voci indicate dal lotto 2, di solito A1 + A4, oppure A2 + A3 | dipende dal meccanismo | sonda 4.6 prima e dopo, `ZS1_AUDIO_DUMP` |
| 6. "Ripete ai cambi scena" | A10, A11 | fanno sentire ciò che il gioco ha zittito | A/B con `ZS1_SPU_NO_CDAUDIO=1` |
| 7. GPU | V3, P4, P9 | toccano l'ordine delle op del renderer | `ZS1_DUMP_FRAME`, `scripts/fmv_fill_watch.lua` |
| 8. Tempo | 5.1 (modello DMA), A5 (drive), P7 | cambiano il tempo emulato: per ultimi, con una run pulita | pietre miliari di boot in campi emulati contro la run di riferimento |

---

## 7. Cosa non è stato possibile fare qui

- Nessuna esecuzione contro BIOS o dischi: ogni voce marcata DA MISURARE resta un'ipotesi finché non
  gira sulla macchina reale.
- Il legame tra un gioco specifico (Dino Crisis, Monsters & Co.) e un meccanismo audio resta
  un'ipotesi finché la sonda 4.6 non lo stabilisce.
- `duckstation_ref/` e `pcsx-redux/` sono vuoti in questo checkout e non sono stati consultati. Tutte
  le pseudo-soluzioni sono scritte dalla doc citata.
- Il sito di nocash (`problemkaputt.de`) e `psx-spx.consoledev.net` non erano raggiungibili da questo
  ambiente; il testo usato è il repository GitHub di psx-spx al commit indicato in testa.
