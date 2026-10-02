# Prove con BIOS e dischi per il lavoro del 2026-10-02

Questa guida è scritta per **Claude**, in una sessione sulla macchina che ha i BIOS, le immagini dei
dischi e le due GPU. Serve a verificare sui giochi veri le modifiche della pull request
[ZioZoni95/ZonistationOne#3](https://github.com/ZioZoni95/ZonistationOne/pull/3). Quelle modifiche
sono state provate solo con test automatici, mai con un BIOS o un disco.

- Il dettaglio di cosa è cambiato è nel blocco del 2026-10-02 di `CHANGELOG.md`.
- Il perché è in `docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md`; la sezione 8 elenca cosa resta da
  verificare, ed è da lì che vengono queste schede.

Ogni comando, variabile d'ambiente e riga di log citata qui è stata controllata nel codice del branch
o provata in una run. Se una riga attesa non compare, quello **è** un risultato: annotalo, non
cercare di farla comparire.

## Regole

1. **Mai dalla radice del repository.** L'emulatore scrive nella cartella corrente:
   - `memcard1.mcd` e `memcard2.mcd`, e li riscrive a ogni avvio (il test di scrittura del driver
     della memory card);
   - `imgui.ini`, `logs/` e `savestates/`.

   Lancia ogni prova dalla sua cartella sotto `$PROVE`. Non toccare le memory card vere. Se una
   prova ha bisogno di un salvataggio di gioco, chiedi all'umano dove sono le sue memory card e
   **copiale** nella cartella della prova.
2. **Dove sono i log.**
   - Le righe dell'emulatore stanno in `logs/<Categoria>.log`: `System`, `SPU`, `CDROM`, `GPU`,
     `Renderer`, `MDEC`, `DMA`, `Interconnect`, `BIOS` e le altre.
   - Il TTY del BIOS e del gioco va in `logs/BIOS.log`.
   - Quello che stampano gli script Lua va in `logs/Lua.log`.
   - Ogni riga porta l'orologio della macchina emulata: `[f1397   t  27.4809]` è il numero di campo
     video e i secondi emulati.
3. **Le run a tempo finiscono con `timeout`.** Il SIGTERM viene gestito: i log vengono svuotati su
   disco prima dell'uscita. Il codice di uscita 124 è quello normale.
4. **Velocità.** Una misura di velocità non vale niente se nella run c'era una di queste cose:
   - `ZS1_LOG_STDERR`;
   - `ZS1_LOG_LEVEL` sopra `info`;
   - uno script Lua;
   - un breakpoint.

   Prendi sempre la mediana di tre run. Vedi le trappole in `CLAUDE.md`.
5. **Controlla la GPU prima di giudicare un difetto grafico.** In `logs/System.log`:
   - su OpenGL, la riga `GL driver in use: ...`;
   - su Vulkan, `[VK] backend initialised on ...` in `logs/Renderer.log`.

   Se compare `ZS1_GPU=... was requested but the context is on ...`, la richiesta non è stata
   rispettata, e lo scrivi nel report.
6. **Dischi e BIOS.**
   - Un disco PAL va con il BIOS PAL.
   - `--game=` vuole il `.bin` (o il `.bin.ecm`), non il `.cue`.
   - Dino Crisis è LibCrypt: serve il suo `.sbi`, della stessa stampa (SLES-02207 per l'inglese),
     accanto all'immagine oppure indicato con `ZS1_SBI`.
7. **Non correggere niente durante le prove.** Se trovi un difetto, annota la scheda, i comandi e le
   righe di log. La correzione è un lavoro separato, da fare dopo il report.
8. **Le parti marcate _Serve l'umano_ non le puoi fare tu**: guardare, ascoltare, giocare fino a un
   punto preciso. Chiedi all'umano esattamente quello che c'è scritto, con il comando già pronto da
   copiare.
   - Il comando che dai all'umano deve avere i percorsi già espansi: il suo terminale non conosce
     `$BIN` e le altre variabili.
   - Una run interattiva che lanci tu va in background (`... > /dev/null 2>&1 &`), così puoi
     lavorare mentre l'umano gioca.
9. **Variabili e funzioni non restano da un comando all'altro** nella tua shell. Per questo stanno in
   `~/zs1-prove/env.sh` (scheda 0), e ogni blocco di comandi qui sotto comincia con
   `source ~/zs1-prove/env.sh`.
10. **Cicli lunghi.** I cicli delle schede 2, 9 e 10 durano circa 18 minuti ciascuno, più del limite
    per singolo comando della tua shell (in Claude Code 10 minuti). Lanciali in background e
    aspetta la notifica di fine, oppure spezzali in una run per comando.

## Difetti noti che NON vengono da questa PR

Non riportarli come regressioni, a meno che peggiorino rispetto al binario di partenza.

- La posizione della finestra video (X1/Y1 di GP1(06h)/(07h)) è ignorata: gli scuotimenti dello
  schermo non si vedono.
- Il ritaglio dell'overscan c'è solo in NTSC.
- Lo stato del display viene preso a fine campo: dopo un FMV può comparire un fotogramma stirato
  (15 bit letti come 24).
- I "pop" della SPU durante il parlato sono aperti, con causa non trovata.
- Monsters & Co.: i cinque difetti misurati in `CLAUDE.md` (troppi comandi CD, 2,8 s di nero al posto
  dell'FMV Disney, settori al secondo, `CLUT out of VRAM bounds`).
- Gli artefatti sulla iGPU Intel: la correzione è nel codice ma non è mai stata verificata.
- Non esistono il cambio disco a caldo e il reset.

---

## Scheda 0. Preparazione (una volta)

**Variabili e funzioni**, in un file da caricare all'inizio di ogni comando. I percorsi dei BIOS e
dei dischi chiedili all'umano, oppure cercali in `roms/` e `games/` del repository. Servono percorsi
assoluti.

```bash
mkdir -p ~/zs1-prove && cat > ~/zs1-prove/env.sh <<'EOF'
export REPO=$HOME/ZonistationOne                    # dove sta il repository
export BIN=$REPO/ZoniStation_One                    # binario della PR
export BASE=$(dirname "$REPO")/zs1-base/ZoniStation_One   # binario di partenza, per i confronti
export PROVE=$HOME/zs1-prove                        # una sottocartella per ogni prova
export BIOS_PAL=...  BIOS_NTSC=...
export AC2=...       # Ace Combat 2 (Europe), .bin
export DINO=...      # Dino Crisis (E) SLES-02207, .bin.ecm con il .sbi accanto
export CRASH3=...    # Crash Bandicoot 3 (E), .bin.ecm
export MONSTERS=...  # Monsters & Co. (Italy), .bin
prova() { mkdir -p "$PROVE/$1" && cd "$PROVE/$1"; }  # una prova = una cartella
EOF
```

Sostituisci i `...` con i percorsi veri prima di andare avanti.

**Binario della PR.** Se la PR è già stata unita, basta `stable_branch`. Altrimenti:

```bash
source ~/zs1-prove/env.sh
cd "$REPO" && git fetch origin pull/3/head:pr-3 && git checkout pr-3
make && ls -l --time-style=+%F_%T "$BIN"   # la data deve essere di adesso
```

**Binario di partenza** (commit `bbc1c7e`, la base della PR), in una cartella separata. Il suo
Makefile ha una corsa critica sugli header degli shader in compilazione parallela; il ciclo `for` la
aggira:

```bash
source ~/zs1-prove/env.sh
cd "$REPO" && git worktree add ../zs1-base bbc1c7e
cd ../zs1-base
for s in src/gpu/shaders/*.vert src/gpu/shaders/*.frag; do make -j1 "$s.spv.h"; done
make && ls -l "$BASE"
```

**Sessione grafica.** Guarda `echo $XDG_SESSION_TYPE`. Su questa macchina OpenGL vuole X11, e
Vulkan sulla iGPU Intel vuole Wayland (`CLAUDE.md`). Annota la sessione in ogni report.

---

## Scheda 1. Test automatici, e il test GPU sulle GPU vere

**Scopo.** Ripetere qui i test che la PR ha già superato altrove, e far girare sulle GPU vere
l'unico controllo che i rasterizzatori software non possono far fallire.

**Esecuzione.** Per `make hwtest` servono `gcc-mipsel-linux-gnu`, `xvfb` e `mesa-vulkan-drivers`.
Se mancano, chiedi all'umano di installarli.

```bash
source ~/zs1-prove/env.sh
cd "$REPO" && make test && make hwtest
```

**Esito atteso.**
- `all 9 unit tests passed`.
- Per ogni suite (`gpu`, `mdec`, `dma`, `spu`) e per ogni backend: `HWTEST <suite> DONE pass=4 fail=0`.

**Poi, sulle GPU vere** (finestra reale, senza Xvfb). Il controllo
`mask_test_between_overlapping_primitives` passa sempre su Mesa software ed esiste per le GPU vere:

```bash
source ~/zs1-prove/env.sh
cd "$REPO/tests/hw/build"
for gpu in nvidia intel; do for gfx in gl vulkan; do
  ZS1_GPU=$gpu ZS1_GFX=$gfx timeout 15 "$BIN" zero_bios.bin --exe=gpu.exe
  echo "== $gpu $gfx"
  grep -h -E 'GL driver in use|\[VK\] backend initialised|was requested but' logs/System.log logs/Renderer.log
  grep -h -o 'HWTEST gpu .*' logs/BIOS.log
done; done
```

**Esito atteso.** Quattro `PASS` e `DONE pass=4 fail=0` per ogni combinazione che parte.

**Se fallisce.**
- Una combinazione che non parte va annotata, non è un fallimento del test. Su Wayland OpenGL non
  parte; su X11 Vulkan su Intel potrebbe non partire.
- Un `FAIL` del controllo maschera su una GPU vera vuol dire che l'isolamento delle primitive
  (`src/gpu/renderer_isolate.h`) non basta su quel driver.
- Riporta la riga `FAIL got=... want=...`.

---

## Scheda 2. Avvio dei dischi (controllo di fumo)

**Scopo.** Accorgersi subito di una regressione grossa, prima delle prove mirate.

**Serve l'umano**: guardare che ogni disco arrivi dove deve.

**Esecuzione.** Ogni run dura 120 s, prima con il binario di partenza e poi con quello della PR:

```bash
source ~/zs1-prove/env.sh
for bin in BASE BIN; do
  for d in AC2 CRASH3 MONSTERS; do
    prova 02-avvio/$d-$bin
    timeout 120 "${!bin}" "$BIOS_PAL" --game="${!d}"
  done
  prova 02-avvio/DINO-$bin
  timeout 120 "${!bin}" "$BIOS_PAL" --game="$DINO"
  prova 02-avvio/BIOS-NTSC-$bin
  timeout 60 "${!bin}" "$BIOS_NTSC"
done
```

**Cosa chiedere all'umano**, per ogni run:
- AC2 deve arrivare all'FMV introduttivo e al titolo;
- Crash 3 al titolo;
- Monsters & Co. ai filmati e al titolo;
- Dino Crisis al menu principale;
- il BIOS NTSC al menu.

Chiedi anche se l'audio c'è.

**Cosa controlli tu.** Confronta gli errori tra le due run dello stesso disco:

```bash
source ~/zs1-prove/env.sh
cd "$PROVE/02-avvio"
for d in AC2 CRASH3 MONSTERS DINO; do
  echo "== $d"
  diff <(grep -h -E '^\[(ERROR|WARN ?)\]' $d-BASE/logs/*.log | sed -E 's/\[f[^]]*\]//; s/ +\(x[0-9]+ repeats\)//' | sort -u) \
       <(grep -h -E '^\[(ERROR|WARN ?)\]' $d-BIN/logs/*.log  | sed -E 's/\[f[^]]*\]//; s/ +\(x[0-9]+ repeats\)//' | sort -u)
done
```

**Esito atteso.**
- Ogni disco arriva allo stesso punto con entrambi i binari.
- Nessun `ERROR` nuovo con il binario della PR. I `WARN` nuovi vanno letti uno per uno e
  riportati.

---

## Scheda 3. Volume CD a 0 = silenzio

**Cosa è cambiato.** Il volume d'ingresso CD della SPU (AVOLL/AVOLR, 1F801DB0h/DB2h) a 0 ora vuol
dire silenzio, come da documentazione. Prima un valore 0 suonava a volume pieno. Un gioco che
lasciava AVOL a 0 contando sul vecchio comportamento ora resterebbe muto nell'audio XA e CD-DA.

**Serve l'umano**: ascoltare.

**Esecuzione.**

```bash
source ~/zs1-prove/env.sh
prova 03-volume-cd/AC2 && timeout 90 "$BIN" "$BIOS_PAL" --game="$AC2"        # FMV introduttivo
grep -h 'CD audio is arriving with the CD input volume' logs/SPU.log
prova 03-volume-cd/DINO && timeout 120 "$BIN" "$BIOS_PAL" --game="$DINO"     # schermate iniziali
grep -h 'CD audio is arriving with the CD input volume' logs/SPU.log
```

Facoltativo: il lettore CD del BIOS. Si apre solo con un CD **audio** nel lettore, perché un disco
di gioco viene avviato; l'emulatore non ha il cambio disco a caldo. Serve quindi l'immagine di un CD
audio, lanciata con `--game=`. Che l'emulatore carichi un CD solo audio non è stato verificato:
se non parte, annotalo e salta questa parte.

**Come leggerlo.**

| Riga nel log | Audio dell'FMV / del parlato | Significato |
|---|---|---|
| assente | presente | va bene: il gioco scrive AVOL prima che arrivi audio CD |
| presente | presente | va bene: AVOL era 0 solo all'inizio, poi il gioco l'ha scritto |
| presente | **assente**, mentre con `$BASE` c'è | **KO**: la regola "0 = silenzio" toglie audio a questo gioco |

**Se è KO.**
1. Ripeti la run con la sonda `scripts/cutscene_audio_classify.lua` (scheda 5), con
   `ZS1_CLASSIFY_STATE=` vuoto per partire dall'avvio.
2. Le righe `[cls] ... AVOLL ...` in `logs/Lua.log` dicono se il gioco scrive mai AVOL.
3. Riporta le righe e il punto del gioco.

---

## Scheda 4. Loop della SPU

**Cosa è cambiato.**
- Key On non azzera più l'indirizzo di ripetizione di una voce.
- Una scrittura di LSAX (indirizzo di ripetizione) dopo il primo blocco della voce viene tenuta
  ("latch"). Questa regola viene dall'emulatore di riferimento.
- Nessuna delle due cose è scritta nella documentazione. Se un gioco perde un loop o un campione si
  ripete male, la causa sospetta è questa. `make hwtest` (suite `spu`) verifica solo il caso
  documentato.

**Serve l'umano**: ascoltare la musica e gli effetti sostenuti negli stessi punti, con i due binari.
Non c'è un interruttore per tornare al comportamento vecchio: il confronto è con `$BASE`.

**Sintomi da cercare:**
- una nota tenuta che si spegne prima del tempo;
- un campione che ripete il suo attacco ("balbetta");
- un clic regolare a ogni giro del loop;
- un loop che riparte dal punto sbagliato (si sente un pezzo d'attacco in mezzo a una nota tenuta).

**Esecuzione oggettiva (tua).** Registra 90 s di uscita audio dall'avvio con entrambi i binari:

```bash
source ~/zs1-prove/env.sh
for bin in BASE BIN; do for d in AC2 CRASH3; do
  prova 04-loop/$d-$bin
  ZS1_AUDIO_DUMP=$PWD/uscita.raw timeout 90 "${!bin}" "$BIOS_PAL" --game="${!d}"
done; done
```

Il dump è PCM grezzo: 16 bit con segno, stereo, 44100 Hz. Analizzalo con lo script del repository.
Usa sempre `$BIN` per l'analisi, anche sui dump di `$BASE`: nel binario di partenza Lua non ha la
libreria `io` e lo script non parte.

```bash
source ~/zs1-prove/env.sh
for f in "$PROVE"/04-loop/*/uscita.raw; do
  prova 04-loop/analisi
  ZS1_AUDIO_RAW=$f ZS1_AUDIO_SKIP=10 ZS1_AUDIO_SECS=80 \
    ZS1_LUA_SCRIPT=$REPO/scripts/audio_raw_analyse.lua timeout 15 "$BIN" "$BIOS_PAL"
  echo "== $f"; grep -h '^\[raw\]' logs/Lua.log
done
```

**Esito atteso.**
- Il numero di salti (`adjacent-sample steps >= 6000`) e il silenzio più lungo (`longest run of
  digital silence`) del binario della PR non peggiorano rispetto a quelli di `$BASE`.
- L'umano non sente nessuno dei sintomi sopra in punti dove `$BASE` era pulito.
- Per ascoltarli, i dump si convertono con
  `ffmpeg -f s16le -ar 44100 -ac 2 -i uscita.raw uscita.wav`.

**Se è KO.** L'umano fa F5 un attimo prima del punto, e il savestate va nel report: la prossima
sessione indagherà da lì con `emu.spu_voice(n)`. Il campo `repeat_addr` è l'indirizzo di ripetizione
della voce.

---

## Scheda 5. Cinematiche 3D di Dino Crisis

**Il difetto riportato** (2026-08-21). Nelle cinematiche in-engine l'audio si ripete a certi cambi di
scena e va avanti rispetto all'immagine; negli FMV no. La PR corregge i candidati documentati:
- instradamento dei settori XA;
- XA che continua anche con un INT in sospeso;
- IRQ e loop della SPU;
- sweep di volume;
- Mute/ATV/AVOL.

Nessuno di questi è stato provato sul disco.

**Serve l'umano**, due volte:
1. Arrivare a pochi secondi da una cinematica in cui il difetto si sentiva, premere **F5** e
   chiudere la finestra. Il gioco glielo lanci tu, con il binario della PR e in questa cartella:
   ```bash
   source ~/zs1-prove/env.sh
   prova 05-dino && "$BIN" "$BIOS_PAL" --game="$DINO" > /dev/null 2>&1 &
   ```
   F5 scrive `savestates/slot0.zst`. I savestate vecchi (versione 11) non valgono più: va fatto
   nuovo.
2. Più avanti, ascoltare la scena.

**Esecuzione (tua).** Prima la sonda, che carica da sola il savestate al primo campo:

```bash
source ~/zs1-prove/env.sh
cd "$PROVE/05-dino"
ZS1_LUA_SCRIPT=$REPO/scripts/cutscene_audio_classify.lua timeout 60 "$BIN" "$BIOS_PAL" --game="$DINO"
grep -h '\[cls\]' logs/Lua.log
grep -h '\[STATE\]' logs/System.log      # deve esserci "Loaded savestates/slot0.zst"
```

Ogni 25 campi la sonda scrive una riga `[cls] f=... sect=... xa=... int1_audio=... push=...
starve=...`, più una riga per ogni registro che cambia. Come leggerla (è anche in fondo allo
script):

| Osservazione | Meccanismo / difetto |
|---|---|
| `xa > 0` e `push > 0` nella riga | audio XA dal CD |
| `int1_audio > 0` | difetto A1 ancora presente: **deve essere 0** dopo la PR |
| `xa = 0`, `DMA4` e `KON` regolari, `IRQA_w` costante | streaming ADPCM della SPU |
| `IRQA_w` si ferma dopo uno o due giri | streaming con il difetto A2 |
| `MVOLL`/`MVOLR` con bit 15 a 1 all'inizio scena | dissolvenza con sweep (A4) |
| `SPUCNT` bit 14 a 0 mentre `push` cresce | A7 |
| `AVOLL` a `0000`, `CDMUTE 1` o `ADPMUTE 1` al taglio | A10 |
| `starve` cresce in righe con `xa > 0` | A5 |

Poi l'ascolto, in due passaggi: prima con il binario della PR, poi con `ZS1_CD_XA_HOLD=1`, che
rimette il comportamento vecchio dell'XA sotto INT in sospeso. Ogni volta lanci tu il gioco in
background, e l'umano preme **F8** per caricare lo stato, ascolta la scena e chiude la finestra.

```bash
source ~/zs1-prove/env.sh
cd "$PROVE/05-dino" && "$BIN" "$BIOS_PAL" --game="$DINO" > /dev/null 2>&1 &
```

Quando l'umano ha chiuso la prima finestra:

```bash
source ~/zs1-prove/env.sh
cd "$PROVE/05-dino" && ZS1_CD_XA_HOLD=1 "$BIN" "$BIOS_PAL" --game="$DINO" > /dev/null 2>&1 &
sleep 5; grep -h 'ZS1_CD_XA_HOLD=1' logs/CDROM.log   # conferma che l'interruttore è attivo
```

Facoltativo: aggiungi `ZS1_AUDIO_DUMP=$PWD/scena.raw` e analizza il dump come nella scheda 4.

**Esito atteso.**
- `int1_audio` sempre 0.
- L'umano non sente più l'audio ripetuto al cambio di scena.
- Il ritardo tra audio e immagine **non va misurato** con la sonda caricata (regola 4). Si giudica a
  orecchio e si riporta come impressione.

**Da riportare.**
- Le righe `[cls]` della finestra della scena.
- Quale meccanismo dice la tabella.
- Il giudizio dell'umano con e senza `ZS1_CD_XA_HOLD=1`.

---

## Scheda 6. FMV e grafica

**Cosa è cambiato.**
- MDEC:
  - il reset conserva le tabelle;
  - i comandi 0 e 4..7 non consumano parametri;
  - YUV viene tagliato dopo la somma.
- VRAM:
  - i riempimenti arrivano al renderer come upload;
  - i trasferimenti avvolgono ai bordi;
  - la rilettura avviene solo dove il rasterizzatore può aver disegnato.
- Il DMA aggiorna MADR/BCR.
- Le primitive che leggono il proprio disegno vengono isolate.

**Serve l'umano**: guardare gli FMV:
- l'intro di AC2;
- i filmati iniziali di Monsters & Co. (incluso quello Disney);
- un FMV di Crash 3;
- un FMV di Dino Crisis.

**Esecuzione.** Per ogni disco una run con `$BASE` e una con `$BIN`, guardate dall'umano. In più
catturi tu un fotogramma allo stesso numero di frame con i due binari, per allegarlo al report:

```bash
source ~/zs1-prove/env.sh
for bin in BASE BIN; do
  prova 06-fmv/AC2-$bin
  ZS1_DUMP_FRAME=$PWD/f900.raw ZS1_DUMP_FRAME_N=900 timeout 45 "${!bin}" "$BIOS_PAL" --game="$AC2"
  ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgb24 -video_size 1024x512 \
         -i f900.raw f900.png
done
```

- Il dump è RGB a 24 bit, 1024x512: l'immagine visualizzata sta in alto a sinistra.
- `ZS1_DUMP_VRAM=1` cattura tutta la VRAM invece dello schermo.
- I due binari possono non trovarsi nello stesso istante al frame 900: confronta il contenuto, non i
  pixel.

**Interruttori A/B,** da usare solo se c'è un difetto:
- `ZS1_FORCE_READBACK=1`: rilegge la VRAM a ogni GP0(80h)/(C0h). Se il difetto **sparisce** così, la
  mappa delle tile sporche ha perso un'area: è un bug della PR, riportalo con il numero di frame.
  Conferma in `logs/GPU.log`: `ZS1_FORCE_READBACK=1: every GP0(80h)/(C0h) reads VRAM back`.
- `ZS1_MDEC_WRAP9=1`: l'avvolgimento a 9 bit letterale. Cambia solo i colori molto saturi degli FMV.
  Se così l'immagine è più fedele, riportalo: decide quale delle due varianti resta.

**Esito atteso.** Nessun FMV peggiore che con `$BASE`. Le differenze attese dalla PR sono colori
saturi più corretti e niente spazzatura ai bordi della VRAM.

---

## Scheda 7. Savestate

**Cosa è cambiato.**
- La versione del formato è 12.
- Un salvataggio rilegge dal renderer la VRAM disegnata, così i pixel che esistono solo sulla GPU
  sopravvivono al caricamento.
- Un upload a metà viene conservato.

**Serve l'umano**: giocare.

**Esecuzione.**
1. Tu prepari la cartella e lanci l'emulatore in background. Se l'umano vuole partire da un suo
   salvataggio di AC2, copia prima le sue memory card nella cartella.
   ```bash
   source ~/zs1-prove/env.sh
   prova 07-savestate && "$BIN" "$BIOS_PAL" --game="$AC2" > /dev/null 2>&1 &
   ```
2. L'umano entra in una missione e preme **F5** in un punto con molta grafica 3D, ricordandosi cosa
   c'è a schermo. Poi te lo dice.
3. Tu registri le impronte delle memory card:
   ```bash
   source ~/zs1-prove/env.sh
   cd "$PROVE/07-savestate" && sha256sum memcard*.mcd > prima.txt
   ```
4. L'umano gioca altri 10 s senza salvare sulla memory card, preme **F8**, confronta l'immagine con
   quella del momento di F5, ascolta che l'audio riparta e chiude la finestra.
5. Tu controlli:
   ```bash
   source ~/zs1-prove/env.sh
   cd "$PROVE/07-savestate"
   sha256sum memcard*.mcd | diff prima.txt -      # deve restare vuoto
   grep -h '\[STATE\]' logs/System.log
   ```
   Le righe `[STATE] Saved savestates/slot0.zst (... PC=0x..., cycle=...)` e
   `[STATE] Loaded savestates/slot0.zst (PC=0x..., cycle=...)` devono avere lo stesso PC e lo stesso
   ciclo.
6. Lo stato vecchio. Se l'umano ha un savestate della versione 11, copialo qui come `vecchio.zst` e
   caricalo con la sonda della scheda 5:
   ```bash
   source ~/zs1-prove/env.sh
   cd "$PROVE/07-savestate"
   ZS1_CLASSIFY_STATE=vecchio.zst ZS1_LUA_SCRIPT=$REPO/scripts/cutscene_audio_classify.lua \
     timeout 30 "$BIN" "$BIOS_PAL" --game="$AC2"
   grep -h '\[STATE\]' logs/System.log
   ```

**Esito atteso.**
- Dopo F8 l'immagine è identica a quella del momento di F5, compresi cielo, terreno e HUD, e l'audio
  riparte.
- Le memory card non cambiano.
- Lo stato vecchio viene rifiutato con `[STATE] vecchio.zst is not a v12 ZoniStation state` e la
  macchina continua senza bloccarsi.
- `[STATE] VRAM readback refused` non deve comparire: se compare, il salvataggio ha perso i pixel
  della GPU.

---

## Scheda 8. Cambio di renderer a caldo

**Cosa è cambiato.** Se la rilettura della VRAM prima del cambio viene rifiutata, il nuovo renderer
riparte dalla copia lato CPU invece che da una VRAM vuota.

**Serve l'umano**: guardare.

**Esecuzione** in una sessione X11, sulla NVIDIA, 4 minuti con un cambio ogni 250 campi:

```bash
source ~/zs1-prove/env.sh
prova 08-cambio && ZS1_GPU=nvidia ZS1_GFX_SWITCH_TEST=250 timeout 240 "$BIN" "$BIOS_PAL" --game="$AC2"
grep -h -E 'Renderer switch (stress|done|rolled back)|VRAM readback refused before the switch|No renderer left' logs/System.log
```

**Esito atteso.**
- Un `Renderer switch stress: every 250 fields`.
- Solo righe `Renderer switch done`, seguite dal renderer attivo, e nessun `rolled back`.
- Nessun `VRAM readback refused before the switch` e nessun crash.
- L'umano non vede schermo nero o spazzatura dopo i cambi.
- Su Wayland il passaggio a OpenGL si annulla per un limite noto dell'ambiente, non per un difetto:
  annotalo e basta.

---

## Scheda 9. Prestazioni e vsync

**Cosa è cambiato.**
- Il ciclo principale emula il campo successivo mentre il thread GPU finisce il precedente.
- Gli upload GL non servono più quando c'è la texture barrier.
- Il batching Vulkan.
- Il percorso veloce per le letture RAM della CPU.

`ZS1_VSYNC=0|1|-1` è nuovo: senza la variabile resta il default del driver.

**Esecuzione (tua, senza umano).** Il disco è AC2 dall'avvio, 60 s per run, tre run per
configurazione, con la finestra a schermo e senza log extra né script.

```bash
source ~/zs1-prove/env.sh
for gpu in nvidia intel; do for cfg in BASE BIN BIN-vsync0; do for n in 1 2 3; do
  prova 09-prestazioni/$gpu-$cfg-$n
  bin=${cfg%-vsync0}; extra=""; [ "$cfg" = BIN-vsync0 ] && extra="ZS1_VSYNC=0"
  env ZS1_GPU=$gpu ZS1_FRAME_PROFILE=1 $extra timeout 60 "${!bin}" "$BIOS_PAL" --game="$AC2"
done; done; done
```

Ogni 60 frame `logs/System.log` riceve una riga `[PROF] per frame: emu=... total=... | CPI=...`.
Solo il binario della PR aggiunge `wait=... frame=...`. Mediane, escludendo i primi 600 campi:

```bash
source ~/zs1-prove/env.sh
mediane() { awk -v salta=600 '/\[PROF\]/ { match($0, /\[f *[0-9]+/); f = substr($0, RSTART + 2, RLENGTH - 2) + 0;
    if (f < salta) next; for (i = 1; i <= NF; i++) if ($i ~ /^(emu|total|wait|frame)=/) {
    split($i, kv, "="); sub(/ms$/, "", kv[2]); print kv[1], kv[2] } }' "$1" | LC_ALL=C sort -k1,1 -k2,2n |
  awk '{ v[$1] = v[$1] " " $2; n[$1]++ } END { for (k in v) { split(substr(v[k], 2), a, " ");
    printf "%-6s %s ms (%d righe)\n", k, a[int((n[k] + 1) / 2)], n[k] } }'; }
for d in "$PROVE"/09-prestazioni/*; do echo "== $d"; mediane "$d/logs/System.log"; done
```

**Esito atteso**, tenendo per ogni configurazione la mediana delle tre run:
- `emu` della PR non peggiore di quella di `$BASE` oltre il 5%.
- `frame` vicino a 20 ms: AC2 è PAL, 50 campi al secondo.
- `wait` piccolo. Se con il vsync del driver `wait` è grande e `frame` oscilla, e con
  `ZS1_VSYNC=0` no, il vsync va lasciato spento di default: riportalo.
- Riporta anche il `CPI`. Le ottimizzazioni dell'host non lo cambiano, ma la PR corregge anche il
  comportamento emulato in alcuni punti, quindi può spostarsi di poco. Se tra `$BASE` e PR differisce
  di più dell'1%, segnalalo: la macchina emulata sta eseguendo codice diverso.

---

## Scheda 10. Tempi DMA documentati (`ZS1_DMA_STALL=doc`)

**Cosa è.** Un modello di costo del DMA preso dalla documentazione, spento di default perché cambia
il tempo emulato. Si accende solo se avvicina le pietre miliari di avvio alla run di riferimento.

**Esecuzione (tua).** Per ogni disco tre run da 120 s: `$BASE`, `$BIN`, `$BIN` con
`ZS1_DMA_STALL=doc`.

```bash
source ~/zs1-prove/env.sh
for d in AC2 CRASH3 MONSTERS; do
  prova 10-dma/$d-base && timeout 120 "$BASE" "$BIOS_PAL" --game="${!d}"
  prova 10-dma/$d-pr   && timeout 120 "$BIN"  "$BIOS_PAL" --game="${!d}"
  prova 10-dma/$d-doc  && ZS1_DMA_STALL=doc timeout 120 "$BIN" "$BIOS_PAL" --game="${!d}"
  grep -h 'ZS1_DMA_STALL=doc' logs/Interconnect.log   # conferma che era attivo
done
```

Le pietre miliari sono le righe del TTY in `logs/BIOS.log`, con il campo in cui compaiono:

```bash
source ~/zs1-prove/env.sh
tappe() { sed -n 's/^\[INFO \] \[f *\([0-9]*\) *t *[0-9.]*\] \(.*\)$/\1\t\2/p' "$1/logs/BIOS.log" |
          grep -v -P '\t\[BIOS\]'; }
confronta() { awk -F'\t' 'NR == FNR { if (!($2 in a)) a[$2] = $1; next }
  ($2 in a) && !($2 in visto) { visto[$2] = 1; d = $1 - a[$2];
  printf "%7d %7d %+6d %+6.1f%%  %s\n", a[$2], $1, d, a[$2] ? 100 * d / a[$2] : 0, $2 }' \
  <(tappe "$1") <(tappe "$2"); }
cd "$PROVE/10-dma" && confronta AC2-pr AC2-doc     # campo nella prima, nella seconda, scarto
```

**Run di riferimento.** Se c'è una build DuckStation `Devel` di `duckstation_ref/`, il metodo è in
`CLAUDE.md`: si contano le righe `Now in v-blank` per avere lo stesso asse in campi.

**Esito atteso.**
- `pr` contro `base`: scarti piccoli. Riportali comunque, perché la PR tocca anche altri tempi
  (writeback del DMA, instradamento XA).
- `doc` contro `pr`: scarti misurati.
- `doc` diventa il default solo se, su almeno due dischi, è più vicino alla run di riferimento e la
  scheda 6 e l'audio non peggiorano. Senza run di riferimento si riportano gli scarti e non si
  decide.

---

## Scheda 11. Audio nel cluster (`deploy/`)

**Cosa è cambiato.**
- Opus configurabile (`ZS1_WEBRTC_AUDIO_KBPS`, `_FRAME_MS`, `_TYPE`, `_FEC`, `_LOSS_PCT`).
- Coda audio limitata, con le perdite contate nel log.
- `stereo=1` negoziato: Chrome decodificava in mono.
- Un solo ricampionamento invece di due, sink a 48 kHz.
- Statistiche audio nella pagina.

È stato provato con GStreamer qui, mai nel cluster con un browser vero.

**Prerequisito.** Il cluster `cluster-zs1` deve esistere: `k3d cluster list`. Se non c'è, la scheda
non si esegue e lo scrivi.

**Esecuzione** (comandi di `README.md`, sezione Kubernetes):

```bash
source ~/zs1-prove/env.sh
cd "$REPO"
docker build -f deploy/session/Dockerfile -t zs1/session:dev .
k3d image import zs1/session:dev -c cluster-zs1
./deploy/session/start.sh                 # applica anche sessions.yaml, cambiato dalla PR
kubectl -n zs1 rollout restart deployment/zs1-acecombat
./deploy/session/expose.sh                # stampa gli indirizzi .../webrtc.html
```

**Serve l'umano**: aprire `webrtc.html` di una sessione in Chrome, giocare 2 minuti con audio, poi
ripetere in Firefox, e leggerti la riga delle statistiche (`audio ... kbit/s jb ... ms plc ...%
stereo`).

**Cosa controlli tu:**

```bash
source ~/zs1-prove/env.sh
kubectl -n zs1 logs deploy/zs1-acecombat -c emulator | grep '\[webrtc\]'
```

**Esito atteso.**
- `set_state(PLAYING) -> ...; audio ...` con le impostazioni Opus in uso.
- Righe `audio: N buffers dropped in the last 10 s` assenti o rare.
- Nella pagina:
  - `stereo`, non `mono`;
  - poco sopra 96 kbit/s: 96 è il default di `ZS1_WEBRTC_AUDIO_KBPS`, e la pagina conta anche le
    intestazioni RTP;
  - `plc` sotto l'1-2% in rete locale.
- La pagina `play.html` (WebM via ffmpeg) deve ancora suonare.

**Facoltativo.** `ZS1_SPU_RING_TARGET` riduce la latenza audio nel pod, ma il valore va scelto con
un A/B, con e senza `ZS1_SPU_NO_STRETCH=1`. Senza misure non cambiarlo.

---

## Report

Un blocco per scheda, in questo formato. Consegnalo all'umano; se chiede di salvarlo, va in
`docs/ESITI_PROVE_<AAAA-MM-GG>.md`.

```markdown
### Scheda N. <titolo>
- Esito: OK | KO | parziale | non eseguita (motivo)
- Binari: PR <commit>, base bbc1c7e
- Macchina: GPU e driver (riga del log), sessione X11/Wayland
- Comandi: quelli effettivamente lanciati
- Osservazioni: cosa è stato visto/sentito (dall'umano) e misurato (da te), con le righe di log
- Allegati: percorsi di log, dump e immagini sotto $PROVE
```

Alla fine aggiungi un riepilogo di tre righe: cosa è confermato, cosa è KO e cosa resta da fare.
