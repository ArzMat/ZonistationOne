> **Allegato A** a `docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md`. Prodotto da un'analisi statica
> parallela il 2026-10-02 sul codice a `bbc1c7e` e su psx-spx al commit `00d5dcb`. Le voci riprese nel
> documento principale sono state riverificate riga per riga; le altre portano doc e codice citati per
> il controllo, ma non sono state ricontrollate una per una. Correzione: la nota N1 (sezione 6) è confermata e sviluppata nella sezione 5.1 del documento principale.

# Analisi statica delle prestazioni host di ZoniStation One

Data: 2026-10-02. Albero analizzato: commit `bbc1c7e` (branch di lavoro `claude/wizardly-planck-fb7ppi`).
Nessun file del repository è stato modificato e nulla è stato eseguito: BIOS, dischi, GPU, SDL3 e GLEW non sono disponibili qui. Tutto ciò che segue viene dalla lettura del codice. `duckstation_ref/` non è stato aperto.

---

## 0. Come leggere questo documento

Ogni affermazione porta una delle tre etichette:

- **CERTO**: comportamento letto nel codice, con `file:riga`.
- **STIMA**: ragionamento sul costo, con il ragionamento esplicito. Non è una misura.
- **DA MISURARE**: serve un profilo sulla macchina reale. Il piano è nella sezione 4.

I soli numeri presentati come misurati vengono da commenti o documenti del repository, sempre con la fonte accanto.

**Criterio di ordinamento.** Conta quanto un problema aumenta la probabilità di sforare il budget di un campo PAL (20 ms) o di provocare underrun audio. Il peso dipende dal tempo host risparmiato e da quanto spesso il percorso gira. Sulla macchina del proprietario il thread di emulazione è già a circa il 16-18% del budget (sezione 1). Per questo gli accoppiamenti tra thread e gli stalli sincroni pesano più dei cicli risparmiati per istruzione. Questi ultimi contano per le macchine più lente, per la workspace di debug e per un eventuale fast-forward.

---

## 1. Contesto quantitativo (solo dal repository)

| Fonte | Dato |
|---|---|
| `Makefile:40-45` | emu 3.710 ms/campo (gcc-14, senza LTO), 3.495 ms (gcc-13, senza LTO), 3.225 ms (gcc-13, LTO); mediana di 3, 30 s di Monsters & Co.; CPI 1.618 invariato |
| `Makefile:31-38` | in un profilo perf i tre controlli del debugger valevano il 2.92% dei campioni, `cpu_reg` + `mask_region` l'1.90% |
| `include/cpu.h:105-110` | il vecchio `memcpy` da 128 byte per istruzione valeva l'1.8% (3.5% con LTO); oggi è rimosso |
| `src/core/bus.c:512-515` | `interconnect_load32` da solo il 4.3% dei campioni; una catena di test di regione era costata circa il 10% del tempo host |
| `src/gpu/renderer_gl.c:2031-2034` | readback dell'intera VRAM circa 2.3 ms a chiamata; 50 in un secondo bloccavano il thread di emulazione per 115 ms |
| `CLAUDE.md` (State as of 2026-08-04) | circa 3.7 ms di costo host contro 20 ms di campo PAL, pannelli chiusi |
| `src/utils/log.c:15-18`, `CLAUDE.md` | una run a livello DEBUG scrive circa 1.4 milioni di righe in 100 s |

**Derivazione (STIMA).** Un campo PAL dura 680823 cicli (`src/main.c:47-49`). Con CPI 1.618 sono circa 420 mila istruzioni guest per campo. 3.225 ms / 420 mila = circa 7.7 ns per istruzione guest, cioè 35-45 cicli host a 4.5-5.5 GHz. Il numero comprende tutto ciò che gira sul thread di emulazione: comandi GPU, SPU, eventi, DMA. È il tetto entro cui stanno tutti i finding sull'interprete.

---

## 2. Riepilogo ordinato

| # | Finding | Thread | Natura | Impatto atteso | Rischio accuratezza | Sforzo |
|---|---|---|---|---|---|---|
| F1 | Il thread di emulazione aspetta il thread GPU (e lo swap/vsync) prima di emulare il campo | emu + GPU | CERTO meccanismo, entità DA MISURARE | fino a un periodo di refresh per campo quando scatta; spiegazione candidata degli underrun | nessuno | basso |
| F2 | Upload completi della VRAM inutili sul backend GL (ogni campo e dopo ogni fill) | emu + GPU | CERTO | circa 4 MB/campo di memcpy e altrettanti di upload GL risparmiati; un modo in meno di esaurire il pool | nessuno (con barriera) | basso |
| F3 | Readback sincroni per GP0(80) e GP0(C0) | emu + GPU | CERTO meccanismo, frequenza DA MISURARE | elimina picchi da ms per ogni copia VRAM->VRAM | medio-basso | medio |
| F4 | Vulkan: batching di fatto disattivato, una barriera per primitiva | GPU-VK | CERTO | numero di batch giù di 1-2 ordini di grandezza | nessuno | basso |
| F5 | GL: costo fisso per batch, `glBufferSubData` allo stesso offset, barriera incondizionata | GPU | CERTO meccanismo, costo DA MISURARE | 30-70% del tempo CPU del thread GPU in scene 3D (STIMA) | nessuno | medio |
| F6 | Interprete: lavoro fisso per istruzione (trace ring da 64 KB, IRQ, debugger, doppio dispatch, helper fuori linea) | emu | CERTO + STIMA | 15-35% del tempo emu (STIMA) | da nullo a medio-basso per sotto-voce | medio |
| F7 | Build: LTO condizionale (spento con gcc/g++ di major diverse), nessun PGO | emu | CERTO | -7.7% misurato nel repo per LTO; PGO DA MISURARE | nessuno | basso |
| F8 | Bus: percorso RAM con watchpoint, tabella di regione e chiamate per ogni accesso | emu | CERTO + STIMA | 5-10% del tempo emu (STIMA) | basso | medio |
| F9 | Logging: ogni macro è una chiamata variadica; righe emesse costose (line buffering) | emu | CERTO + STIMA | 1-3% a INFO; molto di più in DEBUG e negli storm di WARN | nessuno | basso |
| F10 | GPU lato CPU: divisioni e chiamate per pixel in GP0(A0), fill, GP0(80), GPUREAD | emu | CERTO + STIMA | 0.3-1 ms per frame con FMV o upload pesanti (STIMA) | nessuno | basso |
| F11 | Probe Lua: `emu.on_event` riceve ogni notifica (per op GTE, per poligono) | emu | CERTO + STIMA | invalida le misure fatte con probe (0.5-3 ms/campo, STIMA) | nessuno | basso |
| F12 | Timer: eventi programmati anche senza IRQ armato; divisioni double per lettura | emu | CERTO, frequenza DA MISURARE | da nullo a decine di migliaia di dispatch per campo | basso (richiede catch-up anche nella lettura MODE) | basso |
| F13 | CD: lettore asincrono a settore singolo; tempi emulati dipendenti dall'I/O host | emu + cdrom-read | CERTO | determinismo; ipotesi sullo scarto 130 contro 152 settori/s | nessuno (migliora la riproducibilità) | medio |
| F14 | MDEC: IDCT densa a 64 bit, YUV in float ricalcolato | emu | CERTO + STIMA | 3-5x sull'IDCT durante gli FMV (STIMA) | nullo se bit-exact | basso |
| F15 | Scheduler: tre scansioni per dispatch, dispatch anticipati inutili | emu | CERTO | <1% | nullo | basso |
| F16 | SPU: scansione key on/off per campione, voci spente pagano chiamate, `%` nel riverbero | emu | CERTO + STIMA | 0.05-0.15 ms/campo (STIMA) | nullo | basso |
| F17 | Passata del VRAM viewer eseguita ogni campo anche nella shell di gioco | GPU | CERTO | 0.2-0.5 ms GPU su iGPU (STIMA) | nullo | basso |
| F18 | GTE: TRACE e notifica Lua per ogni operazione | emu | CERTO | <1% | nullo | basso |
| F19 | Punti ciechi di `ZS1_FRAME_PROFILE` | strumentazione | CERTO | prerequisito per misurare F1, F3, F5 | nullo | basso |

La sezione 6 elenca problemi di accuratezza emersi durante l'analisi. Sono fuori dal perimetro, ma almeno due (N1, N5) cambiano il tempo emulato se si toccano i percorsi DMA o SWL/SWR per ragioni di prestazioni.

---

## 3. Finding

### F1. Il thread di emulazione aspetta il thread GPU, e quindi lo swap, prima di emulare il campo

**Dove.**
- `src/main.c:875`: `renderer_wait_frame_done()` viene chiamata *prima* di `system_run_frame()` (`src/main.c:892`).
- `src/gpu/renderer_gl.c:2445-2451`: l'attesa è su `frames_pending`, che il thread GPU azzera solo dopo `SDL_GL_SwapWindow` (`src/gpu/renderer_gl.c:2236`, azzeramento a `:2248-2251`).
- Vulkan: `src/gpu/vk/renderer_vk.c:1324-1328` (`vkWaitForFences` e `vkAcquireNextImageKHR` con timeout `UINT64_MAX`), `src/gpu/vk/vk_device.c:348` (`VK_PRESENT_MODE_FIFO_KHR`), azzeramento di `frames_pending` a `:1510-1513` dopo il present.
- Nel tree non esiste alcuna chiamata a `SDL_GL_SetSwapInterval` (grep vuoto su `src/`): l'intervallo di swap GL è il default del driver.
- `ZS1_FRAME_PROFILE` prende `t0` dopo l'attesa (`src/main.c:889`) e non misura il pacing (`src/main.c:981-995`): l'attesa è invisibile nel profilo attuale.

**Cosa succede (CERTO).** Il periodo di un'iterazione del main loop è `max(P, G) + E + U + V`:
- P è il pacing sul ring audio (`src/main.c:981-984`);
- G è il tempo che il thread GPU impiega per il campo precedente, incluso lo swap o l'acquire/present;
- E è l'emulazione;
- U è `debug_ui_render`;
- V è l'upload di `src/main.c:924`.

Quando G supera P, l'eccesso si somma direttamente al periodo del campo.

L'attesa esiste per una ragione reale. ImGui riutilizza i propri draw data a ogni `ImGui::NewFrame` (`src/debug_ui.cpp:4192-4193`), e il thread GPU li sta disegnando (`src/gpu/renderer_gl.c:2229-2234`). Questa protezione però serve prima di `debug_ui_render()`, non prima di `system_run_frame()`. L'emulazione scrive solo nello slot di scrittura (`src/gpu/renderer_gl.c:1429-1430`). `write_idx` cambia solo in `glr_submit_frame`, sotto mutex (`src/gpu/renderer_gl.c:2415`). I readback sincroni aspettano già per conto loro (`src/gpu/renderer_gl.c:2431-2432`).

**Costo (STIMA, DA MISURARE).** Se e quanto lo swap blocchi dipende dal driver: Mesa con `vblank_mode`, NVIDIA con `__GL_SYNC_TO_VBLANK`, e da quante immagini il driver accoda. Con contenuto a 50 Hz su un pannello a 60 Hz l'attesa del vblank sta tra 0 e 16.7 ms. Con FIFO in Vulkan l'acquire blocca quando tutte le immagini della swapchain sono in coda. Quando G > P il campo dura più di 20 ms, il ring audio scende e interviene lo stretcher (`src/spu/spu_mixing.c:650-698`).

Ipotesi, **non dimostrata**: `CLAUDE.md` riporta un "-30% drift with underruns" che non è mai riuscito ad attribuire. È compatibile con questo meccanismo, soprattutto su iGPU o con i costi GPU di F2, F4 e F5.

**Pseudo-soluzione.**
```c
/* main.c, nuovo ordine dentro il while */
poll_events(); /* ... richieste di stato, pad ... invariati */
if (s_prof) t0 = now();
system_run_frame(&inter, &cpu);                 /* emula mentre il thread GPU chiude il campo prima */
if (s_prof) t1 = now();
renderer_wait_frame_done(&inter.gpu.renderer);  /* protegge solo ImGui e lo slot da riusare */
if (s_prof) t_wait = now();
debug_ui_render(&cpu, &inter);
renderer_upload_vram(...);                      /* vedi F2 */
renderer_submit_frame(&inter.gpu.renderer, debug_ui_get_draw_data());
```
Inoltre:
- scegliere esplicitamente l'intervallo di swap: `SDL_GL_SetSwapInterval(0)`, perché il clock è già il ring audio (`src/main.c:970-984`), oppure `-1` adattivo con fallback;
- in Vulkan preferire `VK_PRESENT_MODE_MAILBOX_KHR` quando è supportato;
- un interruttore `ZS1_VSYNC=0|1` renderebbe l'A/B ripetibile.

**Impatto atteso.** Toglie fino a un periodo di refresh di serializzazione per campo quando G > P; nullo quando G < P. Nessun costo di CPU.

**Rischio per l'accuratezza.** Nessuno: non tocca alcun tempo emulato. Lato host: tearing se si spegne il vsync; l'ordine rispetto a ImGui resta corretto.

**Verifica.** Sezione 4.4 (A/B con le variabili del driver, stack off-CPU del main thread) e F19.

---

### F2. Upload completi della VRAM inutili sul backend GL

**Dove.**
- `src/main.c:921-924`: `renderer_upload_vram()` dell'intera VRAM a ogni campo.
- `src/gpu/gpu_commands.c:145-150`: `upload_vram_if_dirty()` alla prima primitiva texturizzata dopo un fill (`src/gpu/gpu_commands.c:367` imposta `vram_dirty`); chiamata da `:171`, `:621`, `:731`, `:841`.
- `src/gpu/renderer_gl.c:1262-1266` -> `:1204-1251`: copia di 2 MB nel pool (512 `memcpy` da 2 KB).
- `src/gpu/renderer_gl.c:1960-1967`: `glTexSubImage2D` 1024x512 R16UI sul thread GPU.
- Il mirror R16UI viene campionato solo se manca la texture barrier (`src/gpu/renderer_gl.c:285-300`, `:1360-1369`, `:548-549`). La barriera è presente su tutti i driver, secondo `:546-547` e `src/gpu/vk/renderer_vk.c:934-937`; con la barriera nessuno legge il mirror.

**Cosa succede (CERTO).** Ogni campo produce almeno due copie da 2 MB sul thread di emulazione, una sempre e una per ogni sequenza fill -> primitiva texturizzata. Sul thread GPU produce altrettanti upload da 2 MB. Nessuno di questi ha effetto visibile. In più `glr_upload_vram_rect` (`src/gpu/renderer_gl.c:1268-1288`) aggiorna anche il mirror (`:1964-1966`) per ogni rettangolo reale, anch'esso inutile con la barriera. Vulkan ha già risolto la stessa cosa rendendo l'upload un no-op voluto (`src/gpu/vk/renderer_vk.c:927-949`).

**Effetto collaterale (CERTO il meccanismo, frequenza DA MISURARE).** Il pool è 16 MB per slot (`src/gpu/renderer_gl.c:34`) e ogni upload completo ne consuma 2. Oltre circa 7 nello stesso campo, i rettangoli reali vengono scartati con "VRAM pool full" (`src/gpu/renderer_gl.c:1219-1223`). Lo spreco di prestazioni può quindi diventare una texture mancante, in una scena con molti fill alternati a primitive texturizzate (menu, riquadri UI).

**Costo (STIMA).** 2 MB letti e 2 MB scritti fuori cache costano 0.1-0.2 ms per copia sul thread di emulazione. Sul thread GPU il costo del driver per un `glTexSubImage2D` da 2 MB è simile o superiore. Il bucket `vram_upload` di `ZS1_FRAME_PROFILE` (`src/main.c:893-925`) contiene questa copia, ma anche `debug_ui_render` (vedi F19).

**Pseudo-soluzione.**
```c
/* renderer_gl.c */
void glr_upload_vram(GlRenderer* r, const uint16_t* d) {
    if (!r->initialized) return;
    lua_debug_notify("vram_full_upload");      /* gli script che lo ascoltano restano validi */
    if (s_texture_barrier) return;             /* il mirror non è campionato da nessuno */
    glr_record_vram_update(r, d, 0, 0, 1024, 512, false);
}
/* glr_execute_one_vram_update: eseguire il TexSubImage R16UI solo se !s_texture_barrier */
```
Il dirty-rect proposto nel 2026-08-01 (§1.2) resta utile solo per il fallback senza barriera.

**Impatto atteso.**
- Circa 4 MB/campo di `memcpy` in meno sul thread di emulazione (0.2-0.4 ms/campo, STIMA).
- Un carico paragonabile in meno sul thread GPU.
- Sparisce un modo di esaurire il pool.

**Rischio per l'accuratezza.** Nessuno dove la barriera c'è: comportamento allineato a Vulkan. Senza barriera il percorso resta identico.

**Verifica.** Sezione 4.6 (uprobe su `glr_upload_vram`, `emu.gpu_pool()` per gli skip) e il bucket `vram_upload` prima/dopo.

---

### F3. Readback sincroni per GP0(80) e GP0(C0)

**Dove.**
- `src/gpu/gpu_commands.c:1254`: ogni GP0(80) legge dal GPU il rettangolo sorgente; `:1399` per GP0(C0).
- `src/gpu/renderer_gl.c:2421-2443`: attende `frames_pending == 0`, cioè la fine del campo precedente swap compreso. Poi il thread GPU esegue le op pendenti e chiama `glReadPixels` (`:2018-2065`), che svuota la pipeline.
- `src/gpu/vk/renderer_vk.c:1578-1597` e `:1181-1234`: command buffer allocato e liberato per chiamata, `vkQueueWaitIdle` a `:1217`.
- `src/gpu/gpu_commands.c:1262-1275`: dopo il readback la copia avviene pixel per pixel e il rettangolo destinazione viene ricaricato.

**Cosa succede (CERTO).** Ogni GP0(80) è un giro completo emu -> GPU -> emu. Succede anche quando il rasterizzatore non ha mai toccato il rettangolo sorgente, e quindi `gpu.vram.data` è già autorevole.

**Costo.** Misurato nel repo: 2.3 ms per un readback dell'intera VRAM, 115 ms di stallo per 50 chiamate in un secondo (`src/gpu/renderer_gl.c:2031-2034`). Oggi il readback è limitato al rettangolo, ma restano l'attesa del campo precedente (e quindi di un eventuale vsync, F1) e la sincronizzazione della GPU. STIMA: da 0.1 ms a un periodo di refresh per chiamata. Frequenza per gioco: DA MISURARE.

**Pseudo-soluzione.**
1. Tenere lato CPU una mappa dei tile "toccati dal rasterizzatore", per esempio 64x32 tile da 16x16 pixel (2048 bit).
   - Si marca la bounding box di ogni primitiva registrata, clippata alla drawing area in modo conservativo.
   - Si azzerano i tile interamente coperti da un upload o fill CPU, e quelli appena letti con un readback.
   ```c
   if (vram_rect_touches_gpu_tiles(gpu, src_x, src_y, w, h))      /* gestire il wrap a 1024/512 */
       renderer_read_vram_rect(&gpu->renderer, vram, src_x, src_y, w, h);
   vram_tiles_clear(gpu, src_x, src_y, w, h);                     /* ora la CPU è autorevole */
   ```
2. Secondo passo, più invasivo: eseguire GP0(80) interamente sul GPU come un batch "copia" (stesso shader, rispetto dei bit di maschera) e marcare il rettangolo destinazione come posseduto dal GPU.
3. Vulkan: un command buffer e una fence riutilizzati invece di allocazione e `vkQueueWaitIdle` a ogni chiamata.

**Impatto atteso.** Elimina gli stalli nei titoli che usano GP0(80) a ogni campo: effetti, CLUT animate, copie del framebuffer. Nullo nei titoli che non lo usano.

**Rischio per l'accuratezza.** Medio-basso: una scrittura GPU non marcata produrrebbe un dato vecchio. Mitigazioni:
- marcatura conservativa;
- un interruttore `ZS1_FORCE_READBACK=1` per l'A/B;
- il confronto CPU/GPU già disponibile con `emu.vram_compare` (`src/core/lua_debug.c:779`).

**Verifica.** Sezione 4.6 (conteggio con uprobe su `glr_read_vram_rect`) e 4.4 (stack off-CPU).

---

### F4. Vulkan: il batching è di fatto disattivato

**Dove.**
- `src/gpu/vk/renderer_vk.c:751`: `VKR_FLUSH` registra un batch se ci sono vertici pendenti.
- `src/gpu/vk/renderer_vk.c:796-806`: ogni setter fa `VKR_FLUSH` senza confrontare il valore nuovo con quello attuale; idem `:808-845` per scala, finestra texture, offset e drawing area.
- `src/gpu/gpu_commands.c:534-538`, `:623-626`, `:843-846`: ogni primitiva chiama 3-4 setter prima del push.
- `src/gpu/vk/renderer_vk.c:1365-1371`: tra due batch qualsiasi `vkCmdEndRendering` + `vkr_barrier_vram_feedback` + `vkCmdBeginRendering`.
- `src/gpu/vk/renderer_vk.c:1035-1096`: per ogni batch viewport, scissor, bind vertex buffer, bind descriptor set, bind pipeline, blend constants, push constants, draw (due volte per i semitrasparenti texturizzati).
- Tetto a 8192 batch (`src/gpu/vk/renderer_vk.h:23`) con scarto dei vertici (`renderer_vk.c:701-704`).
- Il contatore "Draw batches" della vista Pipeline (`src/debug_ui.cpp:2109`) è alimentato solo dal backend GL (`src/gpu/renderer_gl.c:1447`): su Vulkan il problema è invisibile nella UI.

**Cosa succede (CERTO).** Il primo setter della primitiva N chiude il batch della primitiva N-1. Ogni primitiva diventa così un batch con il proprio render pass dinamico e una barriera completa. Il backend GL invece confronta prima di fare flush (`src/gpu/renderer_gl.c:1136-1149`, `:1644-1693`).

**Costo (STIMA, DA MISURARE).** Un titolo 3D ha 2000-6000 primitive per campo. Sono altrettante barriere fragment -> fragment, con circa 12 comandi registrati ciascuna:
- 1-3 ms di CPU sul thread di render;
- una serializzazione della GPU che su iGPU può avvicinarsi al budget;
- oltre 8192 primitive, vertici scartati.

**Pseudo-soluzione.**
```c
void vkr_set_dither_mode(VkRenderer* r, bool e) {
    if (r->dither_enabled == e) return;          /* come glr_set_dither_mode */
    VKR_FLUSH(r); r->dither_enabled = e;
}
/* idem per tutti i setter, confrontando i valori già calcolati (scissor, tex_window) */

/* replay: restare nello stesso rendering finché il batch successivo non legge la VRAM */
bool reads_vram = b->textured || b->mask_test;
if (rendering && reads_vram && written_since_barrier) {
    vkr_end_vram_rendering(cmd, &rendering);
    vkr_barrier_vram_feedback(cmd, &r->vram);
    written_since_barrier = false;
}
```
Aggiungere anche `frame_events_record(FEV_DRAW_BATCH, ...)` in `vkr_record_batch`, per rendere il conteggio visibile. I batch che non campionano la VRAM restano dentro la stessa istanza di rendering: lì l'ordine di rasterizzazione garantisce la correttezza del blending.

**Impatto atteso.** Numero di batch ridotto a quello del backend GL (1-2 ordini di grandezza in meno); barriere solo dove servono.

**Rischio per l'accuratezza.** Nessuno se i confronti coprono esattamente i campi copiati nello snapshot del batch (`src/gpu/vk/renderer_vk.c:715-732`). Verifica pixel per pixel con `ZS1_DUMP_FRAME`, già compatibile tra i due backend (`src/gpu/vk/renderer_vk.c:1237-1242`).

---

### F5. GL: costo fisso per batch sul thread GPU

**Dove.**
- `src/gpu/renderer_gl.c:1318-1420`, per ogni batch:
  - circa 10 `glUniform*`, `glScissor`, `glUseProgram`;
  - `glTextureBarrier` incondizionato quando disponibile (`:1360-1361`);
  - bind del VAO;
  - quattro coppie `glBindBuffer` + `glBufferSubData`, tutte all'offset 0 (`:1376-1384`);
  - uno o due `glDrawArrays` e gli unbind finali.
- Il pool di vertici del campo è già contiguo (`s_pos/s_col/s_tex/s_tpg`, `:95-99`) e i VBO sono già dimensionati `VERTEX_BUFFER_LEN` (`:654-657`, `:687`).
- Flush incondizionati su E2/E3/E4/E5 anche a valore invariato: `glr_set_texture_window` (`:1176-1198`), `glr_set_draw_offset` (`:1597-1604`), `glr_set_drawing_area` (`:1609-1639`).
- Doppia copia dei vertici: push in `renderer->positions_data` (`:1054-1065`, `:1092-1131`), poi copia nel pool in `glr_draw` (`:1481-1484`).

**Cosa succede (CERTO).** Ogni batch riscrive l'inizio degli stessi quattro buffer, mentre il draw precedente che li legge può essere ancora in volo. Il driver deve quindi sincronizzarsi o duplicare il buffer; quanto costi dipende dal driver (DA MISURARE). I batch si spezzano a ogni cambio di texture, raw, dither o semitrasparenza, che nelle scene 3D si alternano di continuo.

**Pseudo-soluzione.**
```c
/* inizio del replay di uno slot (e, per il readback parziale, solo la parte nuova):
   un upload per attributo, o un unico VBO interleaved */
upload_range(position_buffer, s_pos[ri], uploaded_upto[ri], s_vtx[ri]); /* ... x4 */
/* in glr_draw_gl */
glDrawArrays(prim, (GLint)b->vertex_start, (GLsizei)b->vertex_count);  /* invece di 0 */
if (s_texture_barrier && (b->texture_enabled || b->mask_test_enabled) && written_since_barrier)
    glTextureBarrier();
/* uniform: cache lato thread GPU, glUniform solo se il valore cambia rispetto al batch precedente */
/* setter E2..E5: confronto prima del flush */
```
Il readback parziale (`src/gpu/renderer_gl.c:2018-2028`) rigioca op dello slot di scrittura a metà campo. L'upload deve quindi essere incrementale (`uploaded_upto`), per coprire i vertici registrati fino a quel momento.

A medio termine: texture, raw e dither come attributi per vertice (flat), così solo il modo di blending e lo scissor spezzano un batch.

**Impatto atteso (STIMA).** 30-70% del tempo CPU del thread GPU in scene con centinaia di batch. Sul thread di emulazione conta dove i thread sono accoppiati (F1, F3).

**Rischio per l'accuratezza.** Nessuno per upload unico, offset e barriera condizionale. Per gli attributi per vertice serve il confronto byte per byte delle immagini con `ZS1_DUMP_FRAME`.

---

### F6. Interprete: lavoro fisso per istruzione

Il percorso per istruzione è `system_run_frame` (`src/core/system.c:64-71`) -> `cpu_run_next_instruction` (`src/cpu/cpu_execution.c:92-229`). Le sotto-voci sono in ordine di rapporto guadagno/rischio.

**F6a. Ring di trace da 64 KB scritto a ogni istruzione.**
- Dove: `include/cpu.h:198-203` (due array da 8192 `uint32_t`), `src/cpu/cpu_execution.c:128-134`.
- CERTO: a ogni istruzione due store, aggiornamento dell'indice e del contatore. Il ring si ferma solo al primo crash (`src/cpu/cpu_exceptions.c:20-26`). Lo leggono solo i dump e la tab "Exec Trace" (`src/debug_ui.cpp:1068-1085`).
- STIMA: il ring percorre 64 KB ogni 8192 istruzioni, più della L1D di un core P Raptor Cove (48 KB) e dei core E (32 KB). Ogni 16 istruzioni porta due linee nuove in L1, e scaccia registri, i 6 KB della icache emulata (`include/cpu.h:184`), lo scratchpad e i campi caldi di `Interconnect`. 3-8% del tempo emu, DA MISURARE.
- Soluzioni, una a scelta:
  - `EXEC_TRACE_SIZE` a 1024 (8 KB, una riga in `include/cpu.h:198`);
  - registrare solo i target dei salti: 5-8 volte meno scritture, e il percorso lineare si ricostruisce dal disassembler;
  - abilitarlo solo con la workspace di debug aperta o con una variabile d'ambiente.
- Rischio: nessuno sul guest; meno storia nei dump.

**F6b. Controllo IRQ con read-modify-write di Cause a ogni istruzione.**
- Dove: `src/cpu/cpu_execution.c:22-49`, chiamato a `:114`.
- CERTO: due load da `Interconnect` e una scrittura di `cpu->cause` per istruzione.
- Soluzione a rischio quasi nullo: scrivere `cause` solo se il bit 10 cambia (`if ((cpu->cause ^ want) & 0x400) cpu->cause ^= 0x400;`).
- Soluzione più ampia: un flag "controllo necessario", tenuto alto finché `(I_STAT & I_MASK) != 0`. Va armato:
  - dove cambiano I_STAT/I_MASK (`src/core/bus_irq.c:17-31`, `src/core/bus.c:231-265`, che già azzera il downcount a `:259` e `:263`);
  - su MTC0 SR/CAUSE (`src/cpu/cpu_instructions.c:120-131`);
  - su RFE (`:143-153`).
  Il bit IP2 di Cause va calcolato al momento, in MFC0 (`:325`) e all'ingresso dell'eccezione (`src/cpu/cpu_exceptions.c:57-81`).
- Rischio medio-basso: un percorso dimenticato ritarda un IRQ. Verifica: CPI identico e milestone di boot negli stessi campi.

**F6c. Debugger interrogato a ogni istruzione.**
- Dove: `src/cpu/cpu_execution.c:155-160`, `src/core/debugger.c:71-75`: chiamata cross-TU, moltiplicazione di hash e load del filtro.
- Soluzione: `if (__builtin_expect(dbg->breakpoint_count | dbg->step_skip_bp, 0))` prima della chiamata.
- Rischio nullo.

**F6d. Doppio dispatch indiretto.**
- Dove: `src/cpu/cpu_decode.c:98-102` e, per SPECIAL (ADDU, OR, SLL, JR, ...), una seconda chiamata indiretta a `:92-96`. Con le tabelle di puntatori il compilatore non può inlineare i corpi, nemmeno con LTO.
- Soluzione: `switch` sull'opcode (o tabella unica a 128 voci) con gli handler `static inline` nello stesso TU. GCC genera una jump table e inlinea i corpi, evitando call/ret e la rilettura di `cpu->regs`.
- STIMA: 10-20% del tempo dell'interprete; DA MISURARE.
- Rischio nullo se gli handler restano identici.

**F6e. Helper minuscoli fuori linea.**
- Dove: `cpu_reg`/`cpu_set_reg` in `src/cpu/cpu_registers.c:15-46`, con controllo `index >= 32` e chiamata al logger; `mask_region` in `src/core/bus.c:31-33`.
- Gli indici vengono da campi a 5 bit (`include/cpu.h:212-214`), quindi il controllo è morto. Il Makefile stesso attribuisce a queste due funzioni l'1.90% dei campioni (`Makefile:35-37`).
- Soluzione: `static inline` negli header, senza controllo.
- Rischio nullo.

**F6f. Contabilità per istruzione tramite puntatore.**
- Dove: `src/cpu/cpu_execution.c:214-218` (tre read-modify-write su `Interconnect`, uno a 64 bit), `:64-71` (rotazione del load delay), `:206` (`regs[0] = 0`, ridondante con `cpu_set_reg`).
- Soluzione: accumulare in variabili locali dentro un loop interno e riversare su `inter` solo al dispatch. Dipende da F6g.
- Rischio nullo se l'ordine di `cpu_cycle_counter` rispetto agli eventi resta quello di oggi.

**F6g. Una chiamata cross-TU per istruzione dal driver del campo.**
- Dove: `src/core/system.c:64-71` chiama `cpu_run_next_instruction` in un altro TU, e controlla `frame_complete`, `paused` e il cap a ogni istruzione. Eppure `frame_complete` lo scrive solo l'evento VBlank (`src/core/event_scheduler.c:196`) e `paused` solo `debugger_handle_break` (`src/core/debugger.c:243-250`).
- Soluzione: un `cpu_run_frame(cpu)` in `cpu_execution.c`, con loop interno su `downcount > 0` e i controlli rari solo dopo `eventq_dispatch_due` o dopo un hit del debugger.
- Rischio nullo.

**F6h (opzionale). Campi caldi sparsi.**
- In `Interconnect` i campi letti a ogni istruzione stanno su linee diverse, dopo strutture da centinaia di KB:
  - `irq_status/irq_mask` a `include/interconnect.h:132-133`;
  - `evq_next_cycle/cpu_cycle_counter/frame_complete` a `:148-150`;
  - `debugger` a `:179`;
  - `cpu_mem_stall_cycles/instructions_retired` a `:195-201`.
- In `Cpu`, `downcount` (`include/cpu.h:193`) sta dopo la icache e la GTE.
- Raggrupparli in una linea è un micro-guadagno, ma cambia il layout serializzato dagli stati salvati: va fatto solo con un bump di versione.

**Impatto complessivo (STIMA).** 15-35% del tempo emu, cioè 0.5-1.2 ms/campo sulla macchina del proprietario, e proporzionalmente di più su CPU lente.

---

### F7. Build: LTO condizionale e nessun PGO

**Dove.**
- `Makefile:57-65`: LTO solo se `gcc` e `g++` hanno la stessa major. Il sistema descritto in `Makefile:47-50` ha gcc 14.2 e g++ 13.3, quindi un `make` semplice costruisce senza LTO (lo stampa come `[build] LTO off`).
- `Makefile:26` (`-O3 -g -march=native -DNDEBUG $(LTO)`), `Makefile:273` (link con `$(CXX)`).

**Cosa succede (CERTO).** Senza LTO restano chiamate vere tutte le funzioni cross-TU del percorso caldo: `cpu_icache_fetch`, `cpu_reg`, `cpu_set_reg`, `mask_region`, `debugger_check_*`, `interconnect_load*/store*`, `ram_load*/store*`, `vram_load16/store16`, `decode_and_execute`, `cpu_run_next_instruction`. Misurato nel repo: LTO vale il 7.7% (`Makefile:40-45`).

**Pseudo-soluzione.**
1. LTO solo per il C, indipendente da `g++`: togliere `$(LTO)` da `CXXFLAGS` e linkare con `$(CC) ... $(LTO) -lstdc++`. Gli oggetti C++ (ImGui, `debug_ui.cpp`) restano oggetti ELF normali; la libstdc++ di gcc-14 è retrocompatibile con oggetti compilati da g++-13.
2. Spostare gli helper minuscoli in header come `static inline` (F6e): così le prestazioni non dipendono più da LTO.
3. Target PGO: `make pgo-gen` (`-fprofile-generate`), 60 s su una scena fissa, poi `make pgo-use` (`-fprofile-use -fprofile-partial-training`). Per un interprete il guadagno tipico viene dal layout dei salti e dalla promozione delle chiamate indirette; entità DA MISURARE.

**Rischio per l'accuratezza.** Nessuno. Il controllo è il CPI, che nessuna ottimizzazione host può muovere (`Makefile:44-45`).

---

### F8. Bus: percorso RAM

**Dove.**
- `src/core/bus.c:571-609` (`interconnect_load32`), in sequenza:
  - test di allineamento;
  - `debugger_check_read_watchpoint`: chiamata e due hash anche senza watchpoint (`src/core/debugger.c:196-201`);
  - `mask_region`: load dalla tabella `REGION_MASK` (`:24-33`);
  - `bus_charge_cpu_load` (`:550-560`), che legge una static;
  - `ram_load32` in un altro TU, con controllo di limiti (`src/core/ram.c:21-44`), attraverso il puntatore `inter->ram` (`include/interconnect.h:123`).
- Stessa forma per `load16`/`load8` (`:611-658`) e per gli store (`:664-750`).
- Gli stessi accessori servono i DMA parola per parola: `src/core/bus.c:815`, `:827`, `:863`, `:943`, `:968`, `:1129`, `:1174`, `:1201`.

**Pseudo-soluzione.**
```c
extern bool g_dbg_watch_active;            /* aggiornato quando cambiano le liste dei watchpoint */
extern const uint32_t REGION_MASK[8];      /* già esportata da bus.c */
static inline bool bus_try_ram_load32(Interconnect* in, uint32_t a, uint32_t* out) {
    uint32_t phys = a & REGION_MASK[a >> 29];          /* identico a mask_region */
    if (__builtin_expect((a & 3) | g_dbg_watch_active | (phys >= 0x00800000u), 0)) return false;
    in->cpu_mem_stall_cycles += g_ram_load_stall;      /* stesso costo di oggi */
    memcpy(out, &in->ram->data[phys & (RAM_SIZE - 1)], 4);
    return true;
}
/* op_lw: if (!bus_try_ram_load32(...)) v = interconnect_load32(...);  percorso lento invariato */
```
Il controllo dell'isolamento della cache (`src/cpu/cpu_instructions.c:181`) resta prima. Una LUT di pagine (fastmem) è l'evoluzione naturale. Per i DMA leggere la nota N1 prima di toccare qualcosa.

**Impatto atteso (STIMA).** 5-10% del tempo emu. `interconnect_load32` da sola era il 4.3% dei campioni (`src/core/bus.c:512-515`).

**Rischio per l'accuratezza.** Basso, se la condizione replica `mask_region` e il costo di stallo è lo stesso. I watchpoint e tutte le regioni diverse dalla RAM restano sul percorso lento.

---

### F9. Logging

**Dove.**
- `include/log.h:99-205`: ogni macro `LOG_*` è una chiamata incondizionata a `log_print`.
- `src/utils/log.c:233-245`: il filtro di livello sta dentro una funzione variadica, che GCC non inlinea. `current_log_level` è già una globale non statica (`src/utils/log.c:19`).
- Ogni riga emessa paga:
  - `snprintf` dello stamp e chiamata al clock (`src/utils/log.c:203-215`);
  - la sink, con `std::mutex`, `std::string` e `fprintf` (`src/debug_ui.cpp:548-570`), su un file in `_IOLBF` (`src/debug_ui.cpp:3495`), quindi una `write()` per riga;
  - un secondo `snprintf` da 720 byte e `rxi_log_log` con lock (`src/utils/log.c:219-221`).
- Chiamate su percorsi caldi:
  - DEBUG con 16 argomenti per ogni rettangolo texturizzato (`src/gpu/gpu_commands.c:193-199`) e per ogni quad texturizzato (`:853-859`);
  - TRACE per primitiva (`:537`, `:557`, `:627`, `:657`, `:737`, `:767`, `:890`, `:956`);
  - TRACE per operazione GTE (`src/gte/gte_ops.c:97`, `:111`, `:126`, `:131`, `:141`, ...);
  - DEBUG per quad e per batch nel renderer (`src/gpu/renderer_gl.c:1086`, `:1515`);
  - DEBUG per macroblocco (`src/core/mdec.c:312`, `:331`, `:347`);
  - TRACE per blocco ADPCM (`src/spu/spu_voice.c:232`).

**Cosa succede (CERTO).** Anche a INFO ognuna di queste valuta gli argomenti, li spilla sullo stack oltre il sesto (ABI SysV) e fa una chiamata con ritorno immediato; nei loop chiamanti vincola l'allocazione dei registri. `CLAUDE.md` dice che TRACE "never in default builds", ma il codice non lo compila via.

**Costo (STIMA).**
- 5-20 ns per chiamata scartata. Un titolo 3D ne fa qualche migliaio per campo, quindi 10-60 µs per campo (1-2%).
- 2-10 µs per ogni riga emessa, tra formattazione, mutex e syscall: rilevante in DEBUG e negli storm di WARN.

**Pseudo-soluzione.**
```c
extern LogLevel current_log_level;
#ifndef ZS1_LOG_MAX_LEVEL
#define ZS1_LOG_MAX_LEVEL LOG_LEVEL_DEBUG     /* TRACE compilato via nelle build normali */
#endif
#define LOG_AT(cat, lvl, ...) do {                                              \
    if ((lvl) <= ZS1_LOG_MAX_LEVEL &&                                           \
        __builtin_expect((int)(lvl) <= (int)current_log_level, 0))              \
        log_print((cat), (lvl), __VA_ARGS__);                                   \
} while (0)
#define LOG_GPU_DEBUG(...) LOG_AT(LOG_CAT_GPU, LOG_LEVEL_DEBUG, __VA_ARGS__)
```
Inoltre:
- file di log in `_IOFBF` da 64 KB, con `fflush` a fine campo e nel gestore di segnale esistente (`src/main.c:404-408`);
- le variabili usate solo nei log (per esempio `dbg_page_x` a `src/gpu/gpu_commands.c:188-192` e `:848-852`) vanno spostate dentro gli argomenti, per non rompere la regola "zero warning" quando la macro sparisce.

**Rischio per l'accuratezza.** Nessuno. Lato diagnosi: con il buffering pieno un crash duro (SIGSEGV) può perdere le ultime righe se il gestore non fa flush.

---

### F10. GPU lato CPU: lavoro per pixel

**Dove.**
- GP0(A0), `src/gpu/gpu_commands.c:1603-1623`:
  - per ogni parola due coppie `idx % vram_load_w` / `idx / vram_load_w`, con divisore variabile;
  - due chiamate a `vram_write_masked` (`:131-140`), che arrivano a `vram_store16` in un altro TU con due controlli (`src/gpu/vram.c:80-91`);
  - a `:1627` un `getenv()` a ogni upload completato.
- Fill: `src/gpu/gpu_commands.c:359-366`, un `vram_store16` per pixel (76800 per un clear 320x240).
- GP0(80): `src/gpu/gpu_commands.c:1262-1273`, `vram_load16` + `vram_write_masked` per pixel.
- GPUREAD in modalità C0: `src/gpu/gpu.c:567-578`, stesse divisioni; `src/gpu/gpu_commands.c:1403`, `getenv()` a ogni GP0(C0).
- `gpu_gp0`: `src/gpu/gpu_commands.c:1703-1723`, ogni parola entra in una FIFO da 16 voci che viene subito svuotata.

**Costo (STIMA).** Un frame FMV 320x240 sono 38400 parole A0: circa 77 mila divisioni da 20-40 cicli e 77 mila chiamate, cioè 0.5-1 ms per frame video sul thread di emulazione.

**Pseudo-soluzione.**
```c
/* A0: una divisione per parola invece di quattro (nessun campo nuovo, savestate invariato) */
uint32_t x = idx % w, y = idx / w;
write_px(x, y, p1);
if (++x == w) { x = 0; ++y; }
write_px(x, y, p2);
/* caso comune senza force_set_mask_bit né preserve_masked_pixels: store diretto su (uint16_t*)vram.data */
/* fill: loop per riga su uint16_t*, due segmenti se la riga attraversa x = 1024 */
/* getenv: cache statica come in trace_textured_primitive (gpu_commands.c:237-239) */
```

**Rischio per l'accuratezza.** Nessuno, se le maschere restano applicate per pixel nel caso generale.

---

### F11. Probe Lua: `emu.on_event` riceve ogni notifica

**Dove.**
- `src/core/lua_debug.c:252-258`: un solo callback, nessun filtro.
- `src/core/lua_debug.c:960-966`: ogni notifica fa `lua_rawgeti`, `lua_pushstring` e `lua_pcall`.
- Notifiche su percorsi caldi:
  - per ogni operazione GTE (`src/gte/gte.c:165`);
  - due per ogni poligono (`src/gpu/gpu_commands.c:1683`, `:1692`);
  - per ogni rettangolo (`:1684`) e per ogni fill (`:368`);
  - per ogni macroblocco (`src/core/mdec.c:352`);
  - per ogni settore (`src/cdrom/cdrom_commands.c:993`);
  - per ogni DMA ch2 (`src/core/bus.c:794`).

**Cosa succede (CERTO).** Appena uno script registra `on_event`, ogni notifica entra nell'interprete Lua, e il filtro sul nome avviene in Lua. Anche `scripts/host_speed.lua` campiona ogni 300 vblank proprio per non perturbare (righe 5-8), ma registra `on_event` alla riga 10 e viene quindi invocato migliaia di volte per campo.

**Costo (STIMA).** 100-300 ns per invocazione. Un titolo 3D produce 5-10 mila notifiche per campo, quindi 0.5-3 ms per campo: la stessa grandezza del tempo emu. Le misure di velocità prese con un probe attivo non sono confrontabili con quelle senza.

**Pseudo-soluzione.** Notifiche identificate da un enum invece che da una stringa, più una bitmask di sottoscrizione in C:
- `emu.on_event(fn, "vblank", "mdec_macroblock")`;
- in `lua_debug_notify(id)`: `if (!(g_event_mask & (1u << id))) return;`;
- senza nomi, comportamento attuale (tutti gli eventi).

**Rischio per l'accuratezza.** Nessuno.

---

### F12. Timer: eventi programmati anche senza IRQ armato

**Dove.**
- `src/core/timers.c:510-516`: ogni evento ri-arma il timer.
- `:473-482`: `timers_reschedule` programma il prossimo confine se il timer conta, indipendentemente da `irq_on_target`/`irq_on_ffff`.
- `:330-355`: `timer_rate_cycles`, per i timer 0 e 1 con sorgente dotclock/hblank, fa due operazioni in double a ogni chiamata, cioè a ogni lettura del contatore (`:401`) e a ogni evento (`:476`).

**Cosa succede (CERTO).** Un timer con target piccolo e reset al target genera un evento ogni `target * rate` cicli, anche se nessuno ha chiesto l'IRQ. Contatore e flag vengono già ricalcolati in lettura (`:398-469`). Ma **la lettura del registro MODE non fa catch-up** (`:203-219`): oggi i flag `reached_*` arrivano in tempo solo perché l'evento scatta a ogni confine.

**Costo.** Irrilevante per un timer libero (un evento ogni 65536 cicli). Potenzialmente decine di migliaia di dispatch per campo per un timer con target basso senza IRQ. DA MISURARE.

**Pseudo-soluzione.**
```c
/* timer_read16, caso TMR_REG_MODE: prima di comporre i bit */
timers_catch_up_one(timers, timer_index);
/* timers_reschedule: programmare solo se il prossimo confine può alzare un IRQ */
bool can_irq = (t->reset_on_target && t->target && t->irq_on_target) || t->irq_on_ffff
            || (!t->reset_on_target && t->irq_on_target);   /* adattare alla semantica esistente */
if (!can_irq) { timers->inter->evq_pending &= ~(1u << (EVQ_TIMER0 + i)); return; }
/* rate: memorizzato in t->rate su scrittura del modo e su cambio di modo video (GP1(08)), non ricalcolato */
```

**Rischio per l'accuratezza.** Basso, a condizione di aggiungere il catch-up nella lettura di MODE. Verifica: stessi milestone di boot, più uno script che legga MODE in un loop su un timer senza IRQ.

---

### F13. CD: lettore asincrono a settore singolo, tempi emulati dipendenti dall'I/O host

**Dove.**
- `src/cdrom/cdrom_disc.c:638-667`: un settore per richiesta.
- `:693-727`: queue e poll sotto mutex.
- `src/cdrom/cdrom_commands.c:818-831`: se il settore non è pronto si riprova dopo `CDROM_READ_RETRY_DELAY`, circa 0.21 ms emulati (`include/cdrom.h:107`).
- `:926-933`: il settore successivo viene chiesto solo dopo la consegna del precedente.
- Lettura ECM con ricostruzione dell'EDC/ECC: `src/cdrom/cdrom_ecm.c:188-221`.

**Cosa succede (CERTO).** Se il thread lettore non ha finito, la consegna slitta di multipli di 0.21 ms **emulati**. Il tempo emulato dipende quindi dalla velocità dell'I/O host, e due run dello stesso disco possono divergere. STIMA: a 5x il tempo reale, i 6.7 ms emulati tra due INT1 a 2x sono circa 1.3 ms host, e una lettura a freddo o una ricostruzione ECM li supera facilmente.

Ipotesi DA MISURARE: contribuisce allo scarto "ours 130/s against their 152/s" registrato in `CLAUDE.md` (Monsters & Co., punto 4).

**Pseudo-soluzione.**
- Read-ahead di 8-32 settori in un ring nel lettore, avviato da ReadN/ReadS/SeekL e invalidato da Setloc.
- Consumo senza mutex con indici atomici, come il ring audio (`src/spu/spu_mixing.c:530-548`).
- Un contatore dei PENDING esposto a Lua.

**Impatto atteso.** Piccolo sul costo (meno mutex e dispatch); grande sulla riproducibilità dei confronti per campo.

**Rischio per l'accuratezza.** Nessuno se la consegna resta allo stesso deadline; la riproducibilità migliora.

---

### F14. MDEC

**Dove.**
- `src/core/mdec.c:182-201`: IDCT densa a due passate con accumulatore `int64_t`, 1024 MAC per blocco e una divisione `/ 8` nel loop interno.
- `:209-231`: YUV -> RGB in float per pixel; la crominanza di ogni 2x2 viene ricalcolata quattro volte.
- `:45-49`: `% 768` a ogni `out_push`.
- `:547-555`: ogni parola DMA rientra in `mdec_execute`.

**Costo (STIMA).** 300 macroblocchi x 6 blocchi = 1800 IDCT per frame 320x240, cioè 0.5-1 ms per frame video.

**Osservazione (CERTO sulle clamp, STIMA sul bound).** I coefficienti escono da `clamp_s32` a [-0x400, 0x3FF] (`:134`, `:152`), e `scale_table / 8` è al massimo 4096 in modulo. La prima passata resta quindi entro 8 x 1024 x 4096 = 2^25, la seconda entro circa 2^27. Un accumulatore `int32_t` è esatto, e con `-march=native` GCC può vettorizzarlo (AVX2, 8 lane).

**Pseudo-soluzione.**
```c
/* mdec_handle_set_scale (mdec.c:381-405): precalcolare con la stessa divisione C, non con >> */
for (i = 0; i < 64; i++) m_scale8[i] = (int32_t)m->scale_table[i] / 8;
/* mdec_idct: int32_t sum; saltare le colonne di src tutte nulle nella prima passata */
/* yuv_to_rgb: calcolare R, G, B di crominanza una volta per 2x2 con le stesse espressioni float */
```
`m_scale8` va tenuto fuori da `Mdec` (o ricalcolato al load), per non cambiare la dimensione serializzata.

**Rispetto a `docs/MDEC_OFFLOAD_DESIGN_2026-08-01.md`.** Il documento propone un pool di worker con interazioni sui savestate (§6). Parte dalla premessa che l'FMV sia "the largest single work spike", che nel repository non è misurata. Rendere il kernel 3-5x più economico su un solo thread non tocca né il protocollo né gli stati salvati: va fatto e misurato prima del pool.

**Rischio per l'accuratezza.** Nullo se bit-exact. Verifica byte per byte dello stream `mdec_dma_out`, come nel §7 del documento di offload.

---

### F15. Scheduler di eventi

**Dove.**
- `src/core/event_scheduler.c:89-111`: la scansione dei 12 eventi si ripete finché ne scatta qualcuno, quindi almeno due volte per dispatch.
- `:117-132`: terza scansione, per trovare il prossimo.
- `src/cpu/cpu_execution.c:221-228`.
- I `downcount -=` di `src/core/bus.c:902`, `:1007`, `:1226` producono dispatch anticipati che non fanno nulla (vedi N1).

**Costo (STIMA).** 50-150 ns per dispatch. Le slice del DMA GPU arrivano ogni circa 68 cicli (`src/core/bus.c:756`, `:779`, `:901`), quindi qualche centinaio di dispatch per campo: meno dell'1%.

**Pseudo-soluzione.** Un solo passaggio che, mentre esegue gli handler, calcola il minimo dei target rimasti. Ripetere solo se un handler ha programmato qualcosa già scaduto.

**Rischio per l'accuratezza.** Nullo se l'ordine di esecuzione per indice resta lo stesso.

---

### F16. SPU

**Dove.**
- `src/spu/spu_mixing.c:319` -> `src/spu/spu.c:59-107`: due loop da 24 per campione, anche con `key_on == key_off == 0`.
- `src/spu/spu_mixing.c:331-343`: 24 chiamate a `spu_voice_get_sample` e a `spu_voice_sweep_tick` anche per le voci spente (`src/spu/spu_voice.c:241-247`, `:349-394`).
- `:102-110`: `%` in `rev_addr` per ogni lettura/scrittura del riverbero, circa 30 per passo a 22050 Hz.
- `:251-257`: FIR a 39 tap, di cui 19 nulli.

**Costo (STIMA).** 300-1500 cicli per campione, 882 campioni per campo PAL: 0.05-0.3 ms per campo. Misurabile direttamente con il bucket `spu` di `ZS1_FRAME_PROFILE` (`src/main.c:938-944`, `src/spu/spu_mixing.c:571-590`).

**Pseudo-soluzione.**
- `if (!(spu->key_on | spu->key_off)) return;` in testa a `spu_process_key_on_off`.
- Bitmask delle voci attive (`voice->on` si aggiorna già a key on e a fine campione).
- Wrap del riverbero con sottrazione condizionale (gli offset sono limitati).
- FIR solo sui tap non nulli.

**Rischio per l'accuratezza.** Nullo: aritmetica identica. Il piano AVX2 per la SPU del 2026-08-01 (§3.2) non è giustificato prima di aver letto quel bucket.

---

### F17. Passata del VRAM viewer a ogni campo

**Dove.** `src/gpu/renderer_gl.c:2159-2178` e `src/gpu/vk/renderer_vk.c:1392-1400`: la decodifica dell'intera VRAM per il viewer di debug gira a ogni campo. Succede anche nella shell di gioco, che non disegna pannelli (`src/debug_ui.cpp:4198-4203`).

**Costo (STIMA).** 524288 frammenti per campo: trascurabile su RTX 4060, 0.2-0.5 ms su iGPU.

**Pseudo-soluzione.** Un campo `enabled` in `VramViewParams`, alzato dalla UI solo quando il viewer è visibile (`g_show_vram_viewer`, `src/debug_ui.cpp:4212`); se è falso la passata salta.

**Rischio per l'accuratezza.** Nullo.

---

### F18. GTE

**Dove.** `src/gte/gte.c:164-165` (`gte_update_error_flag` e `lua_debug_notify` per ogni operazione); TRACE in `src/gte/gte_ops.c` (F9).

**Costo (STIMA).** Meno dell'1%. Le operazioni sono intere e brevi; il costo aggiunto sono le due chiamate per operazione, già coperte da F9 e F11.

---

### F19. Punti ciechi di `ZS1_FRAME_PROFILE`

**Dove.** `src/main.c:886-967`:
- `t0` è preso dopo `renderer_wait_frame_done` (`:875`) e dopo il polling degli eventi: l'attesa del thread GPU non compare (F1).
- Il pacing (`:981-995`) e il thread GPU non sono misurati.
- Il bucket `vram_upload` (`t1 -> t2`) contiene anche `debug_ui_render` (`:919`), non solo `renderer_upload_vram` (`:924`).
- Il bucket `viewer` (`t2 -> t3`) è vuoto da quando la conversione è passata al GPU (`:927-932`).
- Si stampano solo medie su 60 campi: gli stalli di F3 sono picchi e spariscono nella media.
- Con il profilo attivo, `spu_catch_up` chiama `SDL_GetPerformanceCounter` due volte per ogni accesso a un registro SPU (`src/spu/spu_mixing.c:575-589`): poca cosa, ma è strumentazione sul percorso misurato.

**Pseudo-soluzione.**
- Bucket `wait`, `ui`, `upload` e `pace` separati.
- Il massimo oltre alla media.
- Nel thread GPU, i tempi di replay, scanout e swap (`src/gpu/renderer_gl.c:2109-2115`, `:2236`), pubblicati con atomici e stampati nella stessa riga `[PROF]`.

**Rischio per l'accuratezza.** Nullo.

---

## 4. Piano di misura

### 4.1 Regole (da `CLAUDE.md`, valgono per ogni passo)

- Nessuna misura di velocità con `ZS1_LOG_STDERR`, con probe Lua (F11) o con breakpoint attivi.
- Shell di gioco (default: `ZS1_UI` non impostata, oppure `ZS1_UI=gameplay`).
- Tre run per configurazione; si prende la mediana.
- Prima di misurare, controllare la data del binario: `ls -l --time-style=full-iso ZoniStation_One`.
- La riga `[PROF]` è un `LOG_SYSTEM_INFO`. Finisce in `logs/System.log`, che è aperto in `"w"` a ogni avvio (`src/debug_ui.cpp:3488-3497`): va copiata dopo ogni run.
- Il controllo di non regressione del guest è il `CPI=` della stessa riga: nessuna modifica host può spostarlo (`Makefile:44-45`). Per le modifiche al renderer, confronto byte per byte di `ZS1_DUMP_FRAME`; GL e Vulkan producono dump confrontabili (`src/gpu/vk/renderer_vk.c:1237-1242`).
- CPU ibrida (i9-14900HX):
  - `lscpu --extended` per individuare i core P (MAXMHZ più alto);
  - lanciare con `taskset -c <lista core P>`, così il main thread non finisce su un core E tra una run e l'altra;
  - con perf, sui core P, usare gli eventi `cpu_core/...`.
- Scena fissa: arrivare a una scena 3D e salvare con F5. In ogni run caricare con F8 (`src/main.c:757-764`) e lasciar girare almeno 30 s prima di leggere `[PROF]`.

### 4.2 Build

```sh
make clean && make 2>&1 | tee build.log
grep '\[build\]' build.log                 # "LTO off" conferma F7 sulla macchina
# variante con LTO e toolchain coerente
make clean && make CC=gcc-13 CXX=g++-13
# variante per perf con frame pointer (stessa ottimizzazione, unwinding economico)
make clean && make CC=gcc-13 CXX=g++-13 \
     OPT="-O3 -g -march=native -DNDEBUG -flto=auto -fno-omit-frame-pointer"
```
Una variabile `OPT` passata sulla riga di comando sostituisce quella del Makefile (`Makefile:23-27`), quindi `-flto=auto` va ripetuto a mano.

### 4.3 Baseline

```sh
for i in 1 2 3; do
  ZS1_FRAME_PROFILE=1 timeout 90 taskset -c 0-15 \
      ./ZoniStation_One roms/bios-pal.bin --game="games/game.bin"
  grep '\[PROF\]' logs/System.log > prof_base_$i.txt
done
```
Mentre gira, in un altro terminale, la CPU per thread:
```sh
pidstat -t -p "$(pidof ZoniStation_One)" 1 30 > pidstat_base.txt
```
Thread attesi:
- il main thread (stesso TID del PID);
- `GPU` (`src/gpu/renderer_gl.c:2290`) oppure `GPU-VK` (`src/gpu/vk/renderer_vk.c:1532`);
- `cdrom-read` (`src/cdrom/cdrom_disc.c:678`);
- il thread audio di SDL.

### 4.4 F1: accoppiamento con lo swap (nessuna modifica di codice)

```sh
# NVIDIA
__GL_SYNC_TO_VBLANK=1 ZS1_GPU=nvidia ZS1_FRAME_PROFILE=1 ./ZoniStation_One ... &
__GL_SYNC_TO_VBLANK=0 ZS1_GPU=nvidia ZS1_FRAME_PROFILE=1 ./ZoniStation_One ... &
# Mesa / Intel
vblank_mode=3 ZS1_GPU=intel ZS1_FRAME_PROFILE=1 ./ZoniStation_One ... &
vblank_mode=0 ZS1_GPU=intel ZS1_FRAME_PROFILE=1 ./ZoniStation_One ... &
# Vulkan (FIFO fisso, vk_device.c:348)
ZS1_GFX=vulkan ZS1_FRAME_PROFILE=1 ./ZoniStation_One ... &
```
Per ciascuna configurazione, dove dorme il main thread:
```sh
PID=$(pidof ZoniStation_One)
perf record -e sched:sched_switch --call-graph fp -t "$PID" -o offcpu.data -- sleep 20
perf report -i offcpu.data --stdio --no-children --sort symbol | head -60
perf sched record -o sched.data -- sleep 20
perf sched timehist -i sched.data -s | grep -E 'ZoniStation|GPU'
```
- **Conferma**: con il sync attivo una quota non trascurabile degli switch del main thread ha nello stack `glr_wait_frame_done` / `vkr_wait_frame_done`, oltre a `SDL_Delay` del pacing. Inoltre gli underrun (vista Audio, `src/debug_ui.cpp:2471-2472`, letti a fine run) crescono. Con il sync spento spariscono.
- **Smentita**: l'attesa è assente o trascurabile in entrambi i casi.

### 4.5 Hotspot del thread di emulazione (F6, F8, F9, F10, F14, F16)

```sh
PID=$(pidof ZoniStation_One)
perf record -e cpu_core/cycles/ -F 1999 --call-graph fp -t "$PID" -o emu.data -- sleep 30
perf report -i emu.data --stdio --no-children --sort symbol --percent-limit 0.3 > emu_report.txt
perf annotate -i emu.data --stdio -s cpu_run_next_instruction > annot_cpu.txt
perf stat -e cpu_core/cycles/,cpu_core/instructions/,cpu_core/branch-misses/,\
cpu_core/L1-dcache-loads/,cpu_core/L1-dcache-load-misses/ -t "$PID" -- sleep 30
```
- **F6a**: ridurre `EXEC_TRACE_SIZE` a 1024 (una riga in `include/cpu.h:198`). Confermato se `L1-dcache-load-misses` del main thread scende di oltre il 10% e `emu` in `[PROF]` scende, con `CPI=` identico. Smentito se nessuna delle due cambia.
- **F6d/F7**: guardare il tasso di `branch-misses` e la quota di `decode_and_execute`/`op_special` nel report; ripetere con LTO e con PGO e confrontare `emu`.
- **F8**: confermato se `interconnect_load32` + `ram_load32` + `debugger_check_read_watchpoint` + `mask_region` superano insieme il 5%.
- **F9**:
  - quota di `log_print`, `vsnprintf`, `log_sink_callback` e `__write`;
  - righe emesse in 60 s al livello di default, con `wc -l logs/*.log`;
  - ripetere con `ZS1_LOG_LEVEL=error`: se `emu` cambia in modo misurabile, il volume di log conta.
- **F10/F14** (durante un FMV): quota di `gpu_gp0_handle_word`, `vram_write_masked`/`vram_store16`, `mdec_idct`, `mdec_yuv_to_rgb`. F14 è confermato se `mdec_idct` supera il 5% durante il filmato.
- **F16**: bucket `spu` di `[PROF]`; `ZS1_SPU_NO_REVERB=1` come A/B dà la quota del riverbero.

### 4.6 Conteggio di eventi rari con uprobe (F2, F3, F12, F13)

Gli uprobe costano qualche microsecondo per hit. Vanno bene per funzioni chiamate al massimo qualche migliaio di volte al secondo, non per le funzioni calde.
```sh
B=./ZoniStation_One
sudo perf probe -x $B --funcs | grep -E 'read_vram_rect|upload_vram|event_handler|execute_drive'
sudo perf probe -x $B --add glr_read_vram_rect
sudo perf probe -x $B --add glr_upload_vram
sudo perf probe -x $B --add timer0_event_handler
sudo perf probe -x $B --add timer1_event_handler
sudo perf probe -x $B --add timer2_event_handler
sudo perf probe -x $B -L cdrom_execute_drive        # trovare la riga del retry (cdrom_commands.c:830)
sudo perf probe -x $B 'cdrom_execute_drive:<riga relativa del retry>'
sudo perf stat -e 'probe_ZoniStation_One:*' -p "$(pidof ZoniStation_One)" -- sleep 30
sudo perf probe --del 'probe_ZoniStation_One:*'
```
Se LTO ha inlineato una funzione, il probe non si crea. In quel caso usare la build senza LTO solo per il conteggio: il conteggio non dipende dalla velocità.
- **F2**: confermato se `glr_upload_vram` scatta almeno 2 volte per campo (100 al secondo a 50 Hz) sul backend GL. Per gli skip del pool, in una run separata dedicata al conteggio, leggere `emu.gpu_pool()` (`src/core/lua_debug.c:367-375`) ogni 250 vblank: skip maggiori di zero confermano l'effetto collaterale.
- **F3**: confermato se `glr_read_vram_rect` scatta durante il gameplay (non solo nel menu del BIOS) e lo stack off-CPU del passo 4.4 mostra tempo lì.
- **F12**: confermato se `timerN_event_handler` scatta molto più spesso degli IRQ timer effettivi. Gli IRQ si contano con `emu.irq` (`src/core/lua_debug.c:781`), in una run separata.
- **F13**: confermato se il probe sul retry conta hit durante un caricamento e il numero cresce con la cache del disco fredda (`echo 3 | sudo tee /proc/sys/vm/drop_caches` prima della run).

### 4.7 Thread GPU (F4, F5, F17)

```sh
GTID=$(ps -L -o tid=,comm= -p "$(pidof ZoniStation_One)" | awk '$2 ~ /^GPU/ {print $1; exit}')
perf record -F 1999 --call-graph fp -t "$GTID" -o gpu.data -- sleep 20
perf report -i gpu.data --stdio --no-children --sort dso,symbol --percent-limit 0.5 > gpu_report.txt
```
- Numero di batch per campo su GL: vista Pipeline della workspace di debug, voce "Draw batches" (`src/debug_ui.cpp:2109`). È un conteggio, non una misura di velocità, quindi la workspace aperta non lo falsa.
- **F5**: confermato se nel report del thread GPU domina la libreria del driver (`libnvidia-glcore` oppure `iris_dri.so`/`libgallium`) e il `%CPU` del thread `GPU` in `pidstat` cresce con il numero di batch.
- **F4**: confermato se, sulla stessa scena, il `%CPU` di `GPU-VK` è molto superiore a quello di `GPU`. In alternativa, una cattura RenderDoc (`renderdoccmd capture ./ZoniStation_One ...`) mostra una `vkCmdPipelineBarrier` e un `vkCmdBeginRendering` per primitiva.
- **F17**: stessa cattura RenderDoc, oppure A/B dopo il gate; rilevante solo su `ZS1_GPU=intel`.

### 4.8 Perturbazione dei probe (F11)

```sh
ZS1_FRAME_PROFILE=1 ./ZoniStation_One ...                                    # senza probe
ZS1_FRAME_PROFILE=1 ZS1_LUA_SCRIPT=scripts/host_speed.lua ./ZoniStation_One ...
```
Confermato se `emu` cresce di oltre il 5% con lo script, che dichiara di non perturbare.

---

## 5. Rispetto ai documenti del 2026-08-01

### `docs/HARDWARE_UTILIZATION_ANALYSIS_2026-08-01.md`

- **§1.1, `memcpy` da 128 byte per istruzione**: non esiste più (`include/cpu.h:103-118`, `src/cpu/cpu_execution.c:143-149`). Il "+50-100%" della tabella §4 non è più disponibile.
- **§1.1, gate del trace ring**: non fatto. Il ring è ancora scritto a ogni istruzione (`src/cpu/cpu_execution.c:128-134`), vedi F6a.
- **§1.1, controlli dei breakpoint**: sostituiti da un filtro a bit (`include/debugger.h:20-62`), ma restano una chiamata per istruzione e una per accesso (F6c, F8).
- **§1.2, upload completo della VRAM**: ancora presente, ora a `src/main.c:921-924` (il documento citava `main.c:482`).
  - La conversione RGBA8 per il viewer è stata spostata in una passata GPU (`src/main.c:927-932`): quella parte è chiusa.
  - Il punto vero oggi non è il dirty-rect: con la texture barrier il mirror R16UI non serve proprio (F2).
  - Su Vulkan l'upload è già un no-op (`src/gpu/vk/renderer_vk.c:927-949`).
- **§1, "VSync disabled (SetSwapInterval(0))"**: nel tree attuale non c'è alcuna chiamata a `SDL_GL_SetSwapInterval`; l'intervallo è quello del driver (F1).
- **Riferimenti a `renderer.c`** (`:650-687`, `:1635`, `:1771`): non più validi. `src/gpu/renderer.c` è un dispatcher di 271 righe; il codice GL è in `src/gpu/renderer_gl.c` (thread GPU a `:2067-2257`).
- **§3.2, SPU in AVX2**: prima va letto il bucket `spu` (F16); la stima qui è 0.05-0.3 ms/campo.
- **§4, JIT**: fuori perimetro. La sezione 1 mette l'interprete a 35-45 cicli host per istruzione guest sul i9: per le macchine lente F6 e F7 sono la leva più economica, prima di un JIT.

### `docs/MDEC_OFFLOAD_DESIGN_2026-08-01.md`

- I numeri di riga sono scivolati:
  - `mdec_get_status` 53 -> 60;
  - `mdec_idct` 165 -> 182;
  - `mdec_yuv_to_rgb` 192 -> 209;
  - `mdec_copy_out_block` 230 -> 247;
  - `mdec_decode_macroblock` 304 -> 322;
  - `mdec_execute` 389 -> 412;
  - `mdec_dma_in` 524 -> 547;
  - `mdec_dma_out` 534 -> 557;
  - le slice DMA MDEC in `src/core/bus.c` da 739-818 a 927-1010;
  - l'avvio del thread GPU non è più `renderer.c:1771` ma `src/gpu/renderer_gl.c:2263-2295`;
  - il lettore CD asincrono è a `src/cdrom/cdrom_disc.c:638-727` e usa pthread, non i thread SDL.
- La premessa ("FMV is the largest single work spike") non è misurata nel repository. F14 rende il kernel 3-5x più economico senza toccare protocollo, savestate o timing: va fatto e misurato prima di costruire il pool.
- Resta vero che l'MDEC non ha costo emulato per macroblocco (§5.2 del documento): `src/core/mdec.c` non addebita cicli.

---

## 6. Note di accuratezza emerse dall'analisi (fuori perimetro)

Non sono problemi di prestazioni, ma condizionano le ottimizzazioni sugli stessi percorsi.

- **N1. Lo "stallo" dei DMA tramite `downcount` non ha effetto; ne esiste uno accidentale.**
  - `src/core/bus.c:902`, `:1007`, `:1226` sottraggono cicli a `cpu->downcount` senza avanzare `cpu_cycle_counter`. Il dispatch confronta i target con `cpu_cycle_counter` (`src/core/event_scheduler.c:89-111`) e il downcount viene ricalcolato da lì (`src/cpu/cpu_execution.c:221-228`): quel costo si perde (CERTO).
  - Intanto ogni parola che un DMA legge dalla RAM passa per `interconnect_load32`, che addebita 3 cicli di stallo alla CPU (`src/core/bus.c:550-552`) sull'istruzione successiva (`src/cpu/cpu_execution.c:214-216`). Il costo DMA effettivo oggi è quindi quello accidentale.
  - Conseguenza pratica: sostituire gli accessori nei loop DMA con letture dirette (l'ovvia ottimizzazione di F8) **cambia il CPI e i tempi emulati**. Il modello di stallo va deciso esplicitamente, con citazione `DOCS/`, e verificato con i milestone di boot.
- **N2. Il side-channel del BIOS non è privo di effetti.** `src/cpu/cpu_bios.c:395-399` e `:492-499` leggono stringhe guest con `interconnect_load8`, che addebita stalli e passa per i watchpoint in lettura (`src/core/bus.c:638-641`). È un canale diagnostico che sposta il tempo emulato.
- **N3. Il readback sincrono Vulkan non esegue le op pendenti.** `src/gpu/vk/renderer_vk.c:1310-1318` e `:1578-1597` copiano l'immagine senza rigiocare lo slot di scrittura, mentre GL lo fa (`src/gpu/renderer_gl.c:2018-2028`). Su Vulkan, GP0(C0) e GP0(80) leggono quindi la VRAM senza le primitive del campo corrente.
- **N4. Gli stati salvati perdono i pixel rasterizzati.** `src/core/savestate.c` salva `gpu.vram.data` senza alcun readback; l'unica chiamata al renderer è l'upload al load (`:411-416`). Tutto ciò che il rasterizzatore aveva disegnato, e che il gioco non ridisegna, sparisce dopo un load. Da verificare con un titolo che disegna una schermata una volta sola.
- **N5. SWL/SWR fanno una lettura vera.** `src/cpu/cpu_instructions.c:907` e `:934` leggono la parola con `interconnect_load32` per il read-modify-write. In RAM addebitano uno stallo da load (3 cicli, `src/core/bus.c:550-552`); su un registro I/O eseguono una lettura con effetti collaterali.
- **N6. Tempi CD dipendenti dall'host.** Vedi F13: il percorso PENDING rende il tempo emulato funzione della velocità dell'I/O.