> **Allegato B** a `docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md`. Prodotto da un'analisi statica
> parallela il 2026-10-02 sul codice a `bbc1c7e` e su psx-spx al commit `00d5dcb`. Le voci riprese nel
> documento principale sono state riverificate riga per riga; le altre portano doc e codice citati per
> il controllo, ma non sono state ricontrollate una per una. I siti della documentazione ufficiale erano bloccati dal proxy: le proprietà sono verificate sui sorgenti alle versioni installate, con gli URL in sezione 8.

# Percorso audio di ZoniStation One come workload Kubernetes

Analisi end to end, dal SPU emulato al browser, dei due percorsi audio del pod di sessione
(`deploy/session/`): WebRTC (GStreamer `webrtcbin`) e WebM/Opus su HTTP (ffmpeg).
Repository a commit `bbc1c7e` (branch `stable_branch`). Nessun file del repository è stato modificato.

Legenda dello stato di ogni affermazione:

- **CERTO**: verificato leggendo il codice del repo o il sorgente/la documentazione ufficiale upstream, con citazione.
- **INFERITO**: dedotto da codice verificato, ma il comportamento a runtime non è stato osservato.
- **DA MISURARE**: richiede una misura sul cluster; il comando è nel piano di misura (sezione 7).
- **NON VERIFICATO**: non è stato possibile verificarlo da una fonte ufficiale in questa sessione.

---

## 0. Sintesi

1. **Una sola conversione 44,1 kHz -> 48 kHz, fatta da PulseAudio, e dove avviene dipende dall'ordine di avvio.** Il null sink `zs1` nasce a 44100 Hz (default del demone), ma il loop ffmpeg si collega al monitor a 48000 Hz *prima* che l'emulatore apra SDL, e il null sink supporta la riconfigurazione: con alta probabilità il sink passa a 48 kHz e PulseAudio ricampiona lo stream SDL (speex-float-1). L'`audioresample` di GStreamer è quindi un passthrough. INFERITO, DA MISURARE con `pactl list sinks`.
2. **La latenza audio è dominata dall'emulatore, non dalla rete né da Opus.** Il ring SPU (46 a 67 ms) più lo stretcher WSOLA (circa 50 a 65 ms) mirano a 4893 frame = **111 ms** (`spu_mixing.c:634-635`). Il frame Opus da 10 ms risparmia 10 ms su un totale sorgente stimato di 150 a 215 ms. INFERITO.
3. **Audio in ritardo sul video nel percorso WebRTC, stimato tra 80 e 200 ms (tipico circa 140 ms).** Audio e video condividono `msid`/CNAME e lo stesso clock di pipeline, quindi il browser li allinea per istante di cattura. Lo scarto nasce prima, nei buffer dell'emulatore. È vicino alla soglia di rilevabilità ITU-R BT.1359 (-125 ms). INFERITO, DA MISURARE.
4. **Chrome quasi certamente decodifica l'audio in mono.** libwebrtc usa 1 canale se nel fmtp manca `stereo=1`, e `webrtc.html` non tocca l'SDP (`webrtc.html:147-151`). Firefox invece mette `stereo=1` di default. CERTO per il codice libwebrtc e Firefox, INFERITO per Chrome stable.
5. **`audio-type=restricted-lowdelay` è un valore valido** (presente in GStreamer dalla 1.18; Ubuntu 24.04 ha la 1.24.2). Forza però Opus in modalità solo CELT. Di conseguenza `inband-fec` e `dtx`, che valgono solo per il livello LPC/SILK, non possono funzionare con questa impostazione. CERTO.
6. **La coda `leaky=downstream max-size-buffers=2` scarta 10 ms di audio a ogni stallo superiore a circa 20 ms.** Nessun contatore lo rende visibile. CERTO per il meccanismo, DA MISURARE la frequenza.
7. **Opus a 96 kbit/s con frame da 10 ms costa 136 kbit/s sul filo** (IPv4, UDP, RTP, SRTP-80): 42% di overhead. Con frame da 20 ms sono 116 kbit/s. In entrambi i casi è circa il 5% del budget video WAN di 2,5 Mbit/s. CERTO (calcolo).
8. **Due difetti lato emulatore entrano nello stream.** La matrice ATV del CD e il Mute del CD sono codice morto: `cdrom_get_audio_frame()` non ha chiamanti. Rilevante per Dino Crisis, una delle tre sessioni del cluster: secondo psx-spx usa Mute durante il controllo modchip. CERTO. Inoltre SDL 3.2.24 adotta `fragsize` invece di `tlength` per i flussi di playback Pulse, con buffer probabilmente doppio (1024 frame). Il bug è corretto in SDL 3.4.0. CERTO per il codice, INFERITO per l'effetto.
9. **Il percorso HTTP/ffmpeg gira sempre, anche quando si usa solo WebRTC** (`ZS1_AUDIO_STREAM` vale 1 di default e non è impostato nei manifest). Fissa la frequenza del sink, porta il null sink a blocchi di 2,5 ms (`-fragment_size 480` è in **byte**, non in frame) e, mentre aspetta un client, accumula fino a 4 MiB (circa 21,8 s) di audio vecchio. INFERITO, DA MISURARE.

---

## 1. Metodo e limiti

- Codice del repository letto riga per riga per i file citati. Documentazione PlayStation: clone psx-spx `ps1/` al commit `00d5dcb`, citata come `ps1/<percorso>:<riga>`.
- I domini della documentazione ufficiale (gstreamer.freedesktop.org, ffmpeg.org, rfc-editor.org, w3.org, webrtc.googlesource.com, opus-codec.org, itu.int) sono **bloccati dal proxy di uscita** di questa sessione. Ho quindi verificato sui **sorgenti da cui quella documentazione è generata**, alle versioni che il Dockerfile installa, scaricati da raw.githubusercontent.com:
  - GStreamer 1.24.2: `gst_plugins_cache.json` è il file da cui si genera la pagina ufficiale delle proprietà;
  - FFmpeg n6.1.1: `doc/*.texi`;
  - SDL release-3.2.24, PulseAudio v16.1, libopus v1.4;
  - libwebrtc, nella copia vendorizzata in `mozilla-firefox/firefox` (main);
  - W3C `webrtc-stats` e `webrtc-pc`: sorgenti ReSpec delle specifiche;
  - Opus RTP: `draft-ietf-payload-rtp-opus-01` nel repo xiph/opus v1.1, precursore di RFC 7587.

  Per ogni affermazione do l'URL della documentazione ufficiale e quello del sorgente effettivamente letto.
- Versioni dei pacchetti Ubuntu 24.04 (noble) verificate su packages.ubuntu.com:
  - `gstreamer1.0-plugins-base` 1.24.2, contiene `libgstopus.so`, `libgstaudioresample.so`, `libgstaudioconvert.so`;
  - `gstreamer1.0-plugins-good` 1.24.2, contiene `libgstpulseaudio.so` e `libgstrtp.so`;
  - `pulseaudio` 1:16.1, `ffmpeg` 7:6.1.1, `libopus0` 1.4.
- Nessuna misura è stata eseguita: il cluster non è raggiungibile da questa sessione. Tutto ciò che riguarda il runtime è INFERITO o DA MISURARE.

---

## 2. Mappa del percorso

```
 Disco (XA-ADPCM / CD-DA)                     SPU (24 voci, riverbero)
   cdrom_commands.c:923 decode                 spu_mixing.c:317-451
   cdrom_audio.c:217-263 zigzag -> 44,1 kHz           |
   AudioFifo (cap. 9408 frame)  ---pop--->  spu_step() 1 campione / 768 cicli
                                               |
                         ring SPSC 4096 slot (spu.h:41), target 2048 (spu.h:35)
                                               |
                         spu_fill_audio -> WSOLA (spu_stretch.c), target 4893 frame
                                               |
                SDL3 SDL_AudioStream S16/2ch/44100, hint 512 frame (main.c:449-453)
                                               |
           PulseAudio: sink-input 44,1 kHz -> null sink "zs1" (48 kHz probabile) [resample speex]
                                               |
                                      zs1.monitor (48 kHz)
                          +--------------------+---------------------+
                          |                                          |
     pulsesrc 48k/2ch (webrtc_server.py:107)        ffmpeg -f pulse 48k (entrypoint.sh:90-95)
     queue leaky 2 buf -> audioconvert ->            libopus 96k VBR, lowdelay, 10 ms
     audioresample -> opusenc 96k 10 ms CELT         WebM live, cluster 40 ms
     -> rtpopuspay pt 97 -> queue 2 -> webrtcbin     HTTP :6081, un client (-listen 1)
     SRTP AES128_CM_SHA1_80, UDP (TURN)              Traefik /audio o port-forward
                          |                                          |
     Browser: NetEq + decodifica Opus               Browser: <audio> progressivo
     (Chrome: mono se manca stereo=1)               play.html insegue il bordo live
```

---

## 3. Lato emulatore

### 3.1 Generazione dei campioni SPU: 44100 Hz, stereo, 16 bit (CERTO)

- **Riferimento hardware.** psx-spx: frequenza di uscita 44100 Hz (`ps1/spu/soundprocessingunitspu.md:8`). "The Mixer and DAC supports a 44.1kHz output rate" (`:242`). 33,8688 MHz = 768 x 44,1 kHz (`:1180`).
- **Costanti nel codice.** `SAMPLE_RATE 44100` (`include/spu.h:25`) e `CPU_TICKS_PER_SPU_TICK 768` (`include/spu.h:26`). `spu_step()` produce un campione stereo ogni 768 cicli emulati (`src/spu/spu_mixing.c:473-549`).
- **Quando si genera.** Sul clock emulato, tramite l'evento schedulato `EVQ_SPU` ogni 64 campioni (1,45 ms, `spu.h:27-29`) e tramite `spu_catch_up()` prima di ogni accesso ai registri (`spu_mixing.c:573-590`). Il thread SPU a tempo reale è stato rimosso (`CHANGELOG.md:920`).
- **Formato.** Mix a 32 bit con saturazione a 16 bit dopo il volume master (`spu_mixing.c:407-408`), uscita `int16_t` sinistro/destro (`:450-451`).
- **Bitrate PCM.** 44100 x 2 x 16 = **1411,2 kbit/s**.
- **Ring di uscita.** SPSC lock-free, `SPU_SAMPLE_BUFFER_SIZE 4096` slot, di cui 4095 utilizzabili (`spu.h:41`, `spu_mixing.c:536-547`). Se è pieno il campione viene generato comunque e poi scartato (`dropped_samples++`, `:546`). Il consumatore è il callback SDL (`spu_get_samples`, `:596-612`).
- **Produzione a raffiche.** Il ciclo principale emula un field intero e poi attende. Per field PAL 680823 cicli / 768 = **886,5 campioni**; NTSC 566203 / 768 = 737,2 (cicli per frame da `CHANGELOG.md:933`).

### 3.2 Ingresso CD nel mix

- `spu_step()` estrae un frame dall'`AudioFifo` del CD a ogni campione SPU. Se la FIFO è vuota ripete l'ultimo valore e incrementa `total_starved` (`spu_mixing.c:507-516`). Poi applica il volume CD dell'SPU (`:518-523`) e lo somma al mix se `SPUCNT.0` è attivo (`:360-367`). CERTO.
- **Difetto (CERTO): la matrice ATV0-ATV3 e il Mute del CD non vengono mai applicati.**
  - Il codice esiste in `cdrom_get_audio_frame()`: mute alla riga `src/cdrom/cdrom.c:479`, matrice con saturazione alle righe `:486-491`.
  - La funzione **non ha chiamanti** nel repository: grep su `src/` e `include/` trova solo la definizione (`cdrom.c:471`) e la dichiarazione (`include/cdrom.h:391`).
  - L'SPU legge direttamente la FIFO (`spu_mixing.c:510-511`). Il commit `1919b3f`, che ha introdotto la matrice, non ha toccato `spu_mixing.c`.
  - `CHANGELOG.md:464-470` e `README.md:352` dicono che la matrice "reaches the mix": il codice lo smentisce.
  - Conseguenze, dalla documentazione:
    - "Mute is used by Dino Crisis 1 to mute noise during modchip detection" (`ps1/cdr/cdromdrive.md:1023`). Dino Crisis è una delle tre sessioni del cluster (`deploy/session/sessions.yaml:257-258`).
    - Le opzioni mono/stereo (Spyro) e i fade (Resident Evil 2) non hanno effetto (`ps1/cdr/cdromdrive.md:243-247`).
  - Se il rumore del controllo modchip sia davvero udibile è INFERITO.

### 3.3 XA-ADPCM: formato su disco, decodifica e ricampionamento a 44,1 kHz

**Formato (psx-spx).**

- Codinginfo del sottoheader:
  - bit 0-1 mono/stereo;
  - bit 2 frequenza (0 = 37800 Hz, 1 = 18900 Hz);
  - bit 4-5 bit per campione (0 = 4 bit, 1 = 8 bit) (`ps1/cdr/cdromformat.md:666-668`).
- Settore XA: 12h porzioni da 128 byte = 900h byte, più 14h byte a zero (`:750-751`).
- Ogni porzione: 16 byte di header più 28 word da 32 bit (`:752-773`).
- Compressione "almost 1:4 (only almost 1:4 because there are 16 header bytes within each 128-byte portion)" (`:738-740`).
- Interleave tipici (`:681-691`):
  - 37800 Hz stereo: 1/8 a doppia velocità;
  - 18900 Hz stereo e 37800 Hz mono: 1/16;
  - 18900 Hz mono: 1/32.
- Lettura a 75 settori/s, 150 a doppia velocità (`ps1/cdr/cdromdrive.md:759-760`).

**Calcolo dei bitrate.**

- Con 4 bit ci sono 8 blocchi da 28 campioni per porzione, cioè 224 campioni; per settore 18 x 224 = 4032 campioni, quindi 2016 frame stereo. Con 8 bit sono la metà.
- Payload = 2304 byte/settore (18 x 128). Settore grezzo = 2352 byte.

| Variante XA | Frame/settore | Settori/s | Payload (kbit/s) | Settore grezzo (kbit/s) | Bit dei campioni (kbit/s) | Interleave a 2x |
|---|---|---|---|---|---|---|
| 37,8 kHz, 4 bit, stereo | 2016 | 18,75 | **345,6** | 352,8 | 302,4 | 1/8 (doc `:683`) |
| 37,8 kHz, 4 bit, mono | 4032 | 9,375 | 172,8 | 176,4 | 151,2 | 1/16 (doc `:685`) |
| 18,9 kHz, 4 bit, stereo | 2016 | 9,375 | 172,8 | 176,4 | 151,2 | 1/16 (doc `:684`) |
| 18,9 kHz, 4 bit, mono | 4032 | 4,6875 | 86,4 | 88,2 | 75,6 | 1/32 (doc `:686`) |
| 37,8 kHz, 8 bit, stereo | 1008 | 37,5 | 691,2 | 705,6 | 604,8 | 1/4 (calcolato, non in tabella doc) |
| 37,8 kHz, 8 bit, mono | 2016 | 18,75 | 345,6 | 352,8 | 302,4 | 1/8 (calcolato) |
| 18,9 kHz, 8 bit, stereo | 1008 | 18,75 | 345,6 | 352,8 | 302,4 | 1/8 (calcolato) |
| 18,9 kHz, 8 bit, mono | 2016 | 9,375 | 172,8 | 176,4 | 151,2 | 1/16 (calcolato) |

Verifica sul caso comune: 302,4 / (112/128) = 345,6 kbit/s, cioè l'header costa il 12,5%. Una volta decodificato, il 37,8 kHz stereo vale 37800 x 2 x 16 = 1209,6 kbit/s; dopo il ricampionamento a 44,1 kHz vale 1411,2 kbit/s. CERTO (calcolo da righe doc).

**Implementazione (CERTO, codice).**

- **Decodifica.** `decode_xa_chunk()` (`src/cdrom/cdrom_audio.c:77-141`):
  - filtro a 2 bit (`:98`), come `ps1/cdr/cdromformat.md:781`;
  - shift > 12 trattato come 9 (`:99`), come `:780`;
  - il filtro IIR riceve il campione saturato (`:134`).
- **37800 -> 44100 Hz.** `resample_xa_37800()` (`cdrom_audio.c:217-235`) è il contatore a sei passi documentato: 7 uscite ogni 6 ingressi, con la tabella zigzag a 29 coefficienti di `:143-165`. Corrisponde a `ps1/cdr/cdromformat.md:879-924` e al six-step counter di `:953-958`. Un settore 37,8 kHz stereo da 2016 frame diventa 2352 frame a 44,1 kHz, cioè 53,3 ms.
- **18900 -> 44100 Hz.** `resample_xa_18900()` (`cdrom_audio.c:242-263`) usa un contatore a credito 7/3 e tabelle a 25 coefficienti (`:167-182`). psx-spx **non** documenta tabelle per 18900 Hz: dà solo l'ipotesi "two-step" (`ps1/cdr/cdromformat.md:930-935`). La fedeltà all'hardware di questo ramo è quindi NON VERIFICATO.
- **CD-DA.** 588 frame per settore, già a 44,1 kHz (`cdrom_audio.c:328-337`): 75 x 2352 x 8 = 1411,2 kbit/s.
- **FIFO CD.**
  - Limite di latenza `AUDIO_FIFO_MAX_LATENCY` = 2352 x 4 = 9408 frame = **213 ms**; oltre questo valore viene scartato il frame più vecchio (`include/cdrom_audio.h:21`, `cdrom_audio.c:31-35`).
  - Profondità misurata in passato: 300-2100 frame, cioè 7-48 ms (`CHANGELOG.md:918`).
  - Nota minore: il commento "~2 s" su `AUDIO_FIFO_CAPACITY` (`cdrom_audio.h:14`) è impreciso. Sono 176400 frame stereo impacchettati in `uint32_t`, cioè 4,0 s.

### 3.4 Dispositivo SDL3 e ring buffer lato host

**Richiesta dell'emulatore (CERTO, codice).**

- `audio_init()` (`src/main.c:442-471`): hint `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=512` (`:449`) e `SDL_AudioSpec want = { SDL_AUDIO_S16, 2, 44100 }` (`:451`), aperti con `SDL_OpenAudioDeviceStream` sul dispositivo di default (`:452-453`).
- Il callback (`:419-440`) chiede al ring blocchi fino a 1024 frame (`CHUNK_FRAMES`, `:424`) e li spinge con `SDL_PutAudioStreamData`.

**Cosa ottiene SDL (CERTO, sorgente SDL release-3.2.24).**

- **Driver.** Ordine dei backend: `PIPEWIRE_PREFERRED`, `PULSEAUDIO`, `PIPEWIRE`, `ALSA`... (`src/audio/SDL_audio.c:30-96`). Lo stage di build del Dockerfile non installa `libpipewire-*-dev` (`deploy/session/Dockerfile:15-21`), quindi il backend PipeWire probabilmente non è compilato e SDL usa PulseAudio. INFERITO.
- **Formato del dispositivo.** È il massimo tra la richiesta e i minimi `SDL_AUDIO_S16` / 2 canali / 44100 Hz (`SDL_sysaudio.h:41-43`, `SDL_audio.c:1758-1774`), quindi **S16, stereo, 44100 Hz**. SDL non ricampiona.
- **Dimensione del blocco.** L'hint impone 512 frame (`SDL_audio.c:148-156`).
- **Parametri del flusso Pulse** (`src/audio/pulseaudio/SDL_pulseaudio.c:672-681`): `rate = 44100`, `tlength = buffer_size` (512 frame = 2048 byte), `fragsize = 2 x buffer_size`, `prebuf`/`minreq`/`maxlength = -1`, `PA_STREAM_ADJUST_LATENCY`.
- **Bug di SDL 3.2.24 (CERTO, codice).**
  - Dopo la connessione SDL rilegge gli attributi con `buffer_size = recording ? tlength : fragsize` (`SDL_pulseaudio.c:735`). La condizione è invertita: per il playback adotta `fragsize`.
  - Per un flusso di playback libpulse non aggiorna `fragsize` dalla risposta del server (`src/pulse/stream.c:1084-1092` di PulseAudio 16.1), quindi resta il valore richiesto: 4096 byte.
  - Il bug è corretto in SDL `release-3.4.0` (`recording ? fragsize : tlength`). È ancora presente in `release-3.2.30`.
  - Effetto atteso: buffer del dispositivo di **1024 frame (23,2 ms)** invece di 512 (11,6 ms). INFERITO (forte), DA MISURARE dal log `[SYSTEM] Audio: ... buf=` (`main.c:466-467`).
- **Il log non mostra il ricampionamento di Pulse.** `SDL_GetAudioDeviceFormat()` restituisce lo spec scelto da SDL (44100), non la frequenza del sink. Il commento a `main.c:459-461` ("a device running at 48 kHz is worth seeing in the log") quindi **non si applica** a PulseAudio. INFERITO.

### 3.5 Stretcher WSOLA tra ring e dispositivo (CERTO, codice)

- Parametri: blocco 30 ms (1323 frame), ricerca 20 ms (882), sovrapposizione 10 ms (441). Working set SEQ+SEEK = 2205 frame = **50 ms** (`include/spu_stretch.h:39-53`).
- Il blocco viene prodotto solo con almeno 2205 frame in ingresso (`src/spu/spu_stretch.c:143-149`). L'uscita è FIFO dalla testa: a tempo 1,0 l'offset è 0 (`:105`).
- Il callback rabbocca lo stretcher fino a `SPU_STRETCH_WORKING + STRETCH_HOLD_MARGIN` = 2845 frame = **64,5 ms** (`spu_mixing.c:659-666`).
- Il controllore di tempo mira a `STRETCH_TARGET_FRAMES` = 2205 + 640 + 2048 = **4893 frame = 111,0 ms**, contando ring e stretcher insieme (`spu_mixing.c:634-635`).
- Banda morta ±20%, cioè 89-133 ms, prima di cambiare il tempo (`:638`, `:673-678`). Tempo limitato a 0,80-1,25 (`spu_stretch.h:60-61`).
- A tempo 1,0 lo stretcher è un passthrough bit-esatto, **ma non a latenza zero**: trattiene circa 50-65 ms di audio in più rispetto al solo ring. `ZS1_SPU_NO_STRETCH=1` lo esclude (`spu_mixing.c:644-654`).
- Underrun: il callback riempie di zeri e conta `underrun_events`/`underrun_samples` (`spu_mixing.c:689-697`).

### 3.6 Chi comanda il ritmo: l'audio (CERTO)

- Con un dispositivo aperto il ciclo principale attende finché il ring supera `SPU_RING_TARGET_SAMPLES` (2048 frame, 46,4 ms), con `SDL_Delay(1)` (`main.c:981-984`). Solo senza dispositivo il ritmo segue il periodo video (`:985-995`). È il "SPU ring is the machine's clock" di `README.md:463-464`.
- **Nel pod il dispositivo è il null sink PulseAudio**, che consuma campioni a tempo su `pa_rtclock_now()` (`module-null-sink.c:105,143,276,283`), basato su `CLOCK_MONOTONIC` (`pulsecore/core-rtclock.c:83-88`). Quindi il tempo emulato dell'intera macchina segue il clock monotono del nodo. INFERITO (forte).
- **Ripiego se PulseAudio non parte.** L'entrypoint imposta `SDL_AUDIODRIVER=dummy` (`entrypoint.sh:52`), che SDL3 rispetta ancora come nome SDL2 (`SDL_hints.c:94-99`). Il driver dummy dorme `io_delay = sample_frames*1000/freq` ms in aritmetica intera (`SDL_dummyaudio.c:34,52`): 512 x 1000 / 44100 = 11 ms invece di 11,61. Consuma quindi circa **+5%** più veloce del nominale e, poiché il ritmo segue il ring, la macchina emulata girerebbe circa il 5% più veloce. INFERITO, DA MISURARE con `scripts/clock_compare.lua`.
- **Dettaglio video correlato.** Il field PAL emulato è a 33868800/680823 = 49,747 Hz, mentre `ximagesrc` cattura a 50 fps fissi (`webrtc_server.py:30,97-98`). Ne risulta circa un frame duplicato ogni 4 s. INFERITO.

---

## 4. Lato container

### 4.1 PulseAudio e null sink

- **Avvio.** `pulseaudio --daemonize=yes --exit-idle-time=-1`, poi `pactl load-module module-null-sink sink_name=zs1` senza argomenti `rate`/`format` (`deploy/session/entrypoint.sh:47-48`). Il pacchetto è 16.1 (packages.ubuntu.com noble).
- **Spec del sink (CERTO, sorgente).** Il null sink prende lo spec di default del core se non riceve `rate`/`format`/`channels` (`module-null-sink.c:336-340`). Il default upstream è s16le, 2 canali, 44100 Hz, con frequenza alternativa 48000 Hz (`src/daemon/daemon.conf.in:86-89`). Pagina man Ubuntu: "The default settings are CD quality: 16bit native endian, 2 channels, 44100 Hz sampling" ([pulse-daemon.conf(5)](https://manpages.ubuntu.com/manpages/noble/man5/pulse-daemon.conf.5.html)).
- **Ricampionatore.** `resample-method` di default è `speex-float-1` ([pulse-daemon.conf(5)](https://manpages.ubuntu.com/manpages/noble/man5/pulse-daemon.conf.5.html); `daemon.conf.in:56`). CERTO per il default upstream; DA MISURARE che `/etc/pulse/daemon.conf` nell'immagine non lo cambi.
- **Sink di default (INFERITO).** `module-always-sink` (caricato da `default.pa.in:137`) scarica il suo `auto_null` appena compare un altro sink (`module-always-sink.c:118-121`). `zs1` resta così l'unico sink, quindi quello di default, su cui SDL apre. Lo conferma indirettamente la cattura a -18,7 dB registrata in `CHANGELOG.md:108-111`.
- **La frequenza del sink dipende da chi si collega per primo (CERTO il meccanismo, INFERITO l'esito).**
  - Il null sink registra una callback di riconfigurazione (`module-null-sink.c:171-174,399`). Quando nasce un source-output con frequenza diversa, `pa_source_reconfigure()` sul monitor richiama `pa_sink_reconfigure()` sul sink (`source-output.c:377-383`, `source.c:1135-1142`). La riconfigurazione è permessa solo se sink e monitor non sono RUNNING (`sink.c:1503-1514`, `source.c:1069-1080`) e se la frequenza richiesta è la default o l'alternativa (`source.c:1098-1100`).
  - L'entrypoint lancia il loop ffmpeg in background (`entrypoint.sh:79-98`) **prima** di eseguire l'emulatore (`:114-118`), e l'emulatore apre l'audio solo dopo Vulkan (`main.c:644` contro `:686`). ffmpeg apre il monitor a 48000 Hz (default del suo demuxer pulse, `pulse_audio_dec.c:380`). Quindi il sink passa a **48000 Hz** e PulseAudio ricampiona lo stream SDL 44,1 -> 48 kHz con speex-float-1.
  - Il monitor esce a 48 kHz: `pulsesrc` e ffmpeg non ricampionano e `audioresample` è un passthrough.
  - Con `ZS1_AUDIO_STREAM=0` il sink resta a 44,1 kHz (SDL arriva prima di `pulsesrc`, che nasce solo alla connessione di un viewer, `webrtc_server.py:245`). In quel caso è il source-output di `pulsesrc` a essere ricampionato a 48 kHz.
  - In entrambi i casi c'è **una sola** conversione 44,1 -> 48 kHz, ma in punti diversi. DA MISURARE (`pactl list sinks`, `pactl list sink-inputs`, campo `Resample method`).
- **Latenza del null sink (INFERITO, forte).**
  - Il null sink elabora blocchi pari alla latenza richiesta più bassa tra i suoi ingressi (`module-null-sink.c:163-166`). Nel calcolo rientra anche la latenza richiesta dal monitor (`sink.c:3147-3151`).
  - ffmpeg chiede `fragment_size` 480 **byte** (vedi 4.3) con `PA_STREAM_ADJUST_LATENCY` (`pulse_audio_dec.c:227-229`): 480 / 4 = 120 frame = **2,5 ms** a 48 kHz.
  - Il sink lavora quindi a blocchi di circa 2,5 ms (circa 400 risvegli al secondo) per tutto il tempo in cui ffmpeg è collegato.

### 4.2 Consumatore 1: pipeline GStreamer `webrtcbin` (`webrtc_server.py:94-115`)

| Elemento e impostazione | Verifica | Stato |
|---|---|---|
| `pulsesrc device=zs1.monitor provide-clock=false` (`:107`) | `provide-clock` è una proprietà di GstAudioBaseSrc, default TRUE (`gstaudiobasesrc.c:66,182-184`). Default `buffer-time` 200 ms e `latency-time` 10 ms (`:62-63`). `pulsesrc` chiede a Pulse `fragsize` = latency-time e `maxlength` = buffer-time, con `ADJUST_LATENCY` (`ext/pulse/pulsesrc.c:1518-1531`). Doc: [pulsesrc](https://gstreamer.freedesktop.org/documentation/pulseaudio/pulsesrc.html), [GstAudioBaseSrc](https://gstreamer.freedesktop.org/documentation/audio/gstaudiobasesrc.html) | CERTO |
| `audio/x-raw,channels=2,rate=48000` (`:108`) | Il formato non è fissato; `audioconvert` lo porta a S16LE come richiede `opusenc` | CERTO |
| `queue max-size-buffers=2 leaky=downstream` (`:109`) | `downstream` = "Leaky on downstream (old buffers)" (`gstqueue.c:241-244`). Restano attivi anche i limiti di default `max-size-bytes` 10 MiB e `max-size-time` 1 s (`:129-131`). Con buffer da 10 ms la coda tiene **al massimo 20 ms**. Doc: [queue](https://gstreamer.freedesktop.org/documentation/coreelements/queue.html) | CERTO |
| `audioconvert ! audioresample` (`:110`) | `audioresample`: `quality` default 4 (0-10), metodo `kaiser` (gst_plugins_cache.json 1.24.2). Da 48 a 48 kHz non converte | CERTO / INFERITO (passthrough) |
| `opusenc bitrate=96000` (`:111`) | `bitrate` in bps, default 64000. `bitrate-type` default **constrained-vbr** (2) (cache 1.24.2; `gstopusenc.c:198`) | CERTO |
| `frame-size=10` | Enum: 2.5, 5, 10, 20, 40, 60; default 20 | CERTO |
| `audio-type=restricted-lowdelay` | Enum: `generic` (2049, default), `voice` (2048), **`restricted-lowdelay` (2051)** (`gstopusenc.c:129-130`, cache 1.24.2). Assente in gst-plugins-base 1.16.0, presente da 1.18.0 (verificato confrontando `ext/opus/gstopusenc.c` ai due tag). Ubuntu noble ha 1.24.2, quindi è valido. Doc: [opusenc](https://gstreamer.freedesktop.org/documentation/opus/opusenc.html) | CERTO |
| Pad sink di `opusenc` | S16LE interleaved, rate 48000 oppure {8000, 12000, 16000, 24000}, canali 1-255 (cache 1.24.2). **Opus non accetta 44,1 kHz**: almeno un ricampionamento è inevitabile | CERTO |
| `rtpopuspay pt=97` (`:112`) | Con ingresso a 2 canali mette `sprop-stereo=1` nelle caps (`gstrtpopuspay.c:292-296`). I campi delle caps diventano parametri `a=fmtp` (`gstsdpmessage.c:3908-3945`), quindi l'offerta contiene `sprop-stereo=1`. Proprietà `dtx` default false. Doc: [rtpopuspay](https://gstreamer.freedesktop.org/documentation/rtp/rtpopuspay.html) | CERTO (codice) / INFERITO (testo SDP finale) |
| `queue max-size-buffers=2` (`:113`) | Non leaky: se `webrtcbin` rallenta blocca a monte, e lo scarto avviene nella coda leaky | CERTO |
| `webrtcbin latency=0` (`:95`) | La proprietà è "Default duration to buffer in the jitterbuffers (in ms)" (`gstwebrtcbin.c:8782-8784`, default 200, `:67`). Vale per i flussi **ricevuti**: la sessione è solo invio, quindi **non ha effetto sull'audio inviato** | CERTO |
| SRTP | Il DTLS di GStreamer offre solo `SRTP_AES128_CM_SHA1_80` (`ext/dtls/gstdtlsagent.c:200`), quindi tag di autenticazione da 80 bit = 10 byte per pacchetto | CERTO |
| Estensioni header RTP | `webrtcbin` non aggiunge `sdes:mid` se le caps non lo contengono (`gstwebrtcbin.c:69,3596`). `rtpopuspay` non lo mette: header RTP di 12 byte | INFERITO |
| `msid` | Senza `msid` esplicito sul pad, `webrtcbin` usa il CNAME della sessione per tutte le tracce (`gstwebrtcbin.c:3135-3149`). Audio e video finiscono quindi nello stesso MediaStream, che è ciò su cui conta `webrtc.html:129` | CERTO (codice) |

**Proprietà Opus non impostate e relativi default (CERTO, cache 1.24.2):** `inband-fec` false, `packet-loss-percentage` 0, `dtx` false, `complexity` 10, `bandwidth` fullband, `max-payload-size` 4000. I nomi delle proprietà sono quelli esatti del sorgente: `"inband-fec"`, `"packet-loss-percentage"`, `"dtx"` (`gstopusenc.c:326-339`).

**Cosa comporta `restricted-lowdelay` in libopus 1.4 (CERTO).**

- Forza `MODE_CELT_ONLY` (`src/opus_encoder.c:1352-1354`) e azzera la compensazione di ritardo (`:1115-1118`). Il lookahead è `Fs/400` = **2,5 ms** invece di 2,5 + 4 = 6,5 ms (`:2575-2577`, `:255-257`).
- Documentazione: "Only use when lowest-achievable latency is what matters most. Voice-optimized modes cannot be used" (`include/opus_defines.h:197-199`; doc HTML: [Opus encoder CTLs](https://opus-codec.org/docs/opus_api-1.4/group__opus__encoderctls.html)).
- FEC in banda e DTX "only applicable to the LPC layer" (`opus_defines.h:479-490`, `:516-522`). Con solo CELT, `inband-fec=true` non può produrre FEC.

**Presenza dei plugin nell'immagine (CERTO).** Il Dockerfile installa `gstreamer1.0-plugins-base`, `-good`, `-bad` e `gstreamer1.0-nice` (`Dockerfile:50-52`). In noble `libgstpulseaudio.so` sta in `gstreamer1.0-plugins-good`; il pacchetto separato `gstreamer1.0-pulseaudio` ne è solo una dipendenza vuota. `libgstopus.so` sta in `gstreamer1.0-plugins-base` (elenchi file su packages.ubuntu.com).

### 4.3 Consumatore 2: ffmpeg WebM/Opus su HTTP (`entrypoint.sh:78-100`)

| Opzione | Significato verificato (FFmpeg 6.1.1) | Stato |
|---|---|---|
| `-f pulse -i zs1.monitor` | Default `sample_rate` 48000, `channels` 2, codec PCM S16 nativo (`libavdevice/pulse_audio_dec.c:37,380-381`; `doc/indevs.texi:1284-1288`, [ffmpeg-devices pulse](https://ffmpeg.org/ffmpeg-devices.html#pulse)) | CERTO |
| `-fragment_size 480` | "Specify the size **in bytes** of the minimal buffering fragment" (`indevs.texi:1293-1295`). Passato tale e quale a `attr.fragsize` (`pulse_audio_dec.c:165-170`). 480 B / 4 B per frame = 120 frame = **2,5 ms** a 48 kHz. Il commento di `entrypoint.sh:87` ("a 480-frame pulse fragment is 10 ms") **sbaglia l'unità**: per 10 ms servono 1920 byte | CERTO |
| `-c:a libopus -b:a 96k` | `vbr` default `on` (`libavcodec/libopusenc.c:554-557`; `doc/encoders.texi:935-949`, [ffmpeg-codecs libopus](https://ffmpeg.org/ffmpeg-codecs.html#libopus-1)). A differenza di `opusenc`, VBR non vincolato | CERTO |
| `-application lowdelay` | Corrisponde a `OPUS_APPLICATION_RESTRICTED_LOWDELAY` (`libopusenc.c:547-550`; `encoders.texi:971-981`). Il commento "keeps the encoder from looking ahead" (`entrypoint.sh:88`) è impreciso: restano 2,5 ms di lookahead (`opus_encoder.c:2575`) | CERTO |
| `-frame_duration 10` | Valori ammessi 2.5, 5, 10, 20, 40, 60; default 20. "Smaller frame sizes achieve lower latency but less quality at a given bitrate" (`encoders.texi:956-961`) | CERTO |
| `-live 1`, `-cluster_time_limit 40` | Opzioni del muxer matroska/webm (`libavformat/matroskaenc.c:3503,3506`) | CERTO |
| `-listen 1` (http) | "experimental HTTP server", un solo client (`doc/protocols.texi:528-533`, [ffmpeg-protocols http](https://ffmpeg.org/ffmpeg-protocols.html#http)) | CERTO |

**Rischio di audio vecchio alla connessione (INFERITO, DA MISURARE).**

- ffmpeg apre gli input prima degli output; con `-listen 1` l'apertura dell'output blocca finché non arriva un client.
- Nel frattempo lo stream di registrazione Pulse è già collegato e non viene letto. Il suo `maxlength` è -1 (`pulse_audio_dec.c:152`), che il server porta a 4 MiB (`protocol-native.c:72,405-406`). La coda lato client ha la stessa dimensione (`pulse/stream.c:1180-1183`).
- 4 MiB a 48 kHz s16 stereo sono circa **21,8 s** di audio vecchio, che ffmpeg consegna per primo al client appena si collega.
- `play.html` lo maschera con il salto al bordo live (`SEEK_AT` 0,6 s, `play.html:54-56,72-81`). Ma il "ms behind" mostrato (`:74-79`) misura solo il buffer del browser, non il ritardo end to end.
- Che ffmpeg apra davvero l'input prima di accettare il client è INFERITO (ordine input/output di `ffmpeg.c`, non riletto in questa sessione).

**Costo quando nessuno ascolta.** Nei manifest `ZS1_AUDIO_STREAM` non è impostato e il default è 1 (`entrypoint.sh:78`), quindi ffmpeg è sempre collegato al monitor anche per chi usa solo WebRTC. Effetti: CPU, frequenza del sink fissata, blocchi da 2,5 ms nel null sink. INFERITO.

### 4.4 Conteggio dei ricampionamenti

| Tratto | Conversione | Dove | Stato |
|---|---|---|---|
| XA 37,8 -> 44,1 kHz | zigzag 7:6 | emulatore, `cdrom_audio.c:217-235` (come l'hardware) | CERTO |
| XA 18,9 -> 44,1 kHz | 7:3 | emulatore, `cdrom_audio.c:242-263` (modello hardware non documentato) | CERTO (codice) |
| Voci SPU | interpolazione gaussiana di pitch | emulatore (come l'hardware) | CERTO |
| SPU 44,1 kHz -> SDL | nessuna (dispositivo a 44100) | SDL | CERTO |
| 44,1 -> 48 kHz | speex-float-1 | PulseAudio: sink-input SDL (caso tipico) **oppure** source-output di `pulsesrc` (`ZS1_AUDIO_STREAM=0`) | INFERITO, DA MISURARE |
| 48 -> 48 kHz | passthrough | GStreamer `audioresample` | INFERITO |
| 48 kHz -> uscita browser | dipende dal dispositivo del client | browser / sistema operativo | fuori controllo |

Non esiste un doppio ricampionamento lato server nel caso tipico. Un audio XA attraversa però due conversioni in cascata, 37,8 -> 44,1 e 44,1 -> 48. Evitare la seconda è impossibile con Opus (pad sink solo 48/24/16/12/8 kHz). Si può solo scegliere dove farla e con che qualità (raccomandazione F).

### 4.5 Bilancio di latenza, percorso WebRTC (stime per stadio)

| Stadio | Valore | Base | Stato |
|---|---|---|---|
| FIFO XA (solo per XA/FMV) | 7-48 ms misurati, massimo 213 ms | `CHANGELOG.md:918`, `cdrom_audio.h:21` | CERTO (limite) |
| Ring SPU | 46,4 ms target, fino a 66,5 ms dopo un field, 92,9 ms se pieno | `spu.h:35,41` | CERTO (codice) |
| WSOLA | circa 50-65 ms (in) + 0-20 ms (out); ring+stretcher = 111 ms (banda 89-133) | `spu_mixing.c:628-678` | CERTO (codice), DA MISURARE |
| Buffer SDL | 11,6 ms richiesti; probabilmente 23,2 ms (bug 3.2.24) | 3.4 | INFERITO |
| Sink-input Pulse + null sink | tlength ≥11,6 ms + blocco null sink (circa 2,5 ms) + speex | 4.1 | INFERITO, DA MISURARE |
| `pulsesrc` | frammento 10 ms (ring fino a 200 ms) | `gstaudiobasesrc.c:62-63` | CERTO |
| Coda leaky | ≤20 ms | `webrtc_server.py:109` | CERTO |
| `opusenc` | 10 ms di frame + 2,5 ms di lookahead | `opus_encoder.c:2575` | CERTO |
| Rete | RTT/2 + relay TURN/DERP | `sessions.yaml:74-79` | DA MISURARE |
| Browser | jitter buffer NetEq adattivo + uscita audio | stats W3C | DA MISURARE |

- **Totale lato sorgente**, dalla generazione SPU al pacchetto Opus: circa **127-215 ms**, tipico circa 165 ms. Rete e browser vanno aggiunti. INFERITO.
- **Video lato sorgente**: rendering e present su Xvfb (0-20 ms), fase di `ximagesrc` a 50 fps (0-20 ms), NVENC (pochi ms, NON VERIFICATO): circa 10-45 ms.
- **Scarto A/V risultante**: audio in ritardo di circa **80-200 ms, tipico circa 140 ms**. INFERITO, DA MISURARE.
- **Percorso HTTP**: stessi 111 ms dell'emulatore + circa 2,5 ms di frammento + 12,5 ms Opus + cluster fino a 40 ms + TCP + buffer del browser tenuto tra 150 e 600 ms da `play.html:54-56`. In totale da centinaia di ms a oltre 0,7 s. INFERITO.

### 4.6 Overhead Opus sul filo e confronto con il budget video

Ipotesi per pacchetto:

- payload Opus = bitrate / 100 pacchetti/s;
- RTP 12 B (nessuna estensione, vedi 4.2);
- tag SRTP 10 B (AES128_CM_SHA1_80, CERTO);
- UDP 8 B, IPv4 20 B.

Le dimensioni degli header RTP/UDP/IP, TURN e WireGuard sono valori di protocollo standard, non riletti in RFC questa sessione: NON VERIFICATO via fetch.

| Configurazione | Pacchetti/s | Payload | Sul filo (IPv4) | + TURN ChannelData (4 B) | + TURN Data indication (36 B) | IPv6 | + WireGuard diretto (60 B) | Overhead |
|---|---|---|---|---|---|---|---|---|
| **96k, 10 ms (attuale)** | 100 | 120 B | **136,0 kbit/s** | 139,2 | 164,8 | 152,0 | 184,0 | 42% |
| 96k, 20 ms | 50 | 240 B | 116,0 | 117,6 | 130,4 | 124,0 | 140,0 | 21% |
| 64k, 10 ms | 100 | 80 B | 104,0 | 107,2 | 132,8 | 120,0 | 152,0 | 62% |
| 64k, 20 ms | 50 | 160 B | 84,0 | 85,6 | 98,4 | 92,0 | 108,0 | 31% |
| 128k, 10 ms | 100 | 160 B | 168,0 | 171,2 | 196,8 | 184,0 | 216,0 | 31% |
| 128k, 20 ms | 50 | 320 B | 148,0 | 149,6 | 162,4 | 156,0 | 172,0 | 16% |

- `opusenc` è in constrained-VBR: 120 B è la media, non una costante. RTCP non è incluso (trascurabile, NON VERIFICATO).
- **Confronto con il video.** `ZS1_WEBRTC_BITRATE_KBPS=2500` (`sessions.yaml:81-82`) agisce **solo** sull'encoder video (`webrtc_server.py:31,79-82,101`); l'audio è fisso a 96000 (`:111`). Con 136 kbit/s di audio, la quota audio è 136/(2500+136) = **5,2%** del traffico media.
- **DERP.** Su un relay DERP di Tailscale, che i commenti dei manifest indicano come caso critico (`sessions.yaml:74-79`), contano anche i 100 pacchetti/s: a 20 ms diventano 50/s. L'overhead specifico di DERP (TCP/TLS) è NON VERIFICATO.

---

## 5. Rischi di correttezza

**R1. La coda leaky sull'audio scarta in silenzio (CERTO il meccanismo, DA MISURARE la frequenza).**

- Con `max-size-buffers=2` e buffer da 10 ms, ogni stallo a valle oltre circa 20 ms (CPU contesa, DTLS lento, GC Python) fa scartare il buffer più vecchio (`gstqueue.c:1185-1193`).
- `GstAudioEncoder` ha `perfect-timestamp` false e tolleranza 40 ms (`gstaudioencoder.c:149-152`). Il buco diventa probabilmente un salto del timestamp RTP che il browser copre con occultamento (PLC), non un click netto. INFERITO.
- Nessun contatore lo registra. `webrtc_server.py:151-152` stampa solo i warning del bus, e il messaggio "leaking item" è a livello DEBUG nella categoria `queue_dataflow`.
- Il warning "Can't record audio fast enough" di `GstAudioBaseSrc` (`gstaudiobasesrc.c:843-851`) compare solo se si blocca `pulsesrc`. Con la coda leaky a valle succede raramente.

**R2. Stereo non negoziato in Chrome (CERTO per libwebrtc, INFERITO per Chrome stable).**

- libwebrtc imposta 2 canali solo con `stereo=1` nei parametri del formato (`api/audio_codecs/opus/audio_decoder_opus.cc:38-59`). Altrimenti usa `GetDefaultNumChannels()`, che vale **1** salvo il field trial `WebRTC-Audio-OpusDecodeStereoByDefault` (`:31-34`, `:78-88`).
- Il formato pubblicizzato di default è `minptime=10;useinbandfec=1`, senza `stereo` (`:70-75`).
- RFC 7587 (testo verificato nel draft -01): "If no value is specified, mono is assumed (stereo=0)"; `sprop-stereo` indica solo che il mittente probabilmente invia stereo ([RFC 7587](https://www.rfc-editor.org/rfc/rfc7587); `draft-ietf-payload-rtp-opus-01.xml:586-607`).
- `webrtc.html:147-151` passa l'SDP senza modificarlo.
- Firefox invece aggiunge `stereo=1` di default ("We prefer to receive stereo", `dom/media/webrtc/jsep/JsepCodecDescription.h:528-535`).
- Se Chrome stable abiliti quel field trial è NON VERIFICATO.
- Paradosso: il vecchio percorso HTTP (WebM decodificato dal media stack, non da libwebrtc) è probabilmente stereo anche in Chrome. INFERITO.

**R3. `audio-type=restricted-lowdelay` (CERTO).** Il valore esiste (GStreamer ≥1.18) ed è accettato dalla 1.24.2 dell'immagine. È adatto alla musica (CELT) e riduce il lookahead di 4 ms, ma esclude SILK, l'ibrido e quindi la FEC.

**R4. FEC e DTX (CERTO).** Sono disattivati (default `false`/0). `inband-fec=true` con `restricted-lowdelay` non avrebbe effetto, perché la FEC vale solo per il livello LPC (`opus_defines.h:479-490`). In GStreamer `inband-fec` è un booleano e quindi passa a libopus il valore 1, non il valore 2 "no switch to SILK for music" (`gstopusenc.c:788`, `opus_defines.h:486-487`). DTX vale solo per LPC (`opus_defines.h:516-522`) ed è comunque inadatto all'audio di gioco continuo.

**R5. Frame da 10 ms a 96 kbit/s: scambio sfavorevole (INFERITO).**

- Risparmia circa 10 ms su una catena lato sorgente di circa 165 ms e su un jitter buffer del browser tipicamente di decine di ms.
- Costa il raddoppio dei pacchetti (overhead 42% contro 21%, sezione 4.6) e qualità inferiore a parità di bitrate (`encoders.texi:958-959`).
- La risposta di Chrome contiene `minptime=10`: scendere a 5 o 2,5 ms violerebbe la preferenza del ricevitore (`draft-ietf-payload-rtp-opus-01.xml:560-568`).

**R6. Deriva tra il clock del monitor Pulse e quello dell'emulatore (INFERITO, forte): non è il rischio principale.**

- Il null sink consuma su `CLOCK_MONOTONIC` e l'emulatore segue il null sink (3.6).
- La pipeline GStreamer usa `GstSystemClock`, perché nessun elemento fornisce un clock con `provide-clock=false` (`gstpipeline.c:62`). Il tipo di default è `MONOTONIC` (`gstsystemclock.c:584-589`).
- `pulsesrc` si allinea con `slave-method=skew` (default, `gstaudiobasesrc.c:67`).
- Tutto deriva dallo stesso clock del nodo, quindi la deriva a lungo termine è circa zero.
- Il rischio vero di underrun è lo **scheduling**: i pod non hanno `requests`/`limits` CPU (`sessions.yaml:93-95`, solo `nvidia.com/gpu`), quindi la QoS CPU è BestEffort. PulseAudio nel container non dispone probabilmente di scheduling realtime. INFERITO, DA MISURARE (`audio_underrun_probe.lua`).

**R7. Sincronizzazione A/V in WebRTC.**

- **Meccanismo presente (CERTO, codice).** `msid`/CNAME sono comuni alle due tracce (`gstwebrtcbin.c:3135-3149`) e il clock di pipeline è unico, quindi il browser può allineare per istante di cattura.
- **Scarto presente (INFERITO).** L'audio catturato a un dato istante è stato generato circa 111 ms o più prima, il video circa 10-45 ms prima. Risultato: circa 80-200 ms di ritardo audio (4.5).
- **Soglie di riferimento.** ITU-R BT.1359 dà una soglia di rilevabilità di +45/-125 ms e di accettabilità di circa +90/-185 ms ([ITU-R BT.1359](https://www.itu.int/rec/R-REC-BT.1359); valori letti solo nei risultati di ricerca, il PDF non è stato scaricato: NON VERIFICATO in prima persona).
- Nelle FMV si aggiunge la FIFO XA (7-48 ms).
- `docs/VULKAN_KUBERNETES_ARCHITECTURE.md:129,151` promette "RTP Stream < 20ms" e audio "synchronized" da uno "SPU worker thread". Il thread non esiste più (`CHANGELOG.md:920`) e la latenza è un ordine di grandezza sopra. CERTO.

**R8. Jitter buffer del browser (CERTO per i nomi, DA MISURARE i valori).**

- È adattivo (NetEq in Chrome). `webrtcbin latency=0` non lo influenza.
- Si può alzare il minimo con `RTCRtpReceiver.jitterBufferTarget` (ms) ([webrtc-pc](https://www.w3.org/TR/webrtc/); `webrtc.html:10458,10508` nel sorgente W3C).
- `webrtc.html` raccoglie statistiche solo del video (`webrtc.html:182`): oggi nessuna metrica audio è visibile.

**R9. Matrice ATV e Mute del CD non applicati (CERTO, sezione 3.2).**

**R10. SDL 3.2.24 con buffer di playback Pulse adottato da `fragsize` (CERTO il codice, INFERITO l'effetto, sezione 3.4).**

**R11. Commenti fuorvianti (CERTO).**

- `entrypoint.sh:87`: "480-frame" sono 480 **byte** = 2,5 ms.
- `entrypoint.sh:88`: il lookahead non è zero.
- `webrtc_server.py:90-93` descrive code "one buffer deep", ma quella audio è da 2.

---

## 6. Raccomandazioni (pseudo-soluzioni)

Ogni voce riporta effetto atteso, rischio e stato. Sono ordinate per beneficio rispetto allo sforzo.

### A. Parametri audio configurabili come quello video

```python
# webrtc_server.py, accanto a BITRATE (righe 30-34)
A_KBPS  = int(os.environ.get("ZS1_WEBRTC_AUDIO_KBPS", "96"))
A_FRAME = os.environ.get("ZS1_WEBRTC_AUDIO_FRAME_MS", "20")        # 2.5|5|10|20|40|60
A_TYPE  = os.environ.get("ZS1_WEBRTC_AUDIO_TYPE", "restricted-lowdelay")  # o generic
A_FEC   = os.environ.get("ZS1_WEBRTC_AUDIO_FEC", "0") == "1"
A_LOSS  = int(os.environ.get("ZS1_WEBRTC_AUDIO_LOSS_PCT", "0"))
OPUS = (f"opusenc bitrate={A_KBPS*1000} frame-size={A_FRAME} audio-type={A_TYPE} "
        f"bitrate-type=constrained-vbr"
        + (f" inband-fec=true packet-loss-percentage={A_LOSS}" if A_FEC else ""))
# Con A_FEC e A_TYPE == "restricted-lowdelay": rifiutare o forzare "generic" e
# loggarlo, perché in CELT-only la FEC non esiste (opus_defines.h:479-490).
```

- **Effetto.** Si può regolare per sessione da `sessions.yaml`, come per `ZS1_WEBRTC_BITRATE_KBPS`.
- **Rischio.** Basso.
- **Stato.** I nomi delle proprietà sono CERTI (cache 1.24.2). I nomi delle variabili d'ambiente sono una proposta.

### B. Bitrate e durata del frame per contenuto PS1

- **Contenuto.** L'uscita SPU è 44,1 kHz stereo, quindi la banda utile arriva al massimo a 22 kHz, mentre il fullband Opus è 20 kHz. Il parlato XA a 18,9 kHz ha banda ≤9,45 kHz; l'XA a 37,8 kHz ≤18,9 kHz.
- **Riferimento.** RFC 7587 §3.1.1 indica come "sweet spot" 64-128 kb/s per musica stereo fullband con frame da 20 ms (verificato nel draft -01, `:194-206`; [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587)).
- **Proposta:**
  - LAN: 96-128 kbit/s, 10 ms. Il costo è irrilevante rispetto a 12 Mbit/s di video.
  - **WAN (default dei manifest): 96 kbit/s, 20 ms** = 116 kbit/s sul filo, 20 kbit/s e 50 pacchetti/s in meno, 10 ms di latenza in più.
  - DERP o link stretti: 64 kbit/s, 20 ms = 84 kbit/s sul filo, circa 52 kbit/s in meno rispetto a oggi.
- **Effetto.** Al massimo circa il 2% del budget totale. La leva vera resta il bitrate video.
- **Rischio.** 64 kbit/s può impoverire lo stereo della musica SPU.
- **Stato.** DA MISURARE con ascolto ABX su tre scene: musica SPU stereo, FMV con XA, parlato XA 18,9 kHz.

### C. Coda audio a tempo, non a buffer, e con contatore

```
pulsesrc device={SINK_MON} provide-clock=false latency-time=10000 buffer-time=100000
  ! audio/x-raw,channels=2,rate=48000
  ! queue name=aq leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=60000000
  ! audioconvert ! audioresample ! {OPUS} ! rtpopuspay pt=97
  ! queue name=aq2 max-size-buffers=0 max-size-bytes=0 max-size-time=60000000 ! sendrecv.
```

```python
self.pipe.get_by_name("aq").connect("overrun", lambda q: counters.__setitem__("aq_overrun", counters.get("aq_overrun", 0) + 1))
```

- Il segnale `overrun` esiste (`gstqueue.c:292-302`). `max-size-time` e `leaky` sono proprietà standard di `queue`.
- **Effetto.** Assorbe stalli fino a 60 ms invece di 20 e rende visibile ogni scarto.
- **Rischio.** Fino a 40 ms di latenza in più, ma solo durante uno stallo.
- **Stato.** INFERITO. Che `overrun` venga emesso anche per le code leaky è NON VERIFICATO.

### D. Stereo in Chrome: munging della risposta

```js
// webrtc.html, dopo createAnswer() e prima di setLocalDescription() (righe 149-150)
function preferStereo(sdp) {
  const m = sdp.match(/a=rtpmap:(\d+) opus\/48000\/2/i);
  if (!m) return sdp;
  const pt = m[1];
  const re = new RegExp(`a=fmtp:${pt} ([^\\r\\n]*)`);
  return re.test(sdp)
    ? sdp.replace(re, (l, p) => /stereo=/.test(p) ? l : `a=fmtp:${pt} ${p};stereo=1;sprop-stereo=1`)
    : sdp.replace(m[0], `${m[0]}\r\na=fmtp:${pt} stereo=1;sprop-stereo=1`);
}
answer.sdp = preferStereo(answer.sdp);
```

- **Effetto.** Decodifica stereo in Chrome: è la condizione di `SdpToConfig`.
- **Rischio.** Basso. Il munging dell'SDP locale è tollerato ma non standardizzato. In Firefox non cambia nulla.
- **Stato.** INFERITO. Verifica con `RTCCodecStats.sdpFmtpLine` e con il test L/R (sezione 7).

### E. FEC sulla WAN (opzionale, con compromesso)

- **Configurazione.** `audio-type=generic inband-fec=true packet-loss-percentage=5..10`. Facoltativamente si può aggiornare `packet-loss-percentage` a runtime dalla perdita osservata (statistiche `webrtcbin` "get-stats"): `opusenc` applica le modifiche di proprietà all'encoder attivo (`gstopusenc.c:1208-1250`).
- **Effetto.** Recupero di pacchetti isolati persi. Il ricevitore pubblicizza già `useinbandfec=1` (`audio_decoder_opus.cc:73-74`).
- **Rischio.** +4 ms di lookahead (`opus_encoder.c:255-257,2575-2577`). Con perdite alte libopus passa a SILK "even at high rates" (`opus_defines.h:485`), perché GStreamer non espone il valore 2. Sulla musica questo degrada la qualità.
- **Alternativa.** Mantenere CELT e accettare il PLC. RED (RFC 2198) per l'audio in `webrtcbin` è NON VERIFICATO.
- **Stato.** INFERITO, DA MISURARE (`fecPacketsReceived`, `concealedSamples`).

### F. Rendere deterministica e di qualità l'unica conversione 44,1 -> 48 kHz

- **Opzione F1 (consigliata).** `pactl load-module module-null-sink sink_name=zs1 rate=48000` in `entrypoint.sh:48` (argomento `rate` verificato in `module-null-sink.c:49-57`). Scegliere il ricampionatore all'avvio: `pulseaudio --daemonize=yes --exit-idle-time=-1 --resample-method=speex-float-5`. L'opzione da riga di comando "takes precedence" sul file di configurazione ([pulse-daemon.conf(5)](https://manpages.ubuntu.com/manpages/noble/man5/pulse-daemon.conf.5.html)).
  - **Effetto.** Frequenza del sink indipendente dall'ordine di avvio e qualità migliore di speex-float-1.
  - **Rischio.** Più CPU in PulseAudio. `soxr-hq`/`soxr-vhq` danno qualità migliore ma, secondo la stessa pagina man, "can add a significant delay to the output (usually up to around 20 ms)".
- **Opzione F2.** Sink a 44100 (`rate=44100`), `pulsesrc ... ! audio/x-raw,rate=44100 ! audioconvert ! audioresample quality=10 ! audio/x-raw,rate=48000`.
  - **Effetto.** Conversione visibile e regolabile in GStreamer (`quality` 0-10, default 4).
  - **Rischio.** Se ffmpeg resta attivo, Pulse ricampiona comunque per ffmpeg; conviene accoppiarla con G.
- **Opzione F3.** Aprire SDL a 48000 (`want.freq = 48000`) e lasciare la conversione a `SDL_AudioStream`.
  - **Rischio.** Cambia anche il comportamento desktop. La qualità del ricampionatore di SDL3 è NON VERIFICATO.
- **Stato.** CERTO per argomenti e opzioni, INFERITO per l'esito.

### G. Spegnere o correggere il percorso ffmpeg quando si usa WebRTC

- **Configurazione.** In `sessions.yaml` aggiungere `ZS1_AUDIO_STREAM: "0"` alle sessioni servite solo via `webrtc.html`. Dove serve ancora: `-fragment_size 1920` (10 ms a 48 kHz s16 stereo, come voleva il commento) e avvio di ffmpeg solo a connessione avvenuta, per evitare i circa 21,8 s di audio vecchio.
- **Effetto.** Meno CPU, null sink non più forzato a 2,5 ms, niente frequenza del sink decisa dall'ordine di avvio, niente coda vecchia.
- **Rischio.** `play.html` perde l'audio finché non viene riattivato.
- **Stato.** INFERITO.

### H. Recuperare latenza nell'emulatore (la voce con più margine)

1. A/B con `ZS1_SPU_NO_STRETCH=1` nel pod. Nel container il "dispositivo" è il null sink sullo stesso clock monotono (3.6), quindi la deriva che il WSOLA corregge è circa nulla. **Effetto**: circa -50/-65 ms. **Rischio**: più underrun sotto contesa CPU. Misura: `audio_underrun_probe.lua`.
2. Portare SDL a ≥ `release-3.4.0` (correzione di `fragsize`/`tlength`), oppure abbassare l'hint a 256 frame. **Effetto**: circa -12 ms. **Rischio**: più callback; sul desktop la latenza su cui `SPU_RING_TARGET_SAMPLES` è tarata cambia (`main.c:445-448`).
3. Rendere `SPU_RING_TARGET_SAMPLES` configurabile da variabile d'ambiente (oggi è una costante in `spu.h:35`): ad esempio 1024 nel pod. **Rischio**: underrun se il pacing ha jitter.

- **Stato complessivo.** INFERITO. Va confermato con `ZS1_FRAME_PROFILE=1`, **senza** `ZS1_LOG_STDERR` né probe Lua per le cifre di velocità (CLAUDE.md, "Traps"). I manifest oggi impostano `ZS1_LOG_STDERR=1` (`sessions.yaml:66-67`).

### I. Compensare lo scarto A/V residuo (solo dopo H)

- **Metodo.** `GstPad.set_offset()` esiste (`gstpad.h:1504-1507`). Un offset negativo sul pad sorgente di `pulsesrc` anticiperebbe il running time dell'audio di N ms.
- **Rischio.** L'effetto su RTCP SR e sulla sincronizzazione del browser è NON VERIFICATO. La via pulita resta ridurre i buffer (H).

### J. Richieste CPU per i pod

- **Configurazione.** `resources.requests.cpu` (e `limits` uguali per la QoS Guaranteed) nei tre Deployment.
- **Effetto.** Meno jitter di scheduling per emulatore, PulseAudio e GStreamer.
- **Rischio.** Meno densità per nodo.
- **Stato.** INFERITO.

### K. Statistiche audio in `webrtc.html`

```js
if (r.type === 'inbound-rtp' && r.kind === 'audio') aud = r;
// jb medio = Δ jitterBufferDelay / Δ jitterBufferEmittedCount ; PLC = Δ concealedSamples / Δ totalSamplesReceived
```

- **Stato.** CERTO per i nomi (W3C webrtc-stats).

### L. Emulatore: collegare `cdrom_get_audio_frame()` nel mix

- **Configurazione.** In `spu_step()` sostituire `cdrom_audio_fifo_pop()` (`spu_mixing.c:510-511`) con `cdrom_get_audio_frame()`, mantenendo la logica di hold sul frame vuoto.
- **Effetto.** Mute (Dino Crisis) e matrice ATV corretti nello stream.
- **Rischio.** Regressione di volume se gli ATV non sono inizializzati a 80h.
- **Stato.** CERTO il difetto, INFERITO la correzione.

---

## 7. Piano di misura (nomi verificati)

Tutte le misure di velocità o latenza vanno fatte **senza** `ZS1_LOG_STDERR=1` e senza probe Lua per vblank (CLAUDE.md). Per esempio: `kubectl -n zs1 set env deploy/zs1-acecombat ZS1_LOG_STDERR-`, poi ripristinare.

**1. PulseAudio nel pod** (campi stampati da `pactl` 16.1, `src/utils/pactl.c:298,660-676,1366-1380,1490-1503`):

```sh
P=$(kubectl -n zs1 get pod -l game=acecombat -o name)
kubectl -n zs1 exec $P -- pactl info              # "Default Sink", "Default Sample Specification"
kubectl -n zs1 exec $P -- pactl list sinks        # zs1: "State", "Sample Specification" (44100 o 48000?)
kubectl -n zs1 exec $P -- pactl list sink-inputs  # stream SDL: "Sample Specification", "Buffer Latency", "Sink Latency", "Resample method"
kubectl -n zs1 exec $P -- pactl list source-outputs # pulsesrc e ffmpeg: "Buffer Latency", "Source Latency", "Resample method"
```

Da ripetere con e senza viewer WebRTC collegato, e con `ZS1_AUDIO_STREAM=0`, per confermare la sezione 4.1.

**2. Emulatore.**

- Riga di log `[SYSTEM] Audio: %d Hz ch=%d fmt=0x%x buf=%d` (`main.c:466`). `buf=1024` conferma R10.
- `ZS1_LUA_SCRIPT=scripts/audio_underrun_probe.lua` e `scripts/clock_compare.lua`.
- `emu.audio_stats()`: nove valori, il nono è l'occupazione del ring (`lua_debug.c:714-728`).
- `emu.stretch()`: il quinto valore è la coda dello stretcher (`lua_debug.c:735-744`).
- Latenza interna = (ring + coda stretcher) / 44100.

**3. GStreamer.**

- Ambiente del processo `webrtc_server.py`, impostabile nel Deployment perché l'entrypoint lo eredita: `GST_DEBUG=opusenc:5,audioencoder:4,audiobasesrc:4,pulse:4,webrtcbin:4,queue_dataflow:5`.
  - Nomi delle categorie da `GST_DEBUG_CATEGORY_INIT` (`gstopusenc.c:351`, `gstaudioencoder.c:365`, `gstaudiobasesrc.c:84`, `gstwebrtcbin.c:634`, `gstqueue.c:186-187`).
  - Livelli: 4 = INFO, 5 = DEBUG (`gstinfo.h:81-82`).
  - Cercare "queue is full, leaking item" (`gstqueue.c:1193`) e "Can't record audio fast enough" (`gstaudiobasesrc.c:847-848`).
  - `queue_dataflow:5` è molto verboso anche per le code video: nominare le code (raccomandazione C) per filtrare.
- Latenza per elemento: `GST_TRACERS="latency(flags=pipeline+element+reported)" GST_DEBUG=GST_TRACER:7`. Sintassi dal commento del tracer, `plugins/tracers/gstlatency.c:32`.

**4. Browser** (chrome://webrtc-internals oppure `pc.getStats()`; nomi verificati nel sorgente di [W3C webrtc-stats](https://www.w3.org/TR/webrtc-stats/)):

- `inbound-rtp` con `kind` audio:
  - `jitterBufferDelay` / `jitterBufferEmittedCount` (ritardo medio), `jitterBufferTargetDelay`, `jitterBufferMinimumDelay`;
  - `concealedSamples`, `silentConcealedSamples`, `concealmentEvents`, `totalSamplesReceived`;
  - `insertedSamplesForDeceleration`, `removedSamplesForAcceleration`;
  - `packetsLost`, `packetsDiscarded`, `fecPacketsReceived`, `fecPacketsDiscarded`;
  - `bytesReceived`, `headerBytesReceived` (payload medio = Δ`bytesReceived`/Δ`packetsReceived`), `audioLevel`.
- `codec`: `sdpFmtpLine` (cercare `stereo=1`) e `channels`.
- `transport`: `srtpCipher`, atteso `AES_CM_128_HMAC_SHA1_80`, e `dtlsCipher`.
- `candidate-pair`: `currentRoundTripTime`, `availableOutgoingBitrate`.
- `media-playout`: `totalPlayoutDelay`, `totalSamplesCount`, `synthesizedSamplesDuration`.
- Esperimento sul jitter: impostare `receiver.jitterBufferTarget` (webrtc-pc) e osservare `jitterBufferTargetDelay`.

**5. Test stereo.**

- Su una scena con panning evidente: Web Audio `createMediaStreamSource(v.srcObject)` -> `ChannelSplitter` -> due `AnalyserNode`, poi confronto della correlazione L/R. Correlazione circa 1,0 significa mono.
- In Chrome l'audio WebRTC remoto potrebbe dover restare collegato anche a un elemento media per arrivare a Web Audio: NON VERIFICATO.
- Ripetere in Firefox come controllo positivo.

**6. Scarto A/V end to end.**

- Registrare lo schermo del client con audio durante una FMV con eventi impulsivi, oppure il logo di avvio della BIOS. Misurare l'offset tra il frame visivo e l'attacco del suono, su 10 eventi, e prendere la mediana.
- Confrontare con la somma delle latenze interne dei punti 1-3. Confrontare A/B con `ZS1_SPU_NO_STRETCH=1`.

**7. Banda.**

- Δ`bytesReceived` + Δ`headerBytesReceived` sull'`inbound-rtp` audio, a 10 e a 20 ms di frame. Atteso circa 106 kbit/s (payload + RTP) a 10 ms; più SRTP/UDP/IP = 136 kbit/s.
- Confronto con la sezione 4.6.

---

## 8. Riferimenti

**Repository (commit `bbc1c7e`).**

- Lato emulatore:
  - `src/main.c:419-471,644,686,981-995`
  - `src/spu/spu_mixing.c:317-451,473-559,573-590,596-698`
  - `src/spu/spu_stretch.c:105,143-160`, `include/spu_stretch.h:39-61`, `include/spu.h:25-41,305-327`
  - `src/cdrom/cdrom_audio.c:26-41,77-141,143-182,217-263,265-322,328-337`, `include/cdrom_audio.h:14-21`
  - `src/cdrom/cdrom.c:471-492`, `src/cdrom/cdrom_commands.c:923-924`
  - `src/core/lua_debug.c:714-744`
- Deploy:
  - `deploy/session/entrypoint.sh:44-53,68-100,102-118`
  - `deploy/session/webrtc_server.py:30-34,94-115,151-152,245`
  - `deploy/session/webrtc.html:21,129,147-151,179-189`
  - `deploy/session/play.html:47-81`
  - `deploy/session/sessions.yaml:66-67,81-82,93-95,257-267`
  - `deploy/session/Dockerfile:15-21,26,42-54`
- Documentazione del repo:
  - `README.md:202-337,352,357,362-364,463-464`
  - `CHANGELOG.md:81-82,108-117,464-470,672-678,687-693,706-711,787-791,854-855,913,918,920,933`
  - `docs/VULKAN_KUBERNETES_ARCHITECTURE.md:125-151,218-221`

**psx-spx (`00d5dcb`).**

- `ps1/spu/soundprocessingunitspu.md:8,54,107,242,1180`
- `ps1/cdr/cdromformat.md:666-668,681-691,738-773,780-781,856-935,953-958`
- `ps1/cdr/cdromdrive.md:227-255,759-760,1018-1030`

**Documentazione ufficiale (URL) e sorgente effettivamente letto.**

- GStreamer 1.24.2:
  - pagine: [opusenc](https://gstreamer.freedesktop.org/documentation/opus/opusenc.html), [rtpopuspay](https://gstreamer.freedesktop.org/documentation/rtp/rtpopuspay.html), [pulsesrc](https://gstreamer.freedesktop.org/documentation/pulseaudio/pulsesrc.html), [queue](https://gstreamer.freedesktop.org/documentation/coreelements/queue.html), [webrtcbin](https://gstreamer.freedesktop.org/documentation/webrtc/index.html), [audioresample](https://gstreamer.freedesktop.org/documentation/audioresample/index.html);
  - letto: `https://raw.githubusercontent.com/GStreamer/gstreamer/1.24.2/subprojects/gst-plugins-base/docs/plugins/gst_plugins_cache.json`, `.../gst-plugins-good/docs/gst_plugins_cache.json` e i file `.c` citati allo stesso tag;
  - confronto di versione: `https://raw.githubusercontent.com/GStreamer/gst-plugins-base/1.16.0/ext/opus/gstopusenc.c` (senza `restricted-lowdelay`) e `.../1.18.0/...` (con).
- FFmpeg 6.1.1:
  - pagine: [ffmpeg-devices](https://ffmpeg.org/ffmpeg-devices.html#pulse), [ffmpeg-codecs](https://ffmpeg.org/ffmpeg-codecs.html#libopus-1), [ffmpeg-formats](https://ffmpeg.org/ffmpeg-formats.html#matroska), [ffmpeg-protocols](https://ffmpeg.org/ffmpeg-protocols.html#http);
  - letto: `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n6.1.1/doc/{indevs,encoders,muxers,protocols}.texi` e `libavdevice/pulse_audio_dec.c`, `libavcodec/libopusenc.c`, `libavformat/matroskaenc.c`.
- libopus 1.4: [API encoder CTLs](https://opus-codec.org/docs/opus_api-1.4/group__opus__encoderctls.html); letto `https://raw.githubusercontent.com/xiph/opus/v1.4/include/opus_defines.h` e `src/opus_encoder.c`.
- RFC 7587: [rfc-editor](https://www.rfc-editor.org/rfc/rfc7587). Testo verificato nel draft `https://raw.githubusercontent.com/xiph/opus/v1.1/doc/draft-ietf-payload-rtp-opus.xml` (-01); la corrispondenza con l'RFC finale per la tabella dei bitrate è confermata solo da un risultato di ricerca.
- PulseAudio 16.1: [pulse-daemon.conf(5) Ubuntu noble](https://manpages.ubuntu.com/manpages/noble/man5/pulse-daemon.conf.5.html); letto `https://raw.githubusercontent.com/pulseaudio/pulseaudio/v16.1/src/...` (`modules/module-null-sink.c`, `modules/module-always-sink.c`, `pulsecore/{sink,source,source-output,protocol-native,core-rtclock}.c`, `pulse/stream.c`, `utils/pactl.c`, `daemon/{daemon.conf.in,default.pa.in}`).
- SDL: `https://raw.githubusercontent.com/libsdl-org/SDL/release-3.2.24/src/audio/{SDL_audio.c,SDL_sysaudio.h,pulseaudio/SDL_pulseaudio.c,dummy/SDL_dummyaudio.c}`, `src/SDL_hints.c`, `include/SDL3/SDL_hints.h`; confronto con `release-3.2.30` e `release-3.4.0`.
- WebRTC:
  - [W3C webrtc-stats](https://www.w3.org/TR/webrtc-stats/) (letto `https://raw.githubusercontent.com/w3c/webrtc-stats/main/webrtc-stats.html`);
  - [W3C webrtc-pc](https://www.w3.org/TR/webrtc/) (letto `https://raw.githubusercontent.com/w3c/webrtc-pc/main/webrtc.html`);
  - libwebrtc: `https://raw.githubusercontent.com/mozilla-firefox/firefox/main/third_party/libwebrtc/api/audio_codecs/opus/audio_decoder_opus.cc`, copia vendorizzata dell'upstream [webrtc.googlesource.com](https://webrtc.googlesource.com/src/+/refs/heads/main/api/audio_codecs/opus/audio_decoder_opus.cc);
  - Firefox: `https://raw.githubusercontent.com/mozilla-firefox/firefox/main/dom/media/webrtc/jsep/JsepCodecDescription.h`.
- Pacchetti Ubuntu noble: `https://packages.ubuntu.com/noble/{gstreamer1.0-plugins-base,gstreamer1.0-plugins-good,gstreamer1.0-pulseaudio,pulseaudio,ffmpeg,libopus0}` e i relativi `amd64/.../filelist`.
- ITU-R BT.1359: [itu.int](https://www.itu.int/rec/R-REC-BT.1359). Non scaricato; valori dai risultati di ricerca, quindi NON VERIFICATO in prima persona.