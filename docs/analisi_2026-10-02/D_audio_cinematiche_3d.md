> **Allegato D** a `docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md`. Prodotto da un'analisi statica
> parallela il 2026-10-02 sul codice a `bbc1c7e` e su psx-spx al commit `00d5dcb`. Le voci riprese nel
> documento principale sono state riverificate riga per riga; le altre portano doc e codice citati per
> il controllo, ma non sono state ricontrollate una per una.

# Audio mancante nelle cutscene 3D in-engine: audit del codice contro psx-spx

Data: 2026-10-02. Repository: HEAD `bbc1c7e`, branch `stable_branch`.
Specifica: clone psx-spx al commit `00d5dcb`, file sotto `docs/ps1/`. Tutti i numeri di riga della
documentazione sono quelli di questo clone, scritti come `ps1/<percorso>:<riga>`; non coincidono con
quelli del fork `DOCS/` né con quelli usati negli audit del 2026-08-17.

Regola dell'audit (la stessa di `docs/CDROM_AUDIT_2026-08-17.md`): nessuna riga è CONFORME senza una
riga di documentazione e una riga di codice confrontate. Ciò che la documentazione non decide è
NON VERIFICATO. Nessun file del repository è stato modificato. `duckstation_ref/` non è stato letto.
Nessun disco o BIOS è presente in questo ambiente (`roms/`, `games/`, `savestates/` assenti): l'audit
è statico, e ogni ipotesi di gioco ha un piano di misura nella sezione 6.

---

## 0. Sintesi

L'FMV funziona e la cutscene in-engine no perché i due percorsi si separano in tre punti precisi, tutti
DIFFORMI dalla specifica:

1. **I settori XA-ADPCM scartati dal filtro vengono consegnati alla CPU come dati.** Con il filtro
   attivo un settore audio+realtime di un altro file/canale deve essere scartato in silenzio
   (`ps1/cdr/cdromdrive.md:604`), mentre `cdrom_commands.c:934-999` gli fa scattare un INT1, lo mette
   nel buffer dati e ne memorizza l'header per GetlocL (`:949-950`). Un FMV ha di solito un solo
   canale audio e un demuxer STR che scarta i settori senza header 0160h, quindi non se ne accorge.
   Un banco XA di dialoghi ha 8-32 canali (`ps1/cdr/cdromformat.md:698`): 7 settori su 8 diventano
   INT1 spurii, e GetlocL risponde con file/canale/submode di un altro canale. Un player XA che usa
   INT1 o GetlocL per capire dove si trova ferma lo stream (silenzio) o lo riposiziona (ripetizione).
2. **Lo streaming SPU-ADPCM non può funzionare in doppio buffer.** L'IRQ di voce confronta l'indirizzo
   IRQ con `curr_addr + 16` (`spu_voice.c:203-207`) invece che con il blocco letto
   (`ps1/spu/soundprocessingunitspu.md:824`): un IRQ posto sul primo blocco di un buffer circolare non
   scatta mai, e le voci "spente" non leggono e non generano IRQ (`:826-829` contro
   `spu_voice.c:244-247`). In più un Loop End senza Loop Start ferma la voce invece di saltare a LSAX
   (`:136-138`, `:163` contro `spu_voice.c:217-223`), e il KON scarta un LSAX scritto prima
   (`spu.c:82-83`). Risultato atteso: il buffer non viene ricaricato e si ripete, oppure la voce di
   streaming si ferma dopo il primo giro (silenzio).
3. **Lo sweep del volume principale non esiste** (`ps1/spu/soundprocessingunitspu.md:405-432` contro
   `spu.c:335-347`): una scena che parte con un fade-in in modo sweep resta al livello precedente, cioè
   muta per intero (XA e voci), e un fade-out in sweep non abbassa nulla.

Seguono, con probabilità minore: il drive che si ferma per qualsiasi INT pendente anche durante l'XA
(`cdrom.c:144-156` contro `ps1/cdr/cdromdrive.md:759-762`), ADPBUSY mai impostato (`cdrom.c:290-301`
contro `:62`), SPUCNT.14 che silenzia anche il CD (`spu_mixing.c:404-405` contro
`ps1/spu/soundprocessingunitspu.md:631`), l'AutoPause con stato `static` che interrompe subito un nuovo
Play (`cdrom_commands.c:1041`), la FIFO di scrittura manuale che scarta i dati scritti in modo Stop
(`spu.c:378-386` contro `:717-720`).

Per i due sintomi già registrati su Dino Crisis ("ripete ai cambi scena", "corre avanti alla scena")
i candidati più forti sono: la ripetizione da riposizionamento dello stream (punto 1), il buffer SPU
non ricaricato (punto 2), Mute/ADPMUTE/ATV/volume CD 0 mai applicati (la coda XA resta udibile quando
il gioco l'ha zittita), lo sweep lineare decrescente che scende sotto zero e risale a volume pieno
invertito; per il "corre avanti", IRQ SPU spurii alla scrittura di IRQA/TSA che fanno ricaricare il
buffer in anticipo, oppure una scena più lenta del reale per motivi di timing CPU/GPU fuori dal
perimetro audio. Il SPU stesso è avanzato esattamente di 768 cicli per campione e non può correre
avanti al clock emulato (righe T1-T2).

---

## 1. Perimetro

Codice letto per intero: `src/spu/{spu.c,spu_voice.c,spu_adsr.c,spu_mixing.c,spu_dma.c,spu_irq.c}`,
`include/spu.h`, `src/cdrom/{cdrom.c,cdrom_commands.c,cdrom_audio.c}`, `include/cdrom.h`,
`include/cdrom_audio.h`, `src/core/event_scheduler.c`, le parti SPU/CDROM/IRQ/DMA di `src/core/bus.c`
(`:223-264`, `:339-362`, `:395-405`, `:756-911`, `:1096-1234`), `src/core/dma.c:95-143`, `:230-250`,
`src/core/bus_irq.c:46-62`, `src/core/system.c:31-75`, `src/cpu/cpu_execution.c:208-228`,
`src/core/lua_debug.c` (API `emu.*`). `src/cdrom/cdrom_disc.c` solo per le funzioni chiamate dal
percorso audio.

Documentazione letta: `ps1/spu/soundprocessingunitspu.md` (sezioni 1-866 e 1176-1190),
`ps1/cdr/cdromdrive.md` (registri `:55-320`, comandi `:490-1135`, timing e buffer `:1870-1956`),
`ps1/cdr/cdromformat.md:625-958`, `ps1/cdr/cdromfileformats/streaming.md` (sezioni su audio, frame
rate e streaming SPU), `ps1/cdr/cdromfileformats/audio.md:469-482`, `ps1/system/dmachannels.md`,
`ps1/hardwarenumbers.md` (catalogo: nessun vincolo sull'audio oltre ai modelli SCPH-1001, `:5`, e
SCPH-7502, `:54`), più `ps1/system/interrupts.md:5`, `:27` per l'acknowledge di I_STAT.

Documenti del progetto letti prima: `docs/study/README.md`, `docs/study/SPU_2026-07-29.md`,
`docs/study/CDROM_XA_2026-07-29.md`, `docs/CDROM_AUDIT_2026-08-17.md`,
`docs/DMA_IRQ_GTE_MDEC_AUDIT_2026-08-17.md`. Lo stato di ogni loro voce aperta è nella sezione 5.

Legenda esito: **CONFORME** (doc e codice confrontati, concordano), **DIFFORME** (differenza
misurabile), **NON VERIFICATO** (la documentazione non decide, o il confronto non è conclusivo).

---

## 2. Perché un FMV suona e una cutscene in-engine no

| Meccanismo | Percorso hardware | Percorso nel codice | Cosa lo distingue da un FMV |
|---|---|---|---|
| (a) XA-ADPCM mentre il motore legge dati | Setmode bit 6 e bit 3, Setfilter file/canale, ReadS; settori audio+RT al decoder, settori dati a INT1 (`ps1/cdr/cdromdrive.md:590-608`, `:1118-1135`) | `cdrom_commands.c:858-999` -> `cdrom_audio.c:265-322` -> FIFO -> `spu_mixing.c:507-523` | banchi XA a molti canali, cambio canale per battuta, Pause/Setloc/ReadS a ogni scena, nessun demuxer STR che scarti i settori estranei |
| (b) CD-DA | Play, report, autopause, SPUCNT.0, volume CD (`:1014-1116`) | `cdrom_commands.c:109-134`, `:1001-1107` -> `cdrom_audio.c:328-337` | AutoPause e cambi di traccia tra una scena e l'altra |
| (c) SPU-ADPCM in streaming | DMA4 verso SPU RAM, IRQA, SPUCNT.6, flag di loop, LSAX, ENDX (`ps1/spu/soundprocessingunitspu.md:124-182`, `:816-866`) | `bus.c:1176-1196`, `spu_dma.c`, `spu_voice.c:160-234`, `spu_irq.c` | in un FMV XA la SPU non è coinvolta; titoli come Metal Gear Solid e Tron Bonne usano questo metodo per i dialoghi (`:859`, `:866`) |
| (d) sequenze con voci | KON/KOFF, pitch, ADSR, sweep (`:196-571`) | `spu.c`, `spu_voice.c`, `spu_adsr.c` | volumi in sweep, fade del volume principale ai cambi scena |

Un FMV STR mette l'audio in un solo canale XA, alternato a settori video che sono dati
(`ps1/cdr/cdromfileformats/streaming.md:776-781`; interleave 1/8 audio e 7/8 video in
`ps1/cdr/cdromformat.md:683`, `:687`). Il filtro trova sempre il suo canale e i settori dati sono
attesi. Una cutscene in-engine pesca una battuta da un file XA con fino a 32 canali
(`ps1/cdr/cdromformat.md:637`, `:698`), oppure carica audio nella SPU: esattamente i casi in cui il
codice diverge.

---

## 3. Tabelle di verifica

### 3.1 XA-ADPCM: instradamento, filtro, decodifica

| ID | Aspetto | Riga doc | Riga codice | Esito | Sintomo atteso |
|---|---|---|---|---|---|
| X1 | Setmode bit 6 (XA-ADPCM) e bit 3 (XA-filter) | `ps1/cdr/cdromdrive.md:518` "1=Send XA-ADPCM sectors to SPU Audio Input"; `:521` "1=Process only XA-ADPCM sectors that match Setfilter" | `cdrom_commands.c:418`, `:420` | CONFORME | - |
| X2 | Setfilter memorizza file e canale | `ps1/cdr/cdromdrive.md:507-509` "ignores sectors except those which have the same channel and file numbers in their subheader" | `cdrom_commands.c:398-403` | CONFORME | - |
| X3 | Il match richiede file E canale | `ps1/cdr/cdromdrive.md:599` "reject if filter_enabled(setmode.3) AND selected file/channel doesn't match" | `cdrom_commands.c:871-873` | CONFORME | - |
| X4 | All'ADPCM servono submode audio e realtime | `ps1/cdr/cdromdrive.md:600` "reject if submode isn't audio+realtime (bit2 and bit6 must be both set)" | `cdrom_commands.c:858-860`, `:875` | CONFORME | - |
| X5 | Scarto se CD-DA o non MODE2 | `ps1/cdr/cdromdrive.md:596-597` "reject if CD-DA AUDIO format" / "reject if sector isn't MODE2 format" | `cdrom_commands.c:858-875`: nessun test su `raw[15]` né sulla traccia | DIFFORME | un settore Mode 1 i cui byte 16-19 sembrano un subheader audio+RT va al decoder: rumore e INT1 perso. Raro |
| X6 | Settore audio+RT scartato dal filtro: nessuna consegna dati | `ps1/cdr/cdromdrive.md:604` "reject if filter_enabled(setmode.3) AND submode is audio+realtime (bit2+bit6)"; `:1132-1133` "INT1 is generated only for non-ADPCM sectors" | `cdrom_commands.c:934-999`: il ramo `else` prende ogni settore rifiutato dall'ADPCM, memorizza l'header per GetlocL (`:949-950`), lo rende il buffer di lettura (`:953-966`) e alza INT1 (`:986-992`) | DIFFORME | in un file XA a N canali, N-1 settori su N diventano INT1 con payload ADPCM; GetlocL riporta file/canale/submode di un altro canale. Il player ferma lo stream (silenzio) o lo riposiziona (ripetizione); ogni INT1 blocca anche il drive fino all'ack (X21) |
| X7 | Settore audio+RT con ADPCM disattivo e filtro attivo | `ps1/cdr/cdromdrive.md:598` "reject if adpcm_disabled(setmode.6)" seguito da `:604` | `cdrom_commands.c:875`, `:934` | DIFFORME | come X6 |
| X8 | Prima consegna dati indipendente da file/canale | `ps1/cdr/cdromdrive.md:605` "1st delivery attempt: send INT1+data, unless there's another INT pending" | `cdrom_commands.c:934-999` (nessun filtro sui settori dati) | CONFORME; il secondo tentativo con controllo di file/canale (`:606-608`) non è modellato: NON VERIFICATO se conti | - |
| X9 | Coding info: bit 0 stereo, bit 2 18900 Hz, bit 4 8 bit | `ps1/cdr/cdromformat.md:666-668` "0-1 Mono/Stereo", "2-2 Sample Rate (0=37800Hz, 1=18900Hz", "4-5 Bits per Sample (0=Normal/4bit, 1=8bit" | `cdrom_commands.c:867-870` | CONFORME | - |
| X10 | Enfasi (bit 6) | `ps1/cdr/cdromformat.md:669`; `:950` "The Emphasis feature isn't used by any known PSX games" | `cdrom_commands.c:867-870` (ignorata) | CONFORME di fatto | - |
| X11 | 18 porzioni, filtri 0-3, campione saturato e retroazionato | `ps1/cdr/cdromformat.md:750-751`, `:780-781`, `:835-837` "s = MinMax(s,-8000h,+7FFFh) ... older=old, old=s" | `cdrom_audio.c:77-141`, `:283-286` | CONFORME | - |
| X12 | Dimensione settore irrilevante per l'XA | `ps1/cdr/cdromdrive.md:1134-1135` "the hardware does always decompress all 900h bytes" | `cdrom_commands.c:923-924` (`raw + 24`, 18 porzioni) | CONFORME | - |
| X13 | 37800 -> 44100: zigzag a 29 tap, 7 uscite ogni 6 ingressi | `ps1/cdr/cdromformat.md:879-893` | `cdrom_audio.c:201-235` | CONFORME | - |
| X14 | 18900 -> 44100 | `ps1/cdr/cdromformat.md:934-935` "lower-pitch zigzag, and gets spread across about fifty 44100Hz samples" (nessuna tabella pubblicata) | `cdrom_audio.c:167-182`, `:209-215`, `:242-263` | NON VERIFICATO: rapporto 7:3 giusto (`:251-252`), tabelle di provenienza ignota, ordine dei tap opposto a `:205` | voce a 18.9 kHz distorta o con aliasing, non silenzio |
| X15 | Storia ADPCM (old/older) attraverso un seek | `ps1/cdr/cdromformat.md:853-854` "*maybe* it's silently done automatically when issuing seek commands?" | `cdrom_audio.c:280-288` (stato portato attraverso ogni seek) | NON VERIFICATO | click all'inizio di una battuta |
| X16 | Coda XA dopo Pause/Setloc/Stop/Init/Setfilter | `ps1/cdr/cdromdrive.md:188` (SMADPCLR solo per il sound map); `:368-372` (tre slot da 900h nel decoder) | nessun flush in `cdrom_commands.c:208-233`, `:265-270`, `:295-322`, `:325-381`, `:398-403`; coda in `cdrom_audio.c:26-41` | NON VERIFICATO | coda vecchia udibile dopo il cambio scena: al massimo un settore (53 ms per 37.8 kHz stereo, 213 ms per 18.9 kHz mono) |
| X17 | Tetto della FIFO pari a un solo settore 18.9 kHz mono | `ps1/cdr/cdromformat.md:686` "1/32 18900Hz Mono ADPCM compressed Audio at double speed" | `cdrom_audio.h:21` (2352x4 = 9408 frame = 4032 campioni x 7/3) | NON VERIFICATO (la doc non regola la FIFO) | il residuo del settore precedente (fino a 64 frame di ritardo SPU) viene scartato: un click ogni 213 ms |
| X18 | ADPBUSY in HSTS | `ps1/cdr/cdromdrive.md:62` "ADPBUSY  ADPCM busy (R, 1=playing XA-ADPCM)"; `:1020-1021` "as seen in 1F801800h.Bit2, which works as usually for XA-ADPCM" | `cdrom.c:290-301` (bit 2 mai impostato) | DIFFORME | un gioco che legge "XA in riproduzione" vede sempre 0: considera finita la battuta e la ferma, o non parte (silenzio) |
| X19 | GetlocP segue la posizione per ogni settore letto | `ps1/cdr/cdromdrive.md:904-905` "Retrieves 8 bytes of position information from Subchannel Q"; `:900-901` "GetlocP does work during Seek" | `cdrom_commands.c:975-978` aggiorna la SubQ solo nel ramo dati; il ramo XA `:922-933` no | DIFFORME (latente) | oggi mascherato da X6; correggendo solo X6, GetlocP si congela durante l'XA |
| X20 | Read dopo Pause riconsegna l'ultimo settore | `ps1/cdr/cdromdrive.md:811-814` "reading resumes at the most recently received sector (ie. returning that sector once another time)" | `cdrom_commands.c:69-76` (riprende dal successivo) | DIFFORME | un settore in meno alla ripresa; basso |
| X21 | La lettura continua a 75/150 settori/s qualunque cosa faccia la CPU | `ps1/cdr/cdromdrive.md:759-762` "The Read commands are continously receiving 75 sectors per second (or 150 sectors at double speed), and, basically, the software must be fast enough"; `:1953-1956`; `:1929-1931` | `cdrom.c:144-156` (con un INT pendente il drive non si ripianifica), riavvio solo all'ack `cdrom.c:412-427` | DIFFORME | l'XA si sospende finché un INT qualsiasi (anche l'INT3 di un GetlocP di polling, o un INT1 spurio di X6) non è riconosciuto: buchi o silenzio se l'ack tarda o manca |
| X22 | Pause rifiutata in fase di seek | `ps1/cdr/cdromdrive.md:586-588` | `cdrom_commands.c:299-303` | CONFORME | - |
| X23 | GetlocL: 80h in seek e su tracce audio | `ps1/cdr/cdromdrive.md:892`, `:896-898` | `cdrom_commands.c:453-466` | CONFORME | - |
| X24 | IRQ CDROM soggetto a HINTMSK | `ps1/cdr/cdromdrive.md:179` "fires an interrupt whenever (HINTMSK & HINTSTS) is non-zero" | `cdrom.c:44-63` -> `bus_irq.c:46-62` (maschera mai letta) | DIFFORME | IRQ per INT mascherati; basso |
| X25 | L'ack svuota la FIFO risultati | `ps1/cdr/cdromdrive.md:195-197` "After acknowledge, the result FIFO is drained" | `cdrom.c:404-428` | DIFFORME | basso |

### 3.2 CD-DA

| ID | Aspetto | Riga doc | Riga codice | Esito | Sintomo atteso |
|---|---|---|---|---|---|
| D1 | Play parte da Setloc pendente o dalla posizione corrente | `ps1/cdr/cdromdrive.md:1032-1036` | `cdrom_commands.c:109-134` | CONFORME | - |
| D2 | Parametro di traccia N+1..99h riavvia la traccia corrente | `ps1/cdr/cdromdrive.md:1037` | `cdrom_commands.c:111-113` (clamp all'ultima traccia) | DIFFORME | traccia sbagliata; basso |
| D3 | In Play il bit CDDA è ignorato | `ps1/cdr/cdromdrive.md:1069-1071` "during Play the drive is always in CD-DA mode, regardless of that bit" | `cdrom_commands.c:1001-1107` (nessun test su `cdda_enable`) | CONFORME | - |
| D4 | Doppia velocità in Play | `ps1/cdr/cdromdrive.md:1072-1073` "it can be used for a fast forward effect (with audible output)" | `cdrom_commands.c:1104-1106` + `cdrom_audio.c:328-337` (588 frame per settore) + scarto del più vecchio `cdrom_audio.c:31-35` | NON VERIFICATO | avanti veloce a blocchi |
| D5 | Report: 8 byte, assoluto su asect 00/20/40/60h, in traccia su 10/30/50/70h | `ps1/cdr/cdromdrive.md:1077-1095` | `cdrom_commands.c:1072-1096` | CONFORME (picco sempre 0) | - |
| D6 | AutoPause su transizione di traccia durante il Play | `ps1/cdr/cdromdrive.md:1100` "Issue INT4(stat) and PAUSE at end of TRACK"; `:1103-1104` "determined by sensing a track number transition in SubQ position info" | `cdrom_commands.c:1039-1054`: `static uint8_t prev_track` (`:1041`) sopravvive a Pause, Setloc e Play | DIFFORME | se il Play precedente è finito con Pause, il primo settore del Play successivo su un'altra traccia fa scattare INT4 e la pausa: musica della cutscene muta |
| D7 | Dopo l'autopause il disco resta alla fine della traccia vecchia | `ps1/cdr/cdromdrive.md:1104-1107` | `cdrom_commands.c:1043-1051` (resta sul primo settore della nuova) | DIFFORME | basso |
| D8 | Il CD-DA non attende gli ack | `ps1/cdr/cdromdrive.md:1929-1931` | `cdrom.c:149-153` (ritenta ogni 1000 cicli) | DIFFORME | piccoli ritardi; basso |
| D9 | Play su traccia dati: silenzio | `ps1/cdr/cdromdrive.md:1044-1046` "all sectors from data tracks are treated as 00, so no sound is played" | `cdrom_commands.c:1035-1037` (nessun push; la SPU tiene l'ultimo campione, `spu_mixing.c:507-516`) | CONFORME in effetto | - |

### 3.3 Dal CD alla SPU

| ID | Aspetto | Riga doc | Riga codice | Esito | Sintomo atteso |
|---|---|---|---|---|---|
| C1 | SPUCNT bit 0 abilita l'ingresso CD | `ps1/spu/soundprocessingunitspu.md:640` "I2SA (CD-ROM) Input Enable (0=Off, 1=On) (for CD-DA and XA-ADPCM)" | `spu_mixing.c:360` | CONFORME | - |
| C2 | SPUCNT bit 2 manda il CD nel riverbero | `ps1/spu/soundprocessingunitspu.md:638` | `spu_mixing.c:363-366` | CONFORME | - |
| C3 | Volume CD 1F801DB0h/DB2h, con segno | `ps1/spu/soundprocessingunitspu.md:442` "0-15  Volume  (-8000h..+7FFFh)" | `spu_mixing.c:518-523`, con `:520-521` (volume 0 trattato come pieno) | DIFFORME | un gioco che porta il volume CD a 0 per un fade o un cambio scena sente tutto a volume pieno: la coda XA resta udibile |
| C4 | Mute (0Bh) azzera l'uscita CD | `ps1/cdr/cdromdrive.md:1019-1022` "muting is just forcing the CD output volume to zero"; `:1023` "Mute is used by Dino Crisis 1" | `cdrom.c:471-492` (`cdrom_get_audio_frame`) non è chiamata da nessuno; la SPU legge la FIFO direttamente a `spu_mixing.c:510-511` | DIFFORME | audio udibile dove il gioco l'ha zittito |
| C5 | ADPMUTE (ADPCTL bit 0) | `ps1/cdr/cdromdrive.md:251` "ADPMUTE Mute XA-ADPCM (1=mute)" | memorizzato a `cdrom.c:436`, mai applicato (C4) | DIFFORME | come C4 |
| C6 | Porte ATV0-ATV3 e ADPCTL | `ps1/cdr/cdromdrive.md:227-230`, `:249-255` | `cdrom.c:363`, `:371`, `:372`, `:430`, `:431-445` | CONFORME (risolto rispetto all'audit di agosto) | - |
| C7 | Matrice ATV applicata, saturata fino al doppio volume | `ps1/cdr/cdromdrive.md:234-240` "the saturation works up to double volume"; `:242` | commit a `cdrom.c:437-444`; applicazione solo in `cdrom.c:486-491`, codice morto | DIFFORME | mono/stereo e fade via ATV ignorati (Resident Evil 2 li usa per i fade, `:246-247`) |
| C8 | SPUCNT bit 14 non riguarda il CD | `ps1/spu/soundprocessingunitspu.md:631` "Mute SPU (0=Mute, 1=Unmute)  (Don't care for CD Audio)" | `spu_mixing.c:404-405` (muto applicato dopo aver sommato CD e riverbero) | DIFFORME | XA muto se il gioco silenzia le voci con il bit 14 durante la scena |
| C9 | SPUCNT bit 15 non riguarda il CD | `ps1/spu/soundprocessingunitspu.md:630` | `spu.c:127-132` (solo voci), `spu_mixing.c:360` | CONFORME | - |
| C10 | Il volume principale si applica anche al CD | `ps1/cdr/cdromdrive.md:1015-1016` "init the following SPU Registers: CD Audio Volume, Main Volume, and SPU Control Bit0" | `spu_mixing.c:404-408` | CONFORME | - |
| C11 | Capture CD in SPU RAM 000h-7FFh, prima del volume | `ps1/spu/soundprocessingunitspu.md:53-54` "CD Audio before Volume processing" | `spu_mixing.c:70-79`, `:369-374`, `include/spu.h:282` (array privato, valore dopo il volume, passo 2 su array di halfword) | DIFFORME | lip-sync e IRQ di capture assenti (I7) |
| C12 | La FIFO CD è consumata a un frame per tick SPU | `ps1/spu/soundprocessingunitspu.md:1180-1181` | `spu_mixing.c:477-524` | CONFORME | - |

### 3.4 SPU: voci, loop, inviluppi, volumi

| ID | Aspetto | Riga doc | Riga codice | Esito | Sintomo atteso |
|---|---|---|---|---|---|
| V1 | KON copia lo Start nell'indirizzo corrente | `ps1/spu/soundprocessingunitspu.md:131` "The start address is copied to the current address upon Key On." | `spu.c:85` | CONFORME | - |
| V2 | KON azzera ENVX e avvia l'Attack | `:557` "automatically initializes ADSR Volume to zero" | `spu.c:90-93` | CONFORME | - |
| V3 | KON azzera ENDX | `:583` "The bits get CLEARED when setting the corresponding KEY ON bits." | `spu.c:75` | CONFORME | - |
| V4 | Scrivere SSA non tocca la voce in corso | `:130` | `spu.c:284-286` | CONFORME | - |
| V5 | KON non cancella LSAX | `:133-138` (LSAX cambia solo con Loop Start o con una scrittura), `:163` | `spu.c:82-83` (`loop_addr_set = false`, `ignore_loop = false`) e `spu_voice.c:219` | DIFFORME | un LSAX scritto prima del KON viene ignorato: la voce si ferma al primo Loop End, silenzio dopo il primo buffer |
| V6 | Loop End salta sempre a LSAX (Code 3) | `:136-138` "it copies the repeat addresss register setting to the current address; that, <after> playing the current ADPCM block"; `:173` "Code 3 = End+Repeat (jump to Loop-address, set ENDX flag)" | `spu_voice.c:217-223` (salta solo se `loop_addr_set`, altrimenti `curr_addr = 0xFFFFFFFF` e la voce si spegne a `:294-300`) | DIFFORME | voce di streaming che si ferma |
| V7 | Code 1 (End+Mute): salto, Release, Env=0, la voce continua a leggere | `:164`, `:171` "jump to Loop-address, set ENDX flag, Release, Env=0000h"; `:180` "There's no way to stop the output" | `spu_voice.c:222`, `:294-300` (voce spenta) | DIFFORME | audio equivalente; nessun IRQ da voci "mute" (I2) |
| V8 | Loop Start copia sempre l'indirizzo in LSAX | `:134-135`, `:165` | `spu_voice.c:210` (saltato se `ignore_loop`), `spu.c:309` (ogni scrittura di LSAX imposta `ignore_loop` fino al KON successivo) | DIFFORME | dopo una scrittura software di LSAX, i Loop Start dei blocchi riscritti in streaming non spostano più il punto di loop: si ricomincia dal punto sbagliato (ripetizione) |
| V9 | Salto ed ENDX dopo aver suonato il blocco finale | `:136-138`; `:584` | `spu_voice.c:216-228`, `:336-339` (ENDX all'inizio del blocco finale) | DIFFORME | ENDX un blocco (28 campioni) in anticipo; basso |
| V10 | KOFF avvia il Release | `:561`, `:569-571` | `spu.c:61-69`, `spu_voice.c:250-253` | CONFORME | - |
| V11 | KON/KOFF applicati al tick a 44.1 kHz successivo | `:106-108` "it may take 1/44100 seconds (300h clock cycles) until it has actually realized the new value" | `spu.c:350-353` (latch), `spu_mixing.c:319`, `spu.c:59-107` | CONFORME | - |
| V12 | Lettura di KON/KOFF | `:588-590` "reading returns the most recently 32bit value" | `spu.c:206-209` (latch già consumato) | DIFFORME | trascurabile |
| V13 | Contatore di pitch, tetto 4000h | `:226-237` "IF Step>3FFFh then Step=4000h" | `spu_voice.c:278-289` | CONFORME | - |
| V14 | Modulazione di pitch | `:229-234` | `spu_voice.c:280-285` | CONFORME | - |
| V15 | Generatore di rumore | `:610-618` | `spu_mixing.c:51-64` | NON VERIFICATO (formulazione diversa, equivalenza non dimostrata qui) | - |
| V16 | ADSR: fasi ed esponenziale oltre 6000h | `:447-481` | `spu_adsr.c:45-155` (rate+8 equivale ai tre casi di `:455-462`) | CONFORME | - |
| V17 | Passo "tutti 1" non avanza mai | `:490-491` | `spu_adsr.c:30-38` (caso assente) | DIFFORME | trascurabile |
| V18 | Direzione dello sweep nel bit 13 | `:418` "13 Sweep Direction (0=Increase, 1=Decrease)" | `spu_voice.c:359`, `:379` | CONFORME | - |
| V19 | Lo sweep parte dal livello corrente | `:427` "Sweep starts at the current volume" | `spu.c:261-270` | CONFORME | - |
| V20 | Sweep lineare decrescente si ferma a 0 | `:431-432` "the current volume level increases to +7FFFh, or decreases to 0000h"; `:480-481` "ELSE ; decreasing / AdsrLevel = MAX(AdsrLevel, 0)" | `spu_voice.c:364-371`, `:384-391` (clamp a -8000h) | DIFFORME | un fade-out lineare passa lo zero e risale fino a -8000h: il suono torna a volume pieno con fase invertita, udibile come "ritorno" dell'audio al cambio scena |
| V21 | Sweep esponenziale crescente oltre 6000h | `:455-462` (passo o contatore divisi per 4) | `spu_voice.c:367-368` (passo proporzionale al livello) | DIFFORME | fade-in più rapido; basso |
| V22 | Bit di fase 12 | `:419`, `:433-435`, `:487-511` | `spu_voice.c:355-393` (ignorato) | DIFFORME | basso |
| V23 | Sweep del volume principale | `:405-406` (MVOLL/MVOLR condividono il formato dei volumi voce), `:414-432` | `spu.c:335-347` (solo il modo diretto cambia `main_vol_*_cur`; nessun tick altrove in `src/`) | DIFFORME | scena che parte con un fade-in in sweep: tutto muto, XA compreso; fade-out in sweep: nessun calo |
| V24 | Volume principale corrente MVOLX | `:526-531` "Current Volume" | `spu.c:228-229` | DIFFORME (conseguenza di V23) | un gioco che attende la fine del fade leggendo MVOLX resta in attesa o va in timeout |
| V25 | Volume corrente per voce VOLX (1F801E00h+4N) | `:528-531` "VOLXL (current voice volume left)" | `spu.c:174-178` (restituisce `EnvelopeVol`, cioè ENVX, per sinistra e destra) | DIFFORME | basso |

### 3.5 SPU: IRQ e trasferimenti

| ID | Aspetto | Riga doc | Riga codice | Esito | Sintomo atteso |
|---|---|---|---|---|---|
| I1 | IRQ quando una voce legge ADPCM all'indirizzo IRQ | `ps1/spu/soundprocessingunitspu.md:824` "Triggers an IRQ when a voice reads ADPCM data from the IRQ address."; `:831-834` | `spu_voice.c:203-207` confronta con `curr_addr + 16`, `spu_irq.c:20-21` (uguaglianza esatta) | DIFFORME | IRQ un blocco in anticipo; mai per un IRQA sul primo blocco di un buffer circolare (il blocco precedente non appartiene al loop) né a metà blocco: il buffer non viene ricaricato, si ripete o si ferma |
| I2 | Le voci inattive continuano a leggere e generano IRQ | `:826-829` "even if the ADSR pattern has finished the Release period - so even inaudible voices can trigger IRQs" | `spu_voice.c:244-247` (voce spenta: nessuna lettura), `spu_adsr.c:145-149`, `spu_voice.c:294-300` | DIFFORME | IRQ "di temporizzazione" da voci mute che non arrivano |
| I3 | IRQ solo con SPUCNT.15=1 | `:635` "IRQ9 Enable ... only when Bit15=1" | `spu_irq.c:17` | DIFFORME | trascurabile |
| I4 | Il flag IRQ si riconosce solo con SPUCNT.6=0 | `:635` "(0=Disabled/Acknowledge, 1=Enabled"; `:655` "6 IRQ9 Flag" | `bus.c:253-257`: l'ack di I_STAT azzera anche `irq9_flag` e STATX.6 | DIFFORME | IRQ in più rispetto all'hardware; un gestore che legge STATX.6 dopo l'ack di I_STAT lo trova a 0 e non ricarica |
| I5 | Una scrittura di IRQA o di TSA non è un accesso alla RAM | `:824`, `:852` (solo letture di voce e trasferimenti) | `spu_irq.c:41-56` (IRQ immediato se `curr_addr` o `transfer_addr` coincide), `spu.c:375` (controllo IRQ sulla scrittura di TSA) | DIFFORME | IRQ spurio all'istante: il gestore ricarica la metà che la voce sta suonando, audio che salta in avanti o si ripete |
| I6 | I trasferimenti DMA e manuali generano IRQ | `:852` "Data Transfers (usually via DMA4) to/from SPU-RAM do also trap SPU interrupts." | `spu_dma.c:30`, `:60`, `:104`, `:123` | CONFORME | - |
| I7 | IRQ di capture con IRQA in 0000h-01FFh | `:837-841` "will trigger IRQs on writes to the four capture buffers"; `:844` | assente (`spu_mixing.c:70-79`) | DIFFORME | scene sincronizzate sulla capture (lip-sync, `:863`, `:865`) senza IRQ |
| I8 | Il registro TSA non incrementa | `:675-679` "whilst the 1F801DA6h value DOESN'T increment" | `spu_dma.c:38`, `:70`, `:111`, `:130` | DIFFORME | basso |
| I9 | L'indirizzo interno incrementa | `:675-679` | `spu_dma.c:26-36` | CONFORME | - |
| I10 | FIFO di scrittura manuale riempita in modo Stop | `:683` "Data (max 32 halfwords)"; `:717-720` "Set ATTR to "Stop" ... Write 1..32 halfword(s) to the Fifo ... Set ATTR to "Manual Write"" | `spu.c:378-386` e `spu_dma.c:89-90` scartano le scritture se il modo non è 1 o 2 | DIFFORME | campioni caricati via I/O nella sequenza documentata persi: voci mute |
| I11 | STATX bit 8/9/10 | `:651-653` | `spu_dma.c:22`/`:39`, `:48`/`:71` (busy alzato e abbassato nella stessa chiamata), bit 8/9 mai alzati | DIFFORME | basso |
| I12 | STATX 5-0 copia di ATTR | `:656` | `spu.c:142-144` | CONFORME | - |
| I13 | RAM_CTRL 1F801DACh = 0004h | `:687-704` | `spu.c:221`, `:377` | CONFORME per 0004h | - |
| I14 | Scritture a 8 bit | `:113-116` "8bit writes to EVEN addresses are executed as 16bit writes" | `spu.c:409-414` (fusione con il valore precedente) | DIFFORME | trascurabile |
| I15 | DMA4: MADR e BCR aggiornati in SyncMode 1 | `ps1/system/dmachannels.md:27-29`, `:56-57` | `dma.c:241-247` (solo in scrittura), `bus.c:1166-1204` | DIFFORME | basso |
| I16 | DMA4 a 4 clk/word | `ps1/system/dmachannels.md:210` | `bus.c:1219` | CONFORME (valore) | - |

### 3.6 Tempo

| ID | Aspetto | Riga doc | Riga codice | Esito | Sintomo atteso |
|---|---|---|---|---|---|
| T1 | 768 cicli CPU per campione | `ps1/spu/soundprocessingunitspu.md:1180-1181` "It is exactly 768 x 44.1 Khz" | `include/spu.h:26`, `spu_mixing.c:473-479` | CONFORME | - |
| T2 | La SPU non corre avanti all'ospite | `:106-108` | `spu_mixing.c:573-590` genera solo fino a `cpu_cycle_counter`, che avanza per istruzione (`cpu_execution.c:214-218`) | CONFORME | il "corre avanti" non nasce dal clock SPU |
| T3 | Riallineamento oltre 1 s di ritardo | nessuna regola | `spu_mixing.c:581-585` | NON VERIFICATO (scatta solo dopo una pausa del debugger) | - |
| T4 | Latenza di rilevazione dell'IRQ di voce | `:824` | `event_scheduler.c:262-265`, `include/spu.h:29` (lotti di 64 campioni) | DIFFORME | IRQ fino a 1.45 ms in ritardo; basso |
| T5 | Ritmo dei settori | `ps1/cdr/cdromdrive.md:1929-1931` "SystemClock*930h/4/44100Hz for Single Speed (and half as much for Double Speed)" | `include/cdrom.h:32-33` | CONFORME | - |
| T6 | Il DMA consuma tempo CPU | `ps1/system/dmachannels.md:237-238` "will stall the CPU until the DMA is finished" | `bus.c:1217-1227`, `:902` riducono solo `downcount`; `cpu_cycle_counter` non avanza | DIFFORME | la CPU emulata ha più tempo del reale: scena più veloce, quindi audio in ritardo, non in anticipo |
| T7 | Catch-up SPU prima che il DMA4 scriva la RAM | `ps1/spu/soundprocessingunitspu.md:106-108` | `bus.c:1176-1180` (nessun `spu_catch_up`) | NON VERIFICATO (le scritture di registro subito prima del DMA fanno già il catch-up) | - |

---

## 4. Classifica dei DIFFORME

Ordine: probabilità di spiegare l'audio mancante nelle cutscene in-engine mentre l'FMV suona; per i
sintomi Dino Crisis, la riga "Ripete / corre avanti".

### R1. Settori XA scartati dal filtro consegnati come dati (X6, X7; accoppiati X5, X19, X21)

- **Perché è primo.** È l'unica divergenza che separa nettamente FMV e cutscene sullo stesso percorso
  XA. FMV: un canale audio, il filtro lo trova sempre, i settori dati sono video STR che il player si
  aspetta e convalida. Cutscene: un banco XA a più canali (`ps1/cdr/cdromformat.md:698` "one file
  with eight 1/8 audio channels"), e i settori degli altri canali (o quelli "vuoti" con canale FFh di
  Ace Combat 3, `:640-641`, `:719`) diventano INT1 + buffer dati + latch GetlocL. È la stessa forma
  già vista con Monsters & Co.: `CLAUDE.md` registra che far entrare settori ADPCM nel latch di
  GetlocL "takes the game's demuxer off the video stream and its speech never plays"; X6 lo fa
  ancora, per i settori rifiutati dal filtro.
- **Ripete / corre avanti.** Ripete: un player che vede un GetlocL con il canale sbagliato o un INT1
  inatteso torna a Setloc+ReadS sull'inizio della battuta. Corre avanti: no.
- **Pseudo-soluzione** (algoritmo di `ps1/cdr/cdromdrive.md:595-608`), in `cdrom_execute_drive`:

  ```c
  uint8_t mode_byte = raw[15];
  bool mode2    = (mode_byte == 2);                       /* :597 */
  bool cdda_trk = traccia_audio(current_lba);             /* :596 */
  bool audio_rt = mode2 && ((raw[18] & 0x44) == 0x44);    /* :600 */
  bool fc_ok    = raw[16] == xa_filter_file && raw[17] == xa_filter_channel;

  /* Posizione: per OGNI settore letto, non solo per quelli dati (X19, :904-905).
   * Mantiene la regola LibCrypt: non aggiornare se lo SBI copre il settore. */
  aggiorna_subq_e_head(current_lba);

  if (!cdda_trk && mode2 && xa_adpcm_enable && audio_rt &&
      (!xa_filter_enable || fc_ok)) {                     /* :596-601 */
      decodifica_xa(); avanza(); ripianifica(periodo); return;
  }
  if (xa_filter_enable && audio_rt) {                     /* :604 */
      /* scartato: niente INT1, niente buffer, niente latch GetlocL */
      avanza(); ripianifica(periodo); return;
  }
  consegna_dati();   /* come oggi: latch (:949-950), buffer, INT1 (:986-992) */
  ```

  Spostare il blocco SubQ di `cdrom_commands.c:975-978` prima della biforcazione è obbligatorio:
  oggi GetlocP durante l'XA funziona solo perché i settori degli altri canali passano dal ramo dati.
- **Rischio.** Medio. Cambia il numero di INT1 visti da ogni titolo che usa XA con filtro, compresi
  gli FMV con audio multilingua; la regola "solo i dati entrano nel latch di GetlocL" resta com'è.
  Da rifare: avvio BIOS, FMV di Ace Combat 2, nuova partita di Monsters & Co., menu di Dino Crisis
  (lo SBI passa dal blocco SubQ spostato).
- **Verifica.** Savestate dentro la cutscene (6.2). Prima e dopo: INT1 per campo
  (`emu.on_event`, "cdrom_int1"), settori XA per campo (8o valore di `emu.cd_audio()`), frame spinti
  nella FIFO (2o valore). Le righe INFO già presenti "XA stream now file=... channel=..." e "XA
  interleave step" (`cdrom_commands.c:898`, `:912-917`) danno file/canale e passo dell'interleave
  senza log di debug. Con il difetto: INT1 per campo circa (N-1)/N dei settori letti in un tratto
  pura XA; dopo: INT1 solo per settori dati. Ascolto: `ZS1_XA_DUMP` e `ZS1_AUDIO_DUMP`.

### R2. IRQ di voce sull'indirizzo sbagliato, voci spente mute anche per l'IRQ (I1, I2; accoppiati I4, I5, T4)

- **Perché.** Lo streaming SPU-ADPCM "overwriting chunks of an endlessly looping sample in-place"
  (`ps1/spu/soundprocessingunitspu.md:855-856`) vive di questi IRQ, e la doc elenca titoli con
  dialoghi in streaming (`:859` Metal Gear Solid, `:866` Tron Bonne). Nello schema classico l'IRQ si
  alterna tra inizio e metà del buffer: il primo non scatta mai qui, perché il blocco che lo precede
  in memoria non fa parte del loop e il confronto è con `curr_addr + 16`. Il gioco non ricarica la
  metà, la voce rigira i vecchi dati.
- **Ripete / corre avanti.** Ripete: sì, alla lettera. Corre avanti: I5 (IRQ immediato alla scrittura
  di IRQA se `transfer_addr` coincide, tipico dopo un DMA che finisce proprio all'inizio dell'altra
  metà) fa ricaricare la metà in riproduzione: salto in avanti.
- **Pseudo-soluzione** (`:824`, `:826-834`, `:852`):

  ```c
  /* voice_decode_block, blocco letto a A = curr_addr */
  if ((control & SPUCNT_IRQ9) && (control & SPUCNT_ENABLE) && !irq9_flag) {
      uint32_t q = irq_addr * 8;
      if (q == A || q == A + 8) alza_irq9();   /* :824, :831-834 */
  }
  /* voci "spente": continuano a leggere blocchi e seguire i loop anche con
   * inviluppo a 0 e senza contribuire al mix (:826-829); almeno quando IRQ9
   * è abilitato, per non pagare 24 decodifiche sempre */
  /* spu_update_irq_addr: tenere solo l'azzeramento se IRQ9 è disabilitato,
   * togliere il controllo su curr_addr e transfer_addr (spu_irq.c:41-56) */
  /* spu.c:375: niente spu_check_irq sulla sola scrittura di TSA */
  /* bus.c:253-257: l'ack di I_STAT non tocca irq9_flag; abbassare la linea
   * IRQ_SPU in spu_set_control quando SPUCNT.6 passa a 0 (:635) */
  ```

- **Rischio.** Medio: più IRQ per chi programma IRQA dentro un loop; costo host se le voci spente
  vengono decodificate sempre. Nessun cambio di `sizeof(Spu)`.
- **Verifica.** Watchpoint di scrittura su IRQA (6.3): con il difetto le scritture di IRQA cessano
  dopo una o due ricariche; corretto, la cadenza resta costante e pari a mezzo buffer / (pitch x
  44100 / 1000h). Contare anche KON (5o valore di `emu.spu_stats()`) e DMA4 (watchpoint su
  `0x1F8010C8`).

### R3. Semantica dei loop (V5, V6, V8)

- **Perché.** Un anello di streaming senza Loop Start (si ripete tutto) con LSAX scritto prima del
  KON: sull'hardware LSAX resta quello scritto e la voce torna all'inizio (`:136-138`, `:163`); qui
  `spu.c:82-83` dimentica la scrittura e `spu_voice.c:217-223` spegne la voce al primo Loop End. La
  cutscene sente il primo giro e poi silenzio. V8 produce loop nel punto sbagliato.
- **Ripete / corre avanti.** Ripete (V8). Corre avanti: no.
- **Pseudo-soluzione** (`:131`, `:134-138`, `:163-173`):

  ```c
  /* KON: copia SSA nell'indirizzo corrente (:131); NON toccare repeat_address */
  /* voice_decode_block, blocco a A: */
  if (flags & 4) repeat_address = A >> 3;                  /* sempre, :134-135 */
  if (flags & 1) {                                         /* :136-138, :163 */
      next = repeat_address * 8;  endx_dopo_il_blocco = true;
      if (!(flags & 2)) { adsr_state = RELEASE; EnvelopeVol = 0; }  /* Code 1, :164, :171 */
  } else next = A + 16;
  /* la voce resta attiva per le letture; sparisce solo dal mix (V7, I2) */
  ```

  `loop_addr_set`, `ignore_loop` e `loop_addr` possono restare inutilizzati nella struttura, così
  `sizeof(Spu)` non cambia e i savestate restano caricabili.
- **Rischio.** Medio. `docs/study/SPU_2026-07-29.md` (finding 11) annota che un altro emulatore
  protegge una scrittura di LSAX fatta durante il primo blocco: la doc non descrive quella regola,
  quindi qui non si adotta, ma è il primo posto da guardare se un titolo perde il proprio loop.
- **Verifica.** Binding nuovo `emu.spu_voice(n)` (6.4) a ogni vblank: voce con `on` che cade a false
  pochi frame dopo il KON mentre `repeat_address` è valido = V5/V6 in azione. Ascolto con
  `ZS1_AUDIO_DUMP`.

### R4. Sweep del volume principale assente (V23, V24)

- **Perché.** Unico difetto che azzera tutto insieme (XA, CD-DA e voci), perché il volume principale
  sta dopo la somma (C10). Fade-in in sweep da un volume diretto a 0: scena muta. Fade-out in sweep:
  l'audio prosegue oltre il cambio scena.
- **Ripete / corre avanti.** Ripete: plausibile (coda non sfumata). Corre avanti: no.
- **Pseudo-soluzione** (`:405-406`, `:414-432`, `:447-481`): applicare a `main_vol_left/right`, per
  campione, la routine di `spu_voice_sweep_tick` (corretta per V20) con i contatori dello sweep. Per
  non cambiare `sizeof(Spu)` i due contatori possono vivere in `last_reverb_output[2]`
  (`include/spu.h:290`, oggi non letto da nessuno). MVOLX (`spu.c:228-229`) restituisce allora il
  livello in sweep.
- **Rischio.** Basso: il modo diretto non cambia.
- **Verifica.** Watchpoint su `0x1F801D80`/`0x1F801D82` (6.3): scritture con bit 15 = 1 all'ingresso
  della scena confermano il caso.

### R5. Il drive si ferma per qualsiasi INT pendente, anche per l'XA (X21)

- **Perché.** L'XA non genera INT1 (`:1132-1133`) e sull'hardware continua a 150 settori/s
  (`:759-762`). Qui un INT3 di un GetlocP di polling, un INT1 spurio di R1 o un INT1 dati riconosciuto
  tardi fermano anche l'XA. Una cutscene in-engine pesa sulla CPU molto più di un FMV.
- **Ripete / corre avanti.** No; buchi e ritardo (audio indietro, non avanti).
- **Pseudo-soluzione.** Modello completo: voce 8 di `docs/study/README.md` (il drive avanza sempre, i
  settori dati non presi in tempo si perdono, `:1953-1956`). Minimo per l'audio: in
  `cdrom_drive_event_tick`, con INT pendente, interrogare comunque il lettore asincrono e processare
  il settore se andrà all'ADPCM o va scartato (R1); trattenere solo la consegna dati.
- **Rischio.** Alto (struttura del drive; sessione dedicata).
- **Verifica.** `total_starved` (5o valore di `emu.cd_audio()`) per campo nella scena, più gli INT
  pendenti al momento dell'evento del drive (binding nuovo `emu.cd_state()`, 6.4).

### R6. ADPBUSY mai impostato (X18)

- **Perché.** Il bit dice se l'XA sta suonando (`:62`), e funziona anche in Mute (`:1020-1021`), nel
  paragrafo che cita Dino Crisis 1 (`:1023`). La doc non dice che Dino Crisis lo legga: è un'ipotesi.
- **Pseudo-soluzione.** In `cdrom_read8` caso 0: `if (xa_in_riproduzione) st |= 0x04;`, con
  l'approssimazione senza campi nuovi `audio_fifo.count > 0 && xa_adpcm_enable && drive_state ==
  DRIVE_READING`.
- **Rischio.** Basso. **Verifica.** `emu.add_read_watch(0x1F801800)` nella scena: letture di HSTS in
  un ciclo di attesa durante la battuta aprono il caso.

### R7. SPUCNT.14 silenzia anche il CD (C8)

- **Pseudo-soluzione** (`:631`): il bit 14 azzera solo voci e ingresso del riverbero dalle voci,
  prima di sommare il CD; volume principale dopo, come oggi.
- **Rischio.** Basso. **Verifica.** Watchpoint su `0x1F801DAA`: bit 14 = 0 mentre `popped` di
  `emu.cd_audio()` cresce = scena muta per C8.

### R8. AutoPause con `prev_track` statico (D6, D7), solo titoli con musica CD-DA

- **Pseudo-soluzione** (`:1100`, `:1103-1104`): niente `static`; in `begin_playing` impostare
  `last_subq = cdrom_disc_get_subq(&disc, current_lba)`, poi confrontare la traccia del settore
  corrente con `last_subq.track_bcd` prima di aggiornarla (`cdrom_commands.c:1057-1060`). Nessun
  campo nuovo.
- **Rischio.** Basso. **Verifica.** Log CDROM in debug (solo funzionale): INT4 entro uno o due
  settori da un Play = difetto.

### R9. FIFO di scrittura manuale scartata in modo Stop (I10)

- **Pseudo-soluzione** (`:683`, `:717-720`): buffer fino a 32 halfword riempito da 1F801DA8h in modo
  0, svuotato in SPU RAM da TSA quando SPUCNT passa al modo 1; in modo 1 scrittura diretta come oggi.
  Serve un campo nuovo in `Spu` (cambia `ZS1_STATE_VERSION`, oggi 11 in `src/core/savestate.c:42`).
- **Rischio.** Basso per l'audio, medio per i savestate. **Verifica.** Watchpoint su `0x1F801DA8`:
  scritture con SPUCNT in modo 0 = dati persi oggi.

### R10. Mute, ADPMUTE, ATV e volume CD 0 mai applicati (C3, C4, C5, C7)

- **Perché qui.** Non zittisce nulla: fa sentire ciò che il gioco ha zittito. Candidato diretto per
  "ripete ai cambi scena": il gioco abbassa il CD (Mute, ADPMUTE, ATV o AVOL a 0) e riposiziona; noi
  lasciamo udibile la coda della battuta precedente e i settori della transizione. Dino Crisis 1 usa
  Mute (`:1023`).
- **Pseudo-soluzione** (`:227-242`, `:251`, `:1019-1022`; SPU `:442`), in `spu_step`:

  ```c
  pop_o_tieni(&l, &r);                         /* come oggi, spu_mixing.c:507-516 */
  if (cd.muted || (cd.xa_mute && sorgente_xa)) l = r = 0;
  int32_t L = (l*cd.vol_ll + r*cd.vol_lr) >> 7;   /* ATV, :227-240 */
  int32_t R = (l*cd.vol_rl + r*cd.vol_rr) >> 7;
  L = sat16(L); R = sat16(R);                      /* saturazione fino al doppio */
  cd_audio_left  = (L * (int16_t)cd_vol_left)  >> 15;   /* niente "0 = pieno" */
  cd_audio_right = (R * (int16_t)cd_vol_right) >> 15;
  ```

  La capture CD (C11) va presa prima di ATV e volume (`:53-54`).
- **Rischio.** Medio: il ramo "0 = pieno" (`spu_mixing.c:520-521`) esiste per un percorso che lascia
  AVOL a 0; trovarlo prima di toglierlo. Da rifare: lettore CD del BIOS, Ace Combat 2.
- **Verifica.** Watchpoint su `0x1F801DB0`/`0x1F801DB2` e sul registro comandi `0x1F801801` (Mute 0Bh)
  nella transizione; A/B con `ZS1_SPU_NO_CDAUDIO=1`: se la ripetizione sparisce, viene dal CD.

### R11. Sweep lineare decrescente sotto zero (V20, V21, V22)

- **Perché qui.** Un fade-out di musica o ambiente in sweep al cambio scena sparisce e poi risale a
  volume pieno invertito: l'audio della scena precedente "ritorna".
- **Pseudo-soluzione** (`:451-453`, `:474-481`): `if (decrescente && !fase) v = max(v, 0); else if
  (decrescente && fase) v = clamp(v, -0x8000, 0); else v = clamp(v, -0x8000, 0x7FFF);` con passo
  invertito se `decrescente XOR fase`.
- **Rischio.** Basso. **Verifica.** `emu.spu_voice(n)`: `vol_left`/`vol_right` negativi su una voce
  in sweep con bit 12 = 0.

### R12. IRQ SPU spurii e flag riconosciuto da I_STAT (I4, I5): "corre avanti"

Dentro R2; tenuto separato perché è l'unico meccanismo nel perimetro audio che fa avanzare il
contenuto di uno stream più in fretta del dovuto.

### R13. Capture fuori dalla SPU RAM, nessun IRQ di capture (C11, I7)

Lip-sync e scene temporizzate sulla capture (`:837-844`, `:863`, `:865`): non zittisce l'XA, ma una
scena che attende l'IRQ di capture può bloccarsi. Pseudo-soluzione: scrivere CD-L, CD-R, voce 1 e
voce 3 in `ram[0x000/0x400/0x800/0xC00 + pos]` a passi di un halfword, prima del volume, con controllo
IRQ su ogni scrittura (RAM_CTRL vale sempre 0004h, condizione di `:844` soddisfatta). Rischio basso.

### Resto (basso impatto sul sintomo)

X5, X20, X24, X25, D2, D7, D8, V9, V12, V17, V25, I3, I8, I11, I14, I15, T4, T6.

---

## 5. Stato delle voci aperte nei documenti precedenti

### 5.1 `docs/study/README.md`, coda di lavoro

| # | Voce | Stato |
|---|---|---|
| 1-7 | (fatte il 2026-07-29) | confermate: maschere coding `cdrom_commands.c:868-870`; mBASE `spu.c:365`; SPUCNT.7 sulle scritture `spu_mixing.c:122-125`; 18900 7:3 `cdrom_audio.c:251-252`; seconda risposta ripianificata `cdrom.c:121-131`; sweep bit 13 `spu_voice.c:359` e livello preservato `spu.c:261-270`; Pause 1x/2x `include/cdrom.h:86-87` |
| 8 | Modello di ritmo del drive | ancora aperto (`cdrom.c:144-156`, `cdrom_commands.c:965-966`) |
| 9 | VSync timeout | fuori perimetro; `CLAUDE.md` lo dà risolto (costo dei load in RAM) |
| 10 | Audio host sotto il 100% | risolto con time-stretch lato consumatore (`spu_mixing.c:650-698`) |
| 11 | Capture nella SPU RAM | ancora aperto (`spu_mixing.c:70-79`, `:369-374`) |
| 12 | Tempi dei trasferimenti SPU | ancora aperto (`spu_dma.c:22`/`:39`) |
| 13 | XA filtrati consegnati come dati; nessun controllo Mode 2 | ancora aperto (`cdrom_commands.c:858-875`, `:934-999`) |
| 14 | Stato ADPCM salvato prima del clamp | risolto in `cdrom_audio.c:134-136` |
| 15 | Matrice CD mal decodificata e mai applicata | decodifica risolta (`cdrom.c:363`, `:371-372`, `:430-445`); applicazione ancora aperta (`cdrom.c:471-492` mai chiamata) |
| 16 | Mute e disabilitazione SPU silenziano il CD | disabilitazione risolta (`spu_mixing.c:360` non guarda il bit 15); mute ancora aperto (`spu_mixing.c:404-405`) |
| 17 | Sweep del volume principale | ancora aperto (`spu.c:335-347`) |
| 18 | VxPitch mascherato in scrittura | risolto in `spu.c:282` |
| 19 | IRQ SPU da voci inattive, offset dell'indirizzo | ancora aperto (`spu_voice.c:203-207`, `:244-247`) |
| 20 | CDDA non decimato a 2x; report a ogni settore | report risolto (`cdrom_commands.c:1072-1096`); decimazione aperta (`cdrom_audio.c:328-337`), ma la doc (`ps1/cdr/cdromdrive.md:1072-1073`) descrive comunque un avanti veloce |

### 5.2 `docs/study/SPU_2026-07-29.md`

| # | Voce | Stato |
|---|---|---|
| 1 | mBASE a 14 bit | risolto in `spu.c:365-366` |
| 2 | SPUCNT.7 sul lato sbagliato | risolto in `spu_mixing.c:122-125` |
| 3 | Capture fuori dalla SPU RAM | ancora aperto (`spu_mixing.c:70-79`, `:369-374`) |
| 4 | Direzione dello sweep | risolto in `spu_voice.c:359`, `:379` |
| 5 | Scrittura in sweep distrugge il livello | risolto in `spu.c:261-270`, `:335-347` |
| 6 | Sweep del volume principale | ancora aperto (`spu.c:335-347`) |
| 7 | VxPitch mascherato | risolto in `spu.c:282`; tetto in `spu_voice.c:287` |
| 8 | Mute e disabilitazione sul CD | come 5.1 riga 16 |
| 9 | IRQ da voci inattive, offset | ancora aperto (`spu_voice.c:203-207`, `:244-247`) |
| 10 | Trasferimenti istantanei | ancora aperto (`bus.c:1166-1204`, `spu_dma.c:16-40`) |
| 11 | Scrittura di LSAX disattiva sempre il Loop Start | ancora aperto (`spu.c:305-310`) |
| 12 | End+Mute in modo rumore | ancora aperto (`spu_voice.c:217-224` non guarda `noise_mode`) |
| 12 | Fattore di modulazione non limitato | risolto: `sval` saturato a 16 bit (`spu_voice.c:327-328`) prima di `:281` |
| 12 | Tabella di interpolazione | risolto: tabella hardware `spu_voice.c:57-122` |
| 12 | ADSR esponenziale crescente | equivalente alla doc (`spu_adsr.c:50-52` contro `:455-462`); "tutti 1 non avanza" ancora aperto |
| 12 | Stato morto | `reverb_ds_buf`/`reverb_us_buf`/`reverb_resample_pos` ora usati dal FIR (`spu_mixing.c:277-303`); `last_reverb_input`/`last_reverb_output` (`include/spu.h:289-290`) e `sinc_unused` (`:177`) ancora inutilizzati |

### 5.3 `docs/study/CDROM_XA_2026-07-29.md`

| # | Voce | Stato |
|---|---|---|
| 1 | Maschere coding info | risolto (`cdrom_commands.c:868-870`) |
| 2 | Rapporto 18900 | rapporto risolto (`cdrom_audio.c:251-252`); tabelle e ordine dei tap NON VERIFICATI (X14) |
| 3 | Seconda risposta scartata | risolto (`cdrom.c:121-131`); comando arrivato mentre occupato ancora scartato (`cdrom.c:336-339`) |
| 4 | Drive fermo con INT pendente; overrun impossibile | ancora aperto (`cdrom.c:144-156`; `cdrom_commands.c:965-966`); il riarmo usa la scadenza residua (`cdrom.c:114-119`, `cdrom_commands.c:986-988`) |
| 5 | Settori XA filtrati consegnati come dati; Mode 2 | ancora aperto (X5, X6) |
| 6 | Costanti di tempo | risolte: cambio di velocità (`cdrom_commands.c:413-415`), Pause (`include/cdrom.h:86-87`), Stop (`:79-80`), ack Init (`:42`), ReadTOC (`:99`); Reset senza INT2 (`cdrom_commands.c:655-661`). Jitter di seek e distanza minima tra interrupt assenti (effetto NON VERIFICATO) |
| 7 | Matrice CD | come 5.1 riga 15 |
| 8 | Stato ADPCM non saturato | risolto (`cdrom_audio.c:134-136`); filtro 4 escluso (`:98`) |
| 9 | Minori | decimazione CDDA aperta; report risolto; `prev_track` statico aperto (`cdrom_commands.c:1041`); IRQ senza maschera aperto (`bus_irq.c:46-62`); politica FIFO invariata (`cdrom_audio.h:21`) |

### 5.4 `docs/CDROM_AUDIT_2026-08-17.md`, Parte 7 e righe rilevanti

| # | Voce | Stato |
|---|---|---|
| 1 | Matrice ATV morta | porte e ADPCTL risolti (`cdrom.c:363`, `:371-372`, `:430-445`); applicazione ancora aperta (`cdrom.c:471-492` mai chiamata, `spu_mixing.c:510-511`) |
| 2 | Letture CDROM a 16/32 bit | risolto in `bus.c:339-356` |
| 3 | GetlocL/Pause in seek, BCD di Setloc | risolto in `cdrom_commands.c:453-466`, `:299-303`, `:217-223` |
| 4 | Init ripetuto risponde INT3 | risolto in `cdrom_commands.c:337-345` |
| 5 | Reset con INT2; Sync/17h/18h | risolto in `cdrom_commands.c:655-661`, `:197-199`, `:574-577` |
| 6 | Il buffer non è una coda | ancora aperto (`cdrom_commands.c:965-966`) |
| 7 | GetTD binario | risolto in `cdrom_commands.c:536-547` |
| 8 | SubQ pregap/lead-out, PREGAP nel CUE | non ricontrollato (fuori dal percorso audio) |
| 9 | Pacchetto report | risolto in `cdrom_commands.c:1072-1096` |
| 10 | SetSession, GetQ, MotorOn, Forward/Backward, GetID | ancora aperto: MotorOn `:273-279`, Forward/Backward `:245-262`, GetQ `:664-680` |
| 11 | Setmode bit 4 come dimensione | ancora aperto (`cdrom_commands.c:956-958`) |
| 12 | Tempi Stop e prima risposta | risolto (`include/cdrom.h:40-42`, `:78-80`; `cdrom.c:355-358`) |
| 13 | IRQ ignora HINTMSK | ancora aperto (`bus_irq.c:46-62`) |
| 14 | Sound map assente | ancora aperto (`cdrom.c:361-362`) |
| 15 | LibCrypt | risolto con SBI (`cdrom_commands.c:975-978`, `:1057-1060`) |
| 16 | Ricampionamento 18900 | ancora aperto (`cdrom_audio.c:167-215`) |
| riga 6.1 | ADPBUSY | ancora aperto (`cdrom.c:290-301`) |
| riga 6.1 | Ack che svuota la FIFO risultati; lettura oltre la risposta | ancora aperto (`cdrom.c:404-428`, `:303-304`) |
| riga C8a | Prima risposta di Pause con bit 5 | risolto in `cdrom_commands.c:304-309` |
| righe C9b/C9c | Mode 2 e consegna dati dei settori filtrati | ancora aperto (X5, X6) |
| riga C17 | Read dopo Pause | ancora aperto (`cdrom_commands.c:69-76`) |
| riga C26 | Mute | push CDDA in Mute risolto (`cdrom_audio.c:328-337`); applicazione del Mute aperta (C4) |
| riga C31 | AutoPause statico | ancora aperto (`cdrom_commands.c:1041`) |

### 5.5 `docs/DMA_IRQ_GTE_MDEC_AUDIT_2026-08-17.md`, Parte 3

| # | Voce | Stato |
|---|---|---|
| 1 | Scritture parziali di DMA e IRQ | risolto in `bus.c:279-293` e `bus.c:236-239` |
| 2 | DICR.31 con le abilitazioni per canale | risolto in `dma.c:112-113` |
| 3 | Il DMA azzera I_STAT.3 | risolto in `dma.c:124-130` |
| 4 | Bus error mai alzato | risolto in `dma.c:138-143`, chiamato da `bus.c:1126`, `:1170` |
| 5 | MVMVA non azzera FLAG | risolto in `gte_ops.c:146` |
| 6 | LZCR con LZCS = FFFFFFFFh | risolto in `gte.c:63-67` |
| 7 | Matrice spazzatura con RT21 | risolto in `gte_ops.c:151-161` |
| 8 | SWC2 non attende il GTE | ancora aperto (`cpu_instructions.c:993-1004`) |
| 9 | MDEC comandi 0/4-7 consumano parametri | ancora aperto (`mdec.c:442-446`) |
| 10 | Ritmo del DMA GPU | risolto in `bus.c:888-911` (A/B con `ZS1_DMA_GPU_PACE=legacy`) |
| 11 | MADR/BCR, restrizioni DMA6, mirror CHCR | ancora aperto (`dma.c:241-247`; nessun caso speciale per il canale 6) |
| 12 | DICR bit 0-6 | ancora aperto (`dma.c:215`, `:283`: memorizzati, mai usati) |
| 13 | Clip a 9 bit sul croma MDEC | probabilmente aperto (`mdec.c:198-200`), non approfondito |
| 14 | Interrupt su opcode cop2 rinviato | ancora aperto (`cpu_execution.c:44`) |
| 15-16 | Letture manuali MDEC, minori | non ricontrollati (fuori perimetro) |
| riga 2.3 | Ack IRQ9 dentro la scrittura di I_STAT (dato "OK") | DIFFORME rispetto alla doc SPU (I4): `ps1/spu/soundprocessingunitspu.md:635`, `:655` |
| riga 2.2 | Stallo CPU durante il DMA solo come riduzione di `downcount` | ancora così (`bus.c:1217-1227`); vedi T6 |

---

## 6. Piano di verifica

### 6.1 Strumenti che esistono davvero

- Variabili d'ambiente lette nel codice: `ZS1_AUDIO_DUMP` (`spu_mixing.c:432`, s16 stereo dell'uscita
  finale), `ZS1_XA_DUMP` (`cdrom_audio.c:305`, XA decodificato prima del ricampionamento),
  `ZS1_SPU_NO_REVERB` (`spu_mixing.c:383`), `ZS1_SPU_NO_CDAUDIO` (`spu_mixing.c:358`),
  `ZS1_SPU_NO_STRETCH` (`spu_mixing.c:646`), `ZS1_DMA_GPU_PACE` (`bus.c:896`), `ZS1_FRAME_PROFILE`,
  `ZS1_LUA_SCRIPT`, `ZS1_LOG_LEVEL`, `ZS1_LOG_STDERR`, `ZS1_RAM_LOAD_STALL`.
- API Lua (`src/core/lua_debug.c:747-795`): `emu.on_event` (eventi "vblank", "cdrom_int1",
  "dma_ch2_done", "mdec_macroblock" e altri), `emu.on_break`, `emu.add_write_watch`,
  `emu.add_read_watch`, `emu.resume`, `emu.disasm`, `emu.reg`, `emu.cycles`, `emu.irq`,
  `emu.cd_audio` (8 valori: count, pushed, popped, dropped, starved, SPUCNT, settori letti, settori XA;
  `:626-636`), `emu.spu_stats` (5 valori; `:426-436`), `emu.audio_stats` (9 valori; `:714-728`),
  `emu.reverb`, `emu.save_state`, `emu.load_state`, `emu.display_area`, `emu.log`. `emu.read_u16`
  legge solo RAM, BIOS e scratchpad (`peek_bytes`, `:92-115`): non legge i registri SPU o CDROM.
- I watchpoint confrontano l'indirizzo virtuale (`bus.c:670`, `:697`; `debugger.c:221-237`): servono
  sia `0x1F80xxxx` sia `0xBF80xxxx`. Al colpo la macchina si ferma e `emu.on_break` riceve
  "Write watchpoint 0x... (PC=0x...)"; il valore si ricava disassemblando lo store a PC e leggendo il
  registro sorgente con `emu.reg`, poi `emu.resume()`.
- Sonde esistenti utili: `scripts/audio_timeline.lua`, `scripts/audio_delivery_probe.lua`,
  `scripts/audio_underrun_probe.lua`, `scripts/clock_compare.lua`, `scripts/spu_rate.lua`,
  `scripts/spu_health.lua`, `scripts/spu_pop_capture.lua` (modello per caricare un savestate al
  primo vblank).
- Due sonde leggono male `emu.cd_audio()`: `scripts/audio_timeline.lua:23` salta "starved", quindi le
  colonne SPUCNT, settori e settori XA sono spostate di uno; `scripts/spu_pop_capture.lua:67` prende
  il terzo valore come "drop", ma è `total_popped`. Correggerle prima di usarle.

### 6.2 Savestate dentro la cutscene

1. Avvio normale fino a pochi secondi prima del trigger della cutscene, F5 (`src/main.c:757-759`,
   scrive `savestates/slot0.zst`, `include/savestate.h:30`).
2. Ogni sonda carica lo stato al primo vblank come fa `scripts/spu_pop_capture.lua`.
3. `ZS1_STATE_VERSION` è 11 (`src/core/savestate.c:42`) e la SPU è salvata come un blocco di
   dimensione controllata. Le correzioni senza campi nuovi (R1, R2, R3, R4 usando
   `last_reverb_output`, R6, R7, R8, R10, R11) lasciano lo stato caricabile; R9 e R13 (se aggiungono
   stato) lo invalidano: rifare il savestate dopo.
4. Misure di velocità solo con `ZS1_FRAME_PROFILE=1`, senza `ZS1_LOG_STDERR` e senza sonde per vblank
   (trappola documentata in `CLAUDE.md`).

### 6.3 Sonda nuova: classificazione del meccanismo (solo API esistente)

Primo passo, prima di toccare il codice: sapere quale dei meccanismi (a)-(d) usa la scena.

```lua
-- scripts/cutscene_audio_classify.lua (da scrivere)
local STATE = "savestates/slot0.zst"
local loaded, f, int1 = false, 0, 0
local prev, hits = {}, {}
local WATCH = {                       -- KUSEG e KSEG1
  [0x1F801DA4]="IRQA", [0xBF801DA4]="IRQA",
  [0x1F801D80]="MVOLL", [0xBF801D80]="MVOLL",
  [0x1F801D82]="MVOLR", [0xBF801D82]="MVOLR",
  [0x1F801DAA]="SPUCNT", [0xBF801DAA]="SPUCNT",
  [0x1F801DB0]="AVOLL", [0xBF801DB0]="AVOLL",
  [0x1F801D88]="KON0", [0xBF801D88]="KON0",
  [0x1F8010C8]="DMA4CHCR", [0xBF8010C8]="DMA4CHCR",
  [0x1F801801]="CDCMD", [0xBF801801]="CDCMD",   -- banco 0: comando
}
for a in pairs(WATCH) do emu.add_write_watch(a) end

emu.on_break(function(reason)
  local wp, pc = reason:match("0x(%x+).-PC=0x(%x+)")
  local name = WATCH[tonumber(wp,16)]
  if name then
    local ins = emu.disasm(tonumber(pc,16))           -- es. "sh      v0, 6564(at)"
    local rt  = ins:match("^%a+%s+([%w$]+),")
    local val = emu.reg(REGIDX[rt])                    -- REGIDX: nome -> indice, dai nomi REG[] di cpu_disasm.c
    hits[name] = (hits[name] or 0) + 1
    if name ~= "KON0" and name ~= "DMA4CHCR" then
      emu.log(string.format("[cls] f=%d %s <- %04x pc=%s", f, name, val & 0xFFFF, pc))
    end
  end
  emu.resume()
end)

emu.on_event(function(ev)
  if ev == "cdrom_int1" then int1 = int1 + 1 return end
  if ev ~= "vblank" then return end
  f = f + 1
  if not loaded then loaded = true; emu.load_state(STATE); return end
  if f % 25 ~= 0 then return end
  local cnt, push, pop, drop, starve, ctrl, sect, xas = emu.cd_audio()
  local gen, sdrop, used, size, keys = emu.spu_stats()
  local d = function(k, v) local x = v - (prev[k] or v); prev[k] = v; return x end
  emu.log(string.format(
    "[cls] f=%d sett=%d xa=%d int1=%d push=%d starve=%d cdq=%d SPUCNT=%04x KON=%d IRQA=%d DMA4=%d",
    f, d("s",sect), d("x",xas), d("i",int1), d("p",push), d("st",starve), cnt, ctrl,
    d("k",keys), d("q",hits.IRQA or 0), d("m",hits.DMA4CHCR or 0)))
end)
```

Lettura del risultato:
- `xa` > 0 e `push` > 0 durante la battuta: meccanismo (a). `int1` vicino a `sett - xa` in un tratto
  senza dati attesi: R1 in azione. Setloc (02h) e ReadS (1Bh) ripetuti sulla stessa battuta nel
  registro CDCMD: la "ripetizione".
- `xa` = 0, `KON` e `DMA4` regolari, IRQA riscritto a cadenza costante: meccanismo (c). IRQA che smette
  di essere riscritto dopo una o due volte: R2.
- MVOLL/MVOLR con bit 15 = 1 all'ingresso: R4. SPUCNT con bit 14 = 0 mentre `push` cresce: R7.
  AVOLL a 0 o comando 0Bh nella transizione: R10.
- `starve` in crescita durante la battuta con `xa` > 0: R5.

### 6.4 Binding nuovi da aggiungere a `lua_debug.c` (oggi non esistono)

Lettura diretta delle strutture, senza `spu_catch_up` né pop di FIFO:

- `emu.spu_voice(n)` -> `on`, `curr_addr`, `repeat_address`, `loop_addr_set`, `ignore_loop`,
  `adsr_state`, `EnvelopeVol`, `vol_left`, `vol_right`, `pitch`, bit di ENDX.
- `emu.spu_irq()` -> `irq_addr`, `irq9_flag`, `control`, `status`, `transfer_addr`,
  `transfer_addr_reg`, `main_vol_left_cur`, `main_vol_right_cur`.
- `emu.cd_state()` -> `drive_state`, `interrupt_flag`, `interrupt_enable`, `mode`, `xa_filter_file`,
  `xa_filter_channel`, `muted`, `xa_mute`, `vol_ll/lr/rl/rr`, `last_header[0..7]`, `head_lba`,
  `seek_phase`.

Con `emu.cd_state()` agganciato a "cdrom_int1": `last_header[6]` (submode) audio+RT (es. 64h) con il
filtro attivo indica un INT1 che l'hardware non avrebbe mai generato (prova diretta di R1).

### 6.5 Per "corre avanti alla scena"

Il clock SPU non può anticipare (T1, T2). Restano due famiglie:
1. Dentro l'audio: ricariche anticipate di uno stream SPU (R2/R12), visibili con la sonda 6.3 come
   IRQA riscritto più spesso di mezzo buffer per periodo di pitch.
2. Fuori dall'audio: scena più lenta del reale. Misura sull'asse dei campi emulati (`CLAUDE.md`): per
   la stessa cutscene, campi tra inizio e fine della battuta e flip di display per campo (cambi di
   `emu.display_area()` a ogni vblank), contro una run di riferimento contata in `Now in v-blank`.
   Meno flip per campo con la stessa durata audio spiegano il sintomo con il timing CPU/GPU. T6 (il
   DMA non fa passare tempo) spinge nella direzione opposta e va tenuto presente.

---

## 7. Ipotesi fuori perimetro, registrate per non perderle

- **LibCrypt su Dino Crisis.** Lo SBI aggiorna la SubQ solo nel ramo dati e CD-DA
  (`cdrom_commands.c:975-978`, `:1057-1060`). Nulla nella doc lega Dino Crisis a un sabotaggio
  dell'audio; è un motivo per verificare che spostare il blocco SubQ (R1) non cambi la sequenza
  GetlocP vista dalla protezione.
- **Scena più lenta per costo CPU.** `ZS1_RAM_LOAD_STALL` e i costi GTE governano quanti frame la scena
  disegna per campo; un CPI troppo alto in scene 3D pesanti rallenta una scena temporizzata sui frame
  disegnati e fa sembrare l'audio in anticipo. Misura: `ZS1_FRAME_PROFILE=1` nella cutscene.

---

## 8. NON VERIFICATO, e perché

- X8: il secondo tentativo di consegna dati con controllo di file/canale
  (`ps1/cdr/cdromdrive.md:606-608`) non è modellato; la doc lo descrive come un bug hardware senza
  dire chi ne dipenda.
- X14: le tabelle 18900 Hz non sono pubblicate (`ps1/cdr/cdromformat.md:930-935`).
- X15: la doc lascia aperto se un seek azzeri la storia ADPCM (`:849-854`).
- X16, X17: la doc descrive tre slot da 900h nel decoder (`ps1/cdr/cdromdrive.md:368-372`) ma non
  quanto audio decodificato sopravviva a Pause o Setloc.
- D4: `ps1/cdr/cdromdrive.md:1072-1073` descrive un "fast forward effect" senza dire come i campioni
  vengano scelti.
- V15: il generatore di rumore è scritto in un'altra forma; l'equivalenza con `:610-618` non è
  dimostrata qui.
- T3, T7: nessuna regola documentale su riallineamento dopo una pausa o ordine DMA/catch-up.
- Ogni legame con giochi specifici (Dino Crisis, Monsters & Co.) resta un'ipotesi finché la sonda 6.3
  non dice quale meccanismo usa la scena.