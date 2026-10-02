> **Allegato C** a `docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md`. Prodotto da un'analisi statica
> parallela il 2026-10-02 sul codice a `bbc1c7e` e su psx-spx al commit `00d5dcb`. Le voci riprese nel
> documento principale sono state riverificate riga per riga; le altre portano doc e codice citati per
> il controllo, ma non sono state ricontrollate una per una.
>
> Due correzioni emerse dalla verifica:
> - la riga "Costo DMA 1 clk/word più row-load: CONFORME" del §3.4 vale per il ritmo con cui le slice
>   DMA vengono pianificate. La riduzione di `downcount` che dovrebbe rappresentare lo stallo della CPU
>   si perde al dispatch successivo: vedi la sezione 5.1 del documento principale;
> - V4/R4 (clip a 9 bit): preso alla lettera, il testo della doc fa avvolgere a nero una somma `Y+R`
>   oltre 255. Non c'è una cattura da hardware che lo confermi; il documento principale propone di
>   applicare prima solo la parte sicura.

# Audit FMV: pipeline CD -> MDEC -> DMA -> GPU -> schermo, contro psx-spx

Data: 2026-10-02. Codice: `stable_branch` a `bbc1c7e`. Documentazione: clone psx-spx al commit
`00d5dcb`, citata come `ps1/<percorso>:<riga>`.

Le citazioni `DOCS/...` nei commenti del codice e negli audit precedenti puntano al vecchio layout
piatto: stesso testo, numeri di riga diversi. Per esempio `mdec.c:12-13` cita rl_decode_block a
`:138-158` e real_idct_core a `:192-212`; nel clone attuale sono
`ps1/cpu/mdec/macroblockdecodermdec.md:146-166` e `:200-219`. Il testo MDEC attuale contiene due
paragrafi assenti nel vecchio layout, e cambiano due verdetti dell'audit del 2026-08-17:
- il reset **non** azzera le tabelle (`:114-117`);
- il clip a 9 bit si applica dopo la somma in yuv_to_rgb (`:235-238`).

**Regola** (come `docs/CDROM_AUDIT_2026-08-17.md`): nessuna riga è CONFORME senza una riga di doc e
una di codice. Quello che la doc non decide è NON VERIFICATO.

**Legenda:**
- **CONFORME**: confrontato su entrambi i lati, coincide;
- **DIFFORME**: differenza dimostrata;
- **NON VERIFICATO**: la doc tace o è dichiaratamente approssimata, o manca una misura.

**Banco di prova.** Il vero `src/core/mdec.c` è stato compilato (incluso tale e quale, con stub per
`log_print` e `lua_debug_notify`) in un programma di prova fuori dal repository, non incluso qui. I
risultati sono nel §4 e sono citati come "banco [n]".

Abbreviazioni nelle tabelle:
- `mdec.md` = `ps1/cpu/mdec/macroblockdecodermdec.md`
- `dma.md` = `ps1/system/dmachannels.md`
- `gp1.md` = `ps1/gpu/display-control-commands-gp1.md`

---

## 1. Sintesi

Il decoder è fedele alla doc nei punti che contano per l'immagine:
- ordine dei blocchi, RLE, zigzag;
- IDCT a due passate, errore massimo 1 livello contro un IDCT float (banco [6]);
- coefficienti YUV, impacchettamento 15/24 bpp.

Lo scanout 24 bpp, lasciato UNVERIFIED dallo studio del 2026-08-18, è conforme. I difetti che possono
produrre artefatti visibili durante un FMV:

1. **Reset MDEC azzera quant e scale table** (`mdec.c:530` -> `mdec_init` -> `memset` a `mdec.c:509`),
   contro `mdec.md:116-117`. Un titolo che resetta senza ricaricare decodifica grigio uniforme.
   Banco [2].
2. **MDEC(0) e MDEC(4..7) consumano parametri** (`mdec.c:442-446`, `:490-498`), contro
   `mdec.md:119-126`. Due halfword di padding `FE00h` arrivate dopo la fine di un MDEC(1) (padding a
   blocchi DMA, `mdec.md:77-79`) diventano un MDEC(7) da 65024 word e **inghiottono il frame
   successivo**. Banco [3b].
3. **GP0(02) Fill raggiunge la texture VRAM solo come quad rasterizzato** (`gpu_commands.c:347`):
   tagliato dalla drawing area, soggetto alla maschera di GP0(E6), senza wrap. Contro
   `ps1/gpu/i-o-ports-dma-channels-commands-vram.md:86-91`. Effetto: bande di letterbox o buffer 24 bpp
   non pulite a schermo pur essendo pulite lato CPU.
4. **Clip a 9 bit sui blocchi Cr/Cb/Y prima della conversione, e mancante dopo la somma**
   (`mdec.c:198-200`, `:222-224`), contro `mdec.md:235-238`. Differenze su 0,2-5% dei pixel, fino a
   12-23 livelli ai bordi delle zone sature. Banco [8].
5. **Ogni word letta da RAM dal DMA paga anche lo stallo di load della CPU**: 3 cicli (`bus.c:579`,
   `:550-555`), oltre al costo DMA documentato (`dma.md:205-207`, `:225-229`). Sono circa 5 ms di CPU
   emulata per ogni frame 320x240 a 24 bpp caricato in VRAM. Non corrompe l'immagine ma toglie budget
   al decoder VLC software: scatti, frame saltati.

Il resto è classificato nel §5. Gli stati delle voci aperte negli audit precedenti sono nel §2.

---

## 2. Stato delle voci aperte negli audit precedenti

### 2.1 `docs/DMA_IRQ_GTE_MDEC_AUDIT_2026-08-17.md` (Parte 3 e voci DIVERGES/UNVERIFIED della 2.5)

| # | Voce | Stato oggi |
|---|---|---|
| 1 | Scritture sub-word su registri DMA/IRQ | risolto in `bus.c:283-297` (DMA) e `bus.c:231-240` (IRQ) |
| 2 | Master flag DICR con gli enable per canale | risolto in `dma.c:112-113` |
| 3 | Il completamento DMA azzera I_STAT.3 | risolto in `dma.c:124-130` |
| 4 | DICR.15 (bus error) mai alzato | risolto in `dma.c:138-143`; chiamato da `bus.c:811`, `:823`, `:841`, `:859`, `:939`, `:964`, `:1126`, `:1170` |
| 5 | MVMVA non azzera FLAG | risolto in `gte_ops.c:146` |
| 6 | LZCR indefinito per LZCS=FFFFFFFFh | risolto in `gte.c:61-67` |
| 7 | Matrice spazzatura MVMVA con RT21 | risolto in `gte_ops.c:151-161` |
| 8 | SWC2 non stalla | ancora aperto (`cpu_instructions.c:993-1004`) |
| 9 | MDEC(0)/(4..7) consumano parametri | ancora aperto (`mdec.c:442-446`, `:490-498`), più grave del previsto (§5 R2) |
| 10 | DMA GPU linked-list a ~15.6 clk/word | risolto in `bus.c:888-911` (`dma_ram_ticks`); vedi però R5 sullo stallo CPU aggiuntivo |
| 11 | MADR/BCR mai riscritti; restrizioni DMA6; mirror CHCR | ancora aperti (`dma.c:241-247`; `dma.c:49-66`; `dma.c:271-275`) |
| 12 | DICR bit 0-6 ignorati | ancora aperto (`dma.c:283`) |
| 13 | Clip a 9 bit anche su Cr/Cb | ancora aperto (`mdec.c:198-200`); con il testo attuale (`mdec.md:235-238`) è DIFFORME netto |
| 14 | Interrupt su opcode cop2 differito | ancora aperto (`cpu_execution.c:44`) |
| 15 | Lettura manuale di 1F801820h in ordine DMA; DREQ1 | ancora aperti (`mdec.c:520-521`, `:225`; `mdec.c:67`) |
| 16a | CHCR bit 28 azzerato al completamento | ancora aperto (`dma.c:80-83`) |
| 16b | CHCR bit 29-30 non memorizzati | ancora aperto (`dma.c:28`, `:65`) |
| 16c | Priorità DPCR | ancora aperto (`bus.c:299-317`) |
| 16d | ORGB scrivibile | risolto in `gte.c:86-87` |
| 16e | "Words remaining" 0 invece di FFFFh | ancora aperto (`mdec.c:74-75`); banco [3a] |
| 16f | 1F8010F8h/FCh | ancora aperto (`dma.c:224-226`) |
| 2.5 | Soglia DREQ0 di 64 halfword | ancora NON VERIFICATO (`mdec.c:66`) |
| 2.5 | Output FIFO svuotata a ogni nuovo comando | ancora NON VERIFICATO (`mdec.c:426`) |
| 2.5 | Scritture MDEC a 8/16 bit scartate | ancora aperto (`bus.c:389-393`) |
| 2.5 | Letture MDEC a 8/16 bit "PLAUSIBLE" | **riclassificato DIFFORME**: la metà alta (1F801822h/1F801826h) torna 0 (§3.2) |

### 2.2 `docs/GPU_DISPLAY_STUDY_2026-08-10.md`

| Voce | Stato oggi |
|---|---|
| Larghezza dalla GP1(08) | risolto in `gpu.c:91-100` (divisori in `gpu.c:41-49`) |
| Altezza e raddoppio | risolto in `gpu.c:104-130` (raddoppio solo con interlace e vres=480, `gpu.c:129`) |
| GP1(06)/(07) senza effetto | parziale: la dimensione segue (`gpu.c:350-362`); la posizione X1/Y1 no, quindi lo screen-shake resta invisibile |
| GP1(05) X mascherato a pari | risolto in `gpu.c:344` |
| Modo 368 | risolto in `gpu.c:42`, `:377-378` |
| Clamp intervalli | risolto in `gpu.c:95`, `:107` |
| §2.1 striscia in alto | ancora aperto. Il crop overscan ora vale solo NTSC (`gpu.c:74-81`), quindi sui PAL la striscia è di nuovo visibile; il CHANGELOG lo registra come "Measured, not resolved" |
| §2.2 fotogramma stirato (latch) | ancora aperto. Il latch a inizio campo è stato provato e spento (`renderer_gl.c:2334-2371`, `ZS1_DISPLAY_LATCH=1` per A/B); il latch per riga non esiste |
| §2.3 schermi neri lunghi | strumentazione presente (`gpu.c:56-61`, `:246-251`, livello DEBUG). Il CHANGELOG riporta l'FMV prima saltato ora visibile (`CHANGELOG.md:427-428`, dopo i rifiuti in seek a `cdrom_commands.c:299`, `:453`); il conteggio per campo non è pubblicato |
| §3 CRTC una volta per frame | ancora aperto (`gpu.c:245-288`) |
| §3 nessuna selezione di campo interlacciato | ancora aperto (`renderer_gl.c:927-929`) |
| §3 nessun wrap orizzontale nello scanout | ancora aperto (`renderer_gl.c:933-935`, `scanout.frag:38-40`) |
| §3 ordine bit hres in GPUSTAT | risolto in `gpu.c:505-506` |
| §3 GP0(C0)/(80) sullo specchio CPU | risolto (readback in `gpu_commands.c:1254`, `:1399`) |

### 2.3 `docs/GPU_CPU_VRAM_PATH_STUDY_2026-08-18.md`

| Voce | Stato oggi |
|---|---|
| Aritmetica scanout 24 bpp | ora **CONFORME** (§3.6) |
| Disegno mascherato a righe in interlace | ancora aperto (fuori perimetro) |
| DMA VRAM solo in vblank | ancora aperto (`bus.c:1117-1133`); permissivo |
| Latch CRTC per riga | ancora aperto |
| Offset di 8 righe (Monsters & Co.) | ancora aperto, NON VERIFICATO (§6) |
| Divergenze con redux | ancora NON VERIFICATO |

### 2.4 `docs/MDEC_OFFLOAD_DESIGN_2026-08-01.md`

| Voce | Stato oggi |
|---|---|
| Offload su thread | proposta non implementata (decode sincrono, `mdec.c:412-502`) |
| Kick ch0/ch1 scartato durante uno slice | ancora aperto (`bus.c:1058-1066`) |
| Nessun costo di decodifica per macroblocco | ancora aperto; NON VERIFICATO (`dma.md:214` "still unknown") |

### 2.5 `CLAUDE.md`

| Voce | Stato oggi |
|---|---|
| "Display window is computed from the wrong register" | risolto in codice (`gpu.c:91-100`); la voce è obsoleta |
| "Display state is snapshotted at the end of the field" | ancora così: stato campionato al submit (`renderer_gl.c:2400-2405`) al confine di VBlank (`system.c:63-70`, `main.c:935`) |
| E.9 larghezza da GP1(06) | risolto |
| E.10 latch a inizio campo | provato e spento |
| E.11 conteggio GP1(03) | strumentazione presente, risultato non registrato |
| E.12 crop overscan | presente, solo NTSC (`gpu.c:63-81`, `:133-140`) |
| G.20 MDEC comandi 0/4-7 e clip crominanza | entrambi aperti |

### 2.6 Voci di `docs/CDROM_AUDIT_2026-08-17.md` che toccano lo streaming STR

| Voce | Stato oggi |
|---|---|
| C3b: bit 4 di Setmode letto come "2328 byte" | ancora aperto (`cdrom_commands.c:956-958`) |
| C9c: settore ADPCM filtrato consegnato come dato | ancora aperto (`cdrom_commands.c:875`, `:934`) |
| C17: read dopo Pause riparte dal settore successivo | ancora aperto (`cdrom_commands.c:69-76`, `:996`) |
| B1: il ring consegna il settore più nuovo | ancora aperto (`cdrom_commands.c:965`) |
| R5: lettura oltre la fine torna 0 | ancora aperto (`cdrom.c:309-310`, `:502`) |

---

## 3. Tabelle di conformità

### 3.1 CDROM (solo ciò che influisce su quanto arriva al MDEC)

| Aspetto | Riga doc | Riga codice | Esito | Impatto in FMV |
|---|---|---|---|---|
| Bit 5: 924h dall'offset 12 | `ps1/cdr/cdromdrive.md:519` "1=924h=WholeSectorExceptSyncBytes" | `cdrom_commands.c:953-955` | CONFORME | nessuno |
| Default 800h | `cdromdrive.md:519` | `cdrom_commands.c:959-961` | CONFORME | nessuno |
| FMV FORM2 da 914h (FF9) | `ps1/cdr/cdromfileformats/streaming.md:576-580` | percorso bit 5 | CONFORME | nessuno |
| Bit 4 "Ignore Bit" | `cdromdrive.md:520`, `:526-533` "the size is kept from the most recent Setmode command which didn't have Bit4 set" | `cdrom_commands.c:956-958` | DIFFORME | dati STR spostati di 12 byte, macroblocchi spazzatura; raro |
| ReadS = ReadN | `cdromdrive.md:749-756` | `cdrom_commands.c:264-270` | CONFORME (nessun errore di lettura emulato) | nessuno |
| Audio+realtime filtrato non consegnato come dato | `cdromdrive.md:604` | `cdrom_commands.c:875`, `:934` | DIFFORME | INT1 in più; frame persi in STR multicanale |
| Read dopo Pause riconsegna l'ultimo settore | `cdromdrive.md:812-814` | `cdrom_commands.c:69-76` (riparte da `current_lba`, già incrementato a `:996`) | DIFFORME | settore perso in streaming con pausa: frame saltato o con un chunk mancante |
| INT1 sul più vecchio, poi salto al più nuovo | `cdromdrive.md:1939-1946` | `cdrom_commands.c:965` | DIFFORME | l'emulatore perde meno settori del reale |
| Padding oltre la fine | `cdromdrive.md:122-124` | `cdrom.c:309-310`, `:502` | DIFFORME | trascurabile |

### 3.2 MDEC: registri, comandi, stato

| Aspetto | Riga doc | Riga codice | Esito | Impatto in FMV |
|---|---|---|---|---|
| Porte 1F801820h/24h | `mdec.md:13-23`, `:31`, `:56` | `mdec.c:514-545`, `bus.c:112` | CONFORME | nessuno |
| Bit 31: FIFO uscita vuota | `mdec.md:33` | `mdec.c:62` | CONFORME | nessuno |
| Bit 30: piena "or Last word received" | `mdec.md:34` | `mdec.c:63` | DIFFORME (minore) | nessuno con DMA |
| Bit 29: busy | `mdec.md:35` | `mdec.c:64-65` | CONFORME | nessuno |
| Bit 28: DREQ0 | `mdec.md:36`, `:59` | `mdec.c:66` | CONFORME nella forma, soglia NON VERIFICATO | nessuno |
| Bit 27: DREQ1 che si azzera dopo le prime word | `mdec.md:64-67` | `mdec.c:67` | DIFFORME (minore) | nessuno con DMA |
| Bit 26-23 copiati dal comando | `mdec.md:38-40`, `:99`, `:109`, `:120-121` | `mdec.c:70-72`, `:422-424` | CONFORME | nessuno |
| Bit 18-16, lato ingresso | `mdec.md:42`, `:48-50` | `mdec.c:73` | CONFORME | nessuno |
| Bit 18-16 con uscita in coda: Y1..Y4 | `mdec.md:45-47` | `mdec.c:73`, `:344` (riporta 4=Cr) | DIFFORME (minore) | nessuno con DMA |
| Bit 15-0 "minus 1 (FFFFh=None)" | `mdec.md:43` | `mdec.c:74-75`; banco [3a] | DIFFORME | blocco di un player che aspetta FFFFh |
| Reset: stato 80040000h | `mdec.md:58` | `mdec.c:529-533`, `:509`; banco [2] | CONFORME | nessuno |
| Reset non azzera le tabelle | `mdec.md:116-117` | `mdec.c:530` -> `memset` `:509`; banco [2] | DIFFORME | **frame grigi** |
| Bit 30/29 nella stessa scrittura del reset | `mdec.md:58-60` | `mdec.c:532` (return prima di `:534-535`) | NON VERIFICATO | nessuno con DMA |
| DMA solo con richieste abilitate | `mdec.md:63-64` | `bus.c:935`, `:958` | DIFFORME (permissivo) | nessuno |
| Lettura manuale a blocchi 8x8 | `mdec.md:25-29` | `mdec.c:520-521`, `:225` | DIFFORME | solo per decoder via CPU |
| Lettura a FIFO vuota "Garbage" | `mdec.md:23` | `mdec.c:558-561` | CONFORME | nessuno |
| Letture a 8/16 bit | `ps1/system/unpredictablethings.md:58` "I/O ports can be read in 8bit, 16bit, or 32bit units" | `bus.c:383-387` passa l'indirizzo non allineato; `mdec.c:515`, `:520`, `:523` confrontano solo ...20/...24, quindi 1F801822h/26h tornano 0 | DIFFORME | `lhu` sulla metà alta legge busy=0 |
| Scritture a 8/16 bit | `unpredictablethings.md:34-35` ("?") | `bus.c:389-393` | NON VERIFICATO | ignoto |
| MDEC(1) | `mdec.md:86-91` | `mdec.c:421-424`, `:430-433` | CONFORME | nessuno |
| MDEC(2) | `mdec.md:98-104`, `:259-268` | `mdec.c:361-379`, `:435` | CONFORME | nessuno |
| MDEC(3) | `mdec.md:108-113` | `mdec.c:381-405`, `:439` | CONFORME | nessuno |
| MDEC(0)/(4..7) senza parametri | `mdec.md:119-126` | `mdec.c:442-446`, `:490-498`; banco [3a], [3b] | DIFFORME | FMV fermo o nero |
| FIFO di uscita svuotata da nuovo comando | nessuna | `mdec.c:426` | NON VERIFICATO | ultima colonna vecchia se capita |
| Dimensioni FIFO | nessuna | `mdec.h:37-38` | NON VERIFICATO | nessuno osservato |

### 3.3 MDEC: decodifica

| Aspetto | Riga doc | Riga codice | Esito | Impatto in FMV |
|---|---|---|---|---|
| Ordine Cr, Cb, Y1..Y4 e quadranti | `mdec.md:133-138` | `mdec.c:337-341`, `:348-351` | CONFORME | nessuno |
| Tabella iq per blocco | `mdec.md:102-104`, `:133-138` | `mdec.c:338` | CONFORME | nessuno |
| Mono | `mdec.md:143` | `mdec.c:323-334` | CONFORME | nessuno |
| Azzeramento del blocco | `mdec.md:148` | `mdec.c:116-117` | CONFORME | nessuno |
| FE00h a inizio blocco ignorato | `mdec.md:151`, `:430-433` | `mdec.c:119-127` | CONFORME | nessuno |
| q_scale | `mdec.md:152`, `:405` | `mdec.c:130` | CONFORME | nessuno |
| DC senza q_scale | `mdec.md:153` | `mdec.c:131` | CONFORME | nessuno |
| Modo q_scale=0 | `mdec.md:155`, `:159` | `mdec.c:132-133`, `:137-138`, `:150-151`, `:155-156` | CONFORME | nessuno |
| Saturazione a -400h..3FFh | `mdec.md:156` | `mdec.c:134`, `:152` | CONFORME | nessuno |
| Avanzamento k | `mdec.md:161`, `:412` | `mdec.c:145` | CONFORME | nessuno |
| `(...*qt[k]*q_scale+4)/8` | `mdec.md:162` | `mdec.c:147-149` | CONFORME nella forma. La "/" è NON VERIFICATO: il C tronca i negativi mentre lo stesso file usa il floor nell'IDCT (`mdec.c:178-180`, `:192`) | banco [5]: 6,9% dei pixel a +-1, max 3; invisibile |
| Fine blocco a k=63 | `mdec.md:163`, `:423-425` | `mdec.c:159-162` | CONFORME | nessuno |
| zagzig | `mdec.md:279-289`, `:300-303` | `mdec.c:108-113`; banco [1] | CONFORME | nessuno |
| Struttura real_idct_core | `mdec.md:206-218` | `mdec.c:186-196`, `:381-405` | CONFORME | nessuno |
| 13 bit alti della scale table | `mdec.md:317-318` | `mdec.c:191` (`/8`) | DIFFORME (minimo) | banco [7]: invisibile |
| `(sum+0FFFh)/2000h` | `mdec.md:214`, `:220-222` | `mdec.c:192` | CONFORME alla doc; NON VERIFICATO sull'hardware (`:223-226`) | banco [6]: max 1 livello |
| Clip dopo l'IDCT su tutti i blocchi | `mdec.md:235-238` (colore), `:250-251` (mono) | `mdec.c:198-200` | DIFFORME colore, CONFORME mono | luci sature più spente ai bordi |
| Sottocampionamento crominanza | `mdec.md:232` | `mdec.c:216-217` | CONFORME | nessuno |
| Coefficienti | `mdec.md:233` | `mdec.c:218-220` | CONFORME; virgola fissa NON VERIFICATO (`:244`) | nessuno noto |
| `(Y+R) AND 1FFh` | `mdec.md:235` | `mdec.c:222-224` | DIFFORME | conta solo con ringing forte |
| Saturazione | `mdec.md:236-238` | `mdec.c:222-224` | CONFORME | nessuno |
| xor 808080h (unsigned) | `mdec.md:239` | `mdec.c:213`, `:222-224` | CONFORME | nessuno |
| Signed | `mdec.md:39`, `:88`, `:239` | `mdec.c:225-228`, `:238-239`; banco [4] | DIFFORME | colori sbagliati; raro |
| y_to_mono | `mdec.md:248-254` | `mdec.c:235-241` | CONFORME | nessuno |
| Pacchetto 24 bpp | `ps1/gpu/video-memory-vram.md:25-31`, `mdec.md:240` | `mdec.c:268-289` | CONFORME | nessuno |
| Pacchetto 15 bpp | `video-memory-vram.md:20-24` | `mdec.c:291-308` | CONFORME | nessuno |
| Conversione 8->5 bit | nessuna | `mdec.c:295-297` (tronca) | NON VERIFICATO | possibile mezzo LSB più scuro |
| Bit 15 (STP) | `mdec.md:40`, `:89` | `mdec.c:292` | CONFORME | nessuno |
| Pacchetto 4/8 bpp | `mdec.md:362-366`, `video-memory-vram.md:45-52` | `mdec.c:249-266` | NON VERIFICATO | nessuno noto |
| Riordino 16x16 via DMA1 | `mdec.md:25-29` | `mdec.c:225`, `bus.c:958-972` | CONFORME nell'effetto | nessuno |

### 3.4 DMA0 / DMA1 / DMA2

| Aspetto | Riga doc | Riga codice | Esito | Impatto in FMV |
|---|---|---|---|---|
| MDEC in/out in SyncMode 1 | `dma.md:170-171`, `:43-47` | `bus.c:1096-1158` | CONFORME | nessuno |
| DMA1 solo con dati pronti | `mdec.md:37`, `:60` | `bus.c:958` | CONFORME | nessuno |
| DMA0 solo con spazio | `mdec.md:36`, `:59` | `bus.c:935` | CONFORME | nessuno |
| Costo DMA 1 clk/word più row-load (ritmo delle slice; vedi la nota in testa) | `dma.md:205-207`, `:225-229` | `bus.c:779`, `:1006`, `:901` | CONFORME | nessuno |
| **Stallo CPU per ogni word letta dal DMA** | `dma.md:205-207`, `:225-229` | `bus.c:943`, `:1129`, `:815`, `:827`, `:863` -> `bus.c:579`, `:550-555`; consumato in `cpu_execution.c:214-217` | DIFFORME | ~5 ms per frame 24 bpp; scatti |
| GPU block: dati letti al kick | `ps1/gpu/i-o-ports-dma-channels-commands-vram.md:30` | `bus.c:1117-1131` | CONFORME | nessuno |
| GPU block: completamento e costo | `dma.md:207` | `bus.c:1132` | DIFFORME (tempi) | nessuno sull'immagine |
| DMA2 richiede GP1(04) | `i-o-ports-dma-channels-commands-vram.md:29`, `ps1/gpu/status-register.md:27-31` | `bus.c:1117` | DIFFORME (permissivo) | nessuno |
| DMA VRAM solo in V-Blank | `ps1/gpu/memory-transfer-commands.md:88-93` | `bus.c:1117-1133` | DIFFORME (permissivo) | nessuno |
| Abort con CHCR start=0 | `dma.md:79`, `:91-92` | `dma.c:262`, `:36-45` | CONFORME | nessuno |
| Kick ch0/ch1 durante uno slice | `dma.md:79` | `bus.c:1058-1066` | NON VERIFICATO | colonne vecchie se capita |
| MADR aggiornato, BA decrementato | `dma.md:27-29`, `:56-58` | `dma.c:241-247`; cursori a `bus.c:1143-1155` | DIFFORME | colonne duplicate |
| Attesa del Master Enable | `dma.md:121-122` | `bus.c:299-317` | CONFORME | nessuno |
| Flag di completamento e IRQ3 | `dma.md:141-153` | `bus.c:914-922`, `dma.c:103-131` | CONFORME | nessuno |
| Bus error | `dma.md:135` | `dma.c:138-143` | CONFORME | nessuno |
| Tempo di decodifica MDEC | `dma.md:214` | nessun costo | NON VERIFICATO | nessuno noto |

### 3.5 GPU: upload GP0(A0), maschera, fill

| Aspetto | Riga doc | Riga codice | Esito | Impatto in FMV |
|---|---|---|---|---|
| A0..BF tutti CPU->VRAM | `memory-transfer-commands.md:3-5`; `i-o-ports-dma-channels-commands-vram.md:44-51` | `gpu_commands.c:1567-1568` | DIFFORME | frame interpretato come comandi; raro |
| Mascheratura coordinate e dimensioni | `memory-transfer-commands.md:64-69` | `gpu_commands.c:1289-1294` | CONFORME | nessuno |
| Coordinate assolute | `memory-transfer-commands.md:85-87` | `gpu_commands.c:1610-1613` | CONFORME | nessuno |
| Padding halfword dispari | `memory-transfer-commands.md:23-24` | `gpu_commands.c:1317-1318` | CONFORME | nessuno |
| Wrap ai bordi | `memory-transfer-commands.md:95-98` | `gpu_commands.c:1612`, `:1619`; `renderer_gl.c:1226-1230`; `renderer_vk.c:897-899` (lettura oltre `vram.data` se y+h>512) | DIFFORME | rettangoli a cavallo dei bordi persi lato GPU; raro |
| E6.0 forza il bit 15 negli upload | `ps1/gpu/rendering-attributes.md:162-169` | `gpu_commands.c:132-133` | CONFORME | nessuno |
| E6.1 protegge negli upload | `rendering-attributes.md:165-169` | `gpu_commands.c:134-137` (specchio CPU), `:1635-1637` (upload intero) | DIFFORME | overlay cancellati; raro |
| Flush prima dell'upload | `i-o-ports-dma-channels-commands-vram.md:55-58` | `renderer_gl.c:1280`, `renderer_vk.c:960` | CONFORME | nessuno |
| Bit 15 dei pixel rasterizzati | `rendering-attributes.md:162-164` | `renderer_gl.c:341`, `ps1.frag:60` | CONFORME | nessuno |
| Fill: bit 15 a 0, ignora E6 | `i-o-ports-dma-channels-commands-vram.md:86-91` | CPU ok (`gpu_commands.c:360-367`); GPU via `draw_rectangle` (`:347`) | DIFFORME | verde in 24 bpp con E6.0=1 |
| Fill: non tagliato, con wrap | `memory-transfer-commands.md:38-47`, `:95-98` | scissor della drawing area (`renderer_gl.c:1344-1346`, `:1508`); `gpu_commands.c:145-150` aggiorna solo lo specchio R16UI | DIFFORME | letterbox sporco |
| Tetto di 1024 upload per campo | nessuna | `renderer_gl.c:1211-1214` (non contati in `emu.gpu_pool`) | NON VERIFICATO | blocchi vecchi se superato |

### 3.6 GPU: display e scanout

| Aspetto | Riga doc | Riga codice | Esito | Impatto in FMV |
|---|---|---|---|---|
| GP1(03) off = nero | `gp1.md:44-47` | `gpu.c:321-328`, `renderer_gl.c:2137-2139` | CONFORME | nessuno |
| GP1(05) X/Y | `gp1.md:75-77` | `gpu.c:344-345` | CONFORME | nessuno |
| Larghezza da GP1(06) | `gp1.md:98-100` | `gpu.c:91-100` | CONFORME | nessuno |
| Divisori | `ps1/gpu/timings.md:86-90` | `gpu.c:41-49` | CONFORME | nessuno |
| Clock della regione per il clamp di X2 | `timings.md:8-12`, `gp1.md:108-110` | `gpu.c:92` (usa GP1(08).3) | DIFFORME (minimo) | nessuno |
| Altezza Y2-Y1 | `gp1.md:134-136` | `gpu.c:104-108` | CONFORME | nessuno |
| Raddoppio solo a 480 righe | `gp1.md:152`, `status-register.md:21`, `:38-39` | `gpu.c:129` | CONFORME | nessuno |
| Modo 368 | `gp1.md:156` | `gpu.c:42`, `:377-378` | CONFORME | nessuno |
| Bit 24 bpp | `gp1.md:154` | `gpu.c:369`, `:156` | CONFORME | nessuno |
| Spacchettamento 24 bpp | `video-memory-vram.md:25-31` | `renderer_gl.c:930-937`; `scanout.frag:35-42` | CONFORME | nessuno |
| Bit 15 come dato in 24 bpp | `video-memory-vram.md:25-28` | `renderer_gl.c:919-925`, `:1991` | CONFORME | nessuno |
| X dispari in 24 bpp | `gp1.md:75` | `renderer_gl.c:933` | CONFORME | nessuno |
| Wrap nello scanout | `gp1.md:84-88` (solo Y su 2 MB) | `renderer_gl.c:929`, `:933-935`; `scanout.frag:33`, `:38-40` | NON VERIFICATO | spazzatura a destra se X+1,5W>1024 |
| Posizione target X1,Y1 | `gp1.md:81-83` | `gpu.c:83-159` | DIFFORME di presentazione; la doc consiglia l'auto-centratura agli emulatori (`gp1.md:257-258`) | shake invisibile |
| Crop overscan NTSC | `gp1.md:141-144` | `gpu.c:74-81`, `:133-140` | DIFFORME di presentazione (`ZS1_OVERSCAN=0`) | -16 righe sugli FMV NTSC |
| Momento del campionamento dello stato display | nessuna | `renderer_gl.c:2398-2405`, `renderer_vk.c:1559-1561`, `system.c:63-70`, `main.c:935` | NON VERIFICATO | fotogramma stirato a fine FMV |
| Selezione del campo interlacciato | `status-register.md:38` | `renderer_gl.c:927-929` | NON VERIFICATO | nessuno a 240 righe |
| Reset GP1(00) | `gp1.md:11-23` | `gpu.c:633`, `:646-648` | CONFORME | nessuno |
| GPUSTAT bit 13 e 16-18 | `status-register.md:16`, `:19-20` | `gpu.c:492`, `:505-506` | CONFORME | nessuno |

---

## 4. Misure dal banco di prova

| # | Prova | Risultato |
|---|---|---|
| [1] | zagzig contro `zagzig[zigzag[i]]=i` | 0 discrepanze |
| [2] | Scale e quant caricati, poi 80000000h a 1F801824h | prima: `scale[0]=5a82`, `iq_y[0]=2`. Dopo: `0000`, `0`, `iq_uv[0]=0`; stato `80040000` |
| [3a] | Stato dopo MDEC(3) e MDEC(2) completati; poi MDEC(0) con conteggio 5 | `80040000` (bit 15-0 = 0000 invece di FFFFh). Dopo MDEC(0): NOCOMMAND con 10 halfword attese; la word MDEC(3) seguente viene mangiata |
| [3b] | MDEC(1) esatto, poi due `FE00FE00h`, poi un altro MDEC(1) | `state=5`, `remaining_hw=130046`, stato `a784fdfe`; il secondo frame produce **0 word** (128 attese) |
| [4] | Grigio Y=-60 in 15 bpp signed | word `7ff87ff8`: R=24 corretto, G=31 e B=31 sbagliati |
| [5] | `(v+4)/8` del C contro floor | diverso nel 42% dei casi; dopo l'IDCT 6,9% dei pixel, max 3 livelli |
| [6] | IDCT di `mdec.c` contro IDCT float, 20000 blocchi | errore medio 0,29, max 1 livello; con 4 bit frazionari dopo la passata 0: 0,25, max 1 |
| [7] | `/8` contro `>>3` | 28 voci su 64 diverse; 6% dei pixel a +-1/2 |
| [8] | Percorso del codice contro `mdec.md:232-239`, con rumore da ringing | sigma 4: 0,24% dei pixel, max 12. sigma 8: 1,4%, max 23. sigma 16: 5,5%, max 255. Esempi: Y=140, Cr=-20, Cb=10 -> R 227 contro 240; Y=120, Cr=100 -> R 255 contro 0 |

---

## 5. DIFFORME in ordine di impatto visivo atteso

Criterio: gravità quando il percorso si attiva, per probabilità che un FMV reale lo attivi. Le
pseudo-soluzioni sono scritte dalla doc citata, non da altri emulatori.

### R1. Il reset MDEC azzera le tabelle

- **Doc:** `mdec.md:116-117`; `:58`.
- **Codice:** `mdec.c:529-533` chiama `mdec_init`, che esegue `memset` (`mdec.c:509`). Banco [2].
- **Sintomo:** grigio medio uniforme dopo un reset senza ricarica delle tabelle. Scenario tipico: un
  filmato interrotto o saltato, poi il successivo.
- **Pseudo-soluzione:**
  ```
  mdec_reset(m):  // 1F801824h con bit31
      salva iq_y, iq_uv, scale_table
      azzera comando, FIFO, blocchi; current_block=0; current_coefficient=64
      ripristina le tabelle
  mdec_write(1F801824h, v):
      se v.bit31: mdec_reset(m)
      enable_dma_in = v.bit30; enable_dma_out = v.bit29   // opzionale, la doc non lo dice
  mdec_init resta per l'accensione (tabelle a zero corrette, mdec.md:114-116)
  ```
- **Rischio:** nullo per chi ricarica le tabelle. La struttura non cambia, quindi i savestate restano
  compatibili.
- **Verifica:**
  ```
  emu.on_event(function(n)
    if n == "mdec_macroblock" and emu.mdec_scale(0) == 0 then
      print(string.format("MDEC con scale table nulla, pc=%08x", emu.pc()))
    end
  end)
  ```
  Scene: saltare con Start l'intro di `Ace Combat 2 (Europe)` o di `Crash Bandicoot 3` e lasciare
  partire il filmato successivo; poi i loghi Disney/Pixar di `Monsters & Co. (Italy)`.

### R2. MDEC(0) e MDEC(4..7) consumano parametri, anche dal padding FE00h

- **Doc:** `mdec.md:119-126`; padding a `:77-79`.
- **Codice:** `mdec.c:442-446`, `:490-498`. Banco [3a], [3b].
- **Sintomo:** frame successivi inghiottiti, timeout del decoder, FMV fermo o nero.
- **Pseudo-soluzione:**
  ```
  case 0,4,5,6,7:
      status_low16 = cw AND FFFFh      // senza "-1" (mdec.md:121-123)
      decode_state = IDLE              // nessun parametro
  bit 15-0 dello stato:
      comando 1/2/3 in corso -> word rimanenti - 1
      altrimenti -> status_low16
          0000h dopo reset (mdec.md:58)
          FFFFh a fine MDEC(1/2/3) (mdec.md:43)
          cw AND FFFFh dopo MDEC(0/4..7)
  ```
- **Rischio:** un campo nuovo in `Mdec` cambia la dimensione della sezione di savestate. Alternativa:
  una variabile file-static come `g_mdec_macroblocks_out` (`mdec.c:94`).
- **Verifica:** la riga `[MDEC] Invalid command` (`mdec.c:443`, WARN) compare già al livello INFO
  predefinito: cercarla nei log degli FMV. In Lua, su `"vblank"`, il secondo valore di
  `emu.mdec_info()` sopra 0x10000 indica il caso.

### R3. Il fill GP0(02) arriva allo schermo tagliato e con la maschera

- **Doc:** `i-o-ports-dma-channels-commands-vram.md:86-91`; `memory-transfer-commands.md:38-47`,
  `:95-98`.
- **Codice:** `gpu_commands.c:347` (quad rasterizzato con scissor `renderer_gl.c:1344-1346` e maschera
  del batch); `gpu_commands.c:145-150` aggiorna solo lo specchio R16UI. `scripts/fmv_fill_watch.lua`
  sospettava già il taglio.
- **Sintomo:** in 24 bpp un buffer largo 480 halfword, pulito mentre la drawing area è larga 320,
  resta sporco nel terzo destro: bande vecchie nel letterbox. Con E6.0=1 il nero diventa 8000h, verde
  medio in 24 bpp.
- **Pseudo-soluzione:**
  ```
  gp0_fill_rectangle:
      mascheratura da memory-transfer-commands.md:40-43
      scrittura lato CPU con wrap (già fatta)
      per ogni pezzo spezzato ai bordi 1024/512: renderer_upload_vram_rect(pezzo)
          // fa già il flush: renderer_gl.c:1280, renderer_vk.c:960
      niente draw_rectangle
  ```
- **Rischio:** un fill a schermo intero diventa un upload da 1 MB; l'ordine resta garantito dal flush.
- **Verifica:**
  - `ZS1_LUA_SCRIPT=scripts/fmv_fill_watch.lua` sull'FMV di `Ace Combat 2 (Europe)` (letterbox
    320x160) e sul clear più upload di `Dino Crisis (E)`;
  - poi `ZS1_DUMP_VRAM=1 ZS1_DUMP_FRAME=<file> ZS1_DUMP_FRAME_N=<n>`, confrontando con
    `emu.vram16(x,y)` sulle righe del letterbox.

### R4. Clip a 9 bit nel punto sbagliato di yuv_to_rgb

- **Doc:** `mdec.md:233-238`; per il mono `:250-251`.
- **Codice:** `mdec.c:198-200`, `:222-224`.
- **Sintomo:** banco [8]. Con ringing moderato cambia l'1-1,5% dei pixel, fino a 23 livelli, ai bordi
  delle zone chiare e sature (luci "piatte").
- **Pseudo-soluzione:**
  ```
  idct: nessun clip finale
  y_to_mono: Y = sext9(Y AND 1FFh); clamp(-128,127); +128 se unsigned
  yuv_to_rgb: G = -0.3437*Cb - 0.7143*Cr; R = 1.402*Cr; B = 1.772*Cb
      per ogni canale C: C = sext9((Y+C) AND 1FFh); clamp(-128,127); +128 se unsigned
  ```
- **Rischio:** pochi pixel per frame in ogni FMV a colori. La parte sicura, se si dubita di `:235`, è
  togliere il pre-clip e lasciare la saturazione finale.
- **Verifica:** il banco come test unitario (primo livello di `docs/TESTING_PLAN_2026-08-20.md`); poi
  A/B con `ZS1_DUMP_FRAME` su una scena satura.

### R5. Lo stallo di load CPU si somma al costo di ogni word letta dal DMA

- **Doc:** `dma.md:205-207`, `:225-229`, `:231-238`.
- **Codice:** `bus.c:943`, `:1129`, `:815`, `:827`, `:863` -> `bus.c:579`, `:550-555` (3 cicli),
  consumato in `cpu_execution.c:214-217`. Le scritture non sono colpite: `RAM_STORE_STALL`=0,
  `bus.c:492`.
- **Sintomo:** almeno 172800 cicli (circa 5 ms) per frame 320x240 a 24 bpp; circa il 15% della CPU a
  30 fps. Scatti e frame saltati.
- **Pseudo-soluzione:**
  ```
  lettura DMA = ram_load32(phys AND (RAM_SIZE-1))    // senza bus_charge_cpu_load né watchpoint
  percorsi a slice: solo dma_ram_ticks(words)        // già presente
  percorso GPU block: aggiungere dma_ram_ticks(words)
  ```
  Vedi la sezione 5.1 del documento principale: la riduzione di `downcount` di quei percorsi oggi si
  perde, quindi questa correzione va fatta insieme a un modello esplicito dello stallo CPU.
- **Rischio:** cambia i tempi di ogni gioco; va validato con le pietre miliari di boot in campi
  emulati, come chiede `CLAUDE.md`.
- **Verifica:** `ZS1_FRAME_PROFILE=1` senza log né probe, prima e dopo, sull'FMV di Monsters & Co.;
  poi `scripts/fmv_pacing.lua` in una corsa separata.

### R6. MADR e BCR mai aggiornati

- **Doc:** `dma.md:27-29`, `:56-58`.
- **Codice:** `dma.c:241-247`; cursori a `bus.c:1143-1155`.
- **Sintomo:** colonne duplicate se un player concatena i kick senza riscrivere MADR.
- **Pseudo-soluzione:** a ogni blocco SyncMode 1, MADR = blocco successivo e BA - 1; a fine
  trasferimento MADR = indirizzo finale e BA = 0. In SyncMode 2, MADR = marcatore di fine.
- **Rischio:** basso.
- **Verifica:** `emu.add_write_watch(0x1F801090)` e `emu.add_write_watch(0x1F801098)` con
  `emu.on_break` e `emu.resume()`: contare i kick non preceduti da una scrittura di MADR.
  `emu.mdec_dma()` dà i cursori interni.

### R7. Test di maschera degli upload sullo specchio CPU

- **Doc:** `rendering-attributes.md:165-169`.
- **Codice:** `gpu_commands.c:134-137`, `:1635-1637`.
- **Pseudo-soluzione:** con `preserve_masked_pixels` attivo, `renderer_read_vram_rect` sul rettangolo
  di destinazione prima del primo dato (come già fanno `gpu_commands.c:1254`, `:1399`).
- **Rischio:** un readback sincrono, solo con il test di maschera acceso.
- **Verifica:** su `"gp0_vram_upload"` controllare il bit 12 di `emu.gpustat()`
  (`status-register.md:15`).

### R8. Consegna dei settori STR

- **Doc e codice:** righe C9c, C17 e B1 del §3.1.
- **Sintomo:** frame STR incompleti: saltati, o con macroblocchi spazzatura in basso.
- **Pseudo-soluzione:**
  - rifiutare come dato i settori audio+realtime quando il filtro è attivo (`cdromdrive.md:604`);
  - a lettura in pausa e senza Setloc pendente, ripartire dall'ultimo settore ricevuto (`:812-814`);
  - il ring a due settori (`:1939-1946`) segue l'audit CDROM.
- **Rischio:** medio sul primo punto, basso sul secondo.
- **Verifica:**
  - contare `"cdrom_int1"` per campo con `emu.on_event`, contro la cadenza attesa di 150 settori al
    secondo in 2x (`cdromdrive.md:759-760`) meno i settori audio;
  - le righe `XA stream now file=` (`cdrom_commands.c:912`, INFO) e `XA sequence break` (`:901`, WARN)
    mostrano i cambi di canale.

### R9. Output signed impacchettato male

- **Doc:** `mdec.md:39`, `:88`, `:239`.
- **Codice:** `mdec.c:225-228`, `:238-239`.
- **Pseudo-soluzione:** `(R AND FFh) OR (G AND FFh)<<8 OR (B AND FFh)<<16`; per il mono `Y AND FFh`.
- **Rischio:** nullo.
- **Verifica:** banco [4]. In corsa, `ZS1_LOG_LEVEL=debug` per pochi secondi e cercare `signed=1`
  (`mdec.c:450-451`); quella corsa non va usata per misurare tempi.

### R10. GP0(A1h..BFh) e (C1h..DFh) non decodificati

- **Doc:** `memory-transfer-commands.md:3-5`.
- **Codice:** `gpu_commands.c:1567-1568`.
- **Pseudo-soluzione:** mappare tutti gli opcode sui due handler, come già fatto per 0x80..0x9F
  (`gpu_commands.c:1534-1565`).
- **Rischio:** nullo.
- **Verifica:** `Unhandled GP0 opcode` (`gpu_commands.c:1675`) nei log.

### R11. Nessun wrap negli upload e lettura fuori buffer

- **Doc:** `memory-transfer-commands.md:95-98`.
- **Codice:** `gpu_commands.c:1612`, `:1619`; `renderer_gl.c:1226-1230`; `renderer_vk.c:897-899`.
- **Pseudo-soluzione:** wrap lato CPU con AND 3FFh / 1FFh; spezzare il rettangolo in fino a quattro
  pezzi prima di `renderer_upload_vram_rect`. Vale anche per GP0(80) e per il fill (R3).
- **Rischio:** nullo; elimina anche la lettura oltre la fine di `vram.data`.
- **Verifica:** la riga `frame upload -> (x,y) wxh` (`gpu_commands.c:1348`, INFO).

### R12. Stato MDEC minore e letture a 16 bit

Voci:
- bit 15-0 (coperti da R2);
- metà alta in lettura a 16 bit: in `bus.c:383-387` passare l'indirizzo allineato a 4, poi estrarre
  la metà;
- blocco corrente con uscita in coda (`mdec.md:45-47`);
- bit 30 "last word received" (`:34`);
- DREQ1 (`:64-67`).

Impatto nullo con i player a DMA; rischio di blocco per i player a polling. Verifica: banco.

### R13. Lettura manuale della porta dati in ordine 16x16

`mdec.md:25-29` contro `mdec.c:520-521`. Riguarda solo i decoder via CPU. Pseudo-soluzione: blocchi
8x8 in uscita, riordino solo nel percorso DMA1.

### R14. Scale table con `/8` invece dei 13 bit alti

`mdec.md:317-318` contro `mdec.c:191`. Pseudo-soluzione: `scale_table[i] >> 3`. Effetto invisibile
(banco [7]).

### R15. Presentazione: crop NTSC e posizione X1/Y1

`gp1.md:141-144` e `:81-83` contro `gpu.c:133-140` e `gpu.c:83-159`. Sono scelte di presentazione:
il crop si spegne con `ZS1_OVERSCAN=0`, e per la posizione la doc stessa consiglia l'auto-centratura
(`gp1.md:257-258`).

---

## 6. Aperti NON VERIFICATI con sintomo noto

| Voce | Perché non decidibile | Misura che decide |
|---|---|---|
| Fotogramma stirato a fine FMV | la doc non dice quando si applicano GP1(05)/(08); il latch a inizio campo peggiorava (`renderer_gl.c:2334-2356`) | contare le scritture GP1(05)/(08) per campo (esiste già il conteggio GP1(03), `gpu.c:246-251`); `scripts/display_blank_probe.lua`, `scripts/display_map_probe.lua` |
| Striscia di 8 righe negli FMV PAL di Monsters & Co. | il codice applica `gp1.md:81-83` alla lettera; il gioco carica a y=8 | `scripts/vram_display_survey.lua` e `scripts/fmv_upload_coverage.lua`; se quelle righe dovrebbero essere nere, cercare il fill mancante (R3) |
| FIFO di uscita svuotata da un nuovo comando | nessuna riga | su `"mdec_ch1_done"` confrontare `emu.mdec_dma()` con le word prodotte |
| Kick ch0/ch1 ignorato | la doc non lo descrive | `ch%u kick ignored` a DEBUG (`bus.c:1064`) |
| Decode MDEC istantaneo | `dma.md:214` | solo confronto con l'hardware |
| Arrotondamenti YUV e 8->5 bit | `mdec.md:244`; nessuna riga per 8->5 | catture da hardware |
| Tetto di 1024 upload per campo | limite dell'emulatore | contare `"gp0_vram_upload"` per campo |

---

## 7. Fuori perimetro FMV, trovati di passaggio

- **GP1(10h) con indici sfasati di uno:**
  - doc (`gp1.md:191-202`): 02h finestra texture, 03h angolo alto sinistro, 04h angolo basso destro,
    05h offset, 07h versione;
  - codice (`gpu.c:399-436`): 02h texpage, 03h finestra, 04h alto sinistro, 05h basso destro, 06h
    offset.

  DIFFORME.
- **Latch GPUREAD consumato alla prima lettura** e, se vale 0, sostituito dal contatore `dummy++`
  (`gpu.c:592-600`), mentre la doc lo dice rileggibile (`gp1.md:204-205`). DIFFORME.
- **GP1(40h..FFh) non specchiati** su 00h..3Fh (`gp1.md:243-244` contro `gpu.c:449`, `:469-471`).
  DIFFORME.
- **Il fill lato GPU rispetta anche E6.1**, che la doc esclude (incluso in R3).
