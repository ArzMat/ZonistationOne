# Changelog

All notable changes to ZonistationOne are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### 2026-10-02: follow-up of `docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md`

Every item below rests on a psx-spx line (clone at commit `00d5dcb`, cited as `<path>:<line>`) or on
a measurement in this environment. Nothing here was run against a BIOS or a disc: the checks are
the unit tests (`make test`, 8 programs) and the new bare-metal hardware tests (`make hwtest`, 15
checks on the OpenGL and the Vulkan renderer), which fail on the code before these changes and
pass after them. The list of what still needs a real run is at the end of this block.

#### Fixed: audio in in-engine cutscenes
- **XA sectors the filter rejects are discarded, not handed to the CPU.** With the XA filter on, an
  audio+realtime sector of another file or channel became an INT1 carrying ADPCM data and moved the
  GetlocL location onto a channel the game was not playing: N-1 spurious interrupts in N on a
  multi-channel dialogue bank, which is what an in-engine cutscene plays from. Routing now follows
  the documented algorithm (cdr/cdromdrive.md:590-612): CD-DA and non-Mode 2 sectors never reach
  the ADPCM decoder, and GetlocP follows every sector the head passes, with the LibCrypt rule
  unchanged.
- **The drive keeps feeding XA while an interrupt is pending.** Any unacknowledged INT, even the
  INT3 of a polling GetlocP, stopped the whole drive and put holes in the speech; now only data
  delivery waits for the acknowledge (:605, :758-765). `ZS1_CD_XA_HOLD=1` restores the old
  behaviour.
- **SPU streaming IRQs fire where the hardware fires them.** The voice IRQ compared IRQA with the
  block after the one read, so an IRQ address at the start of a looping buffer never fired and a
  double-buffered stream never refilled; writing IRQA or TSA raised IRQs no access caused; and
  acknowledging I_STAT also cleared the SPU's own flag. The IRQ now traps the block being read,
  silent voices keep reading and can raise it, and only SPUCNT.6 = 0 acknowledges it
  (spu/soundprocessingunitspu.md:635, :823-852).
- **SPU loops follow the ADPCM flags as documented.** Key On no longer forgets a repeat address
  written before it, Loop End always jumps to it, and a software write no longer disables Loop Start
  flags until the next Key On (:131-182). A streaming ring without a Loop Start flag used to play
  once and go silent.
- **Volume sweeps are the documented envelope.** A linear fade-out ran past 0 and came back at full
  volume with the phase inverted, heard as the previous scene's sound returning; it now stops at 0,
  the phase bit and the exponential increase steps work (:447-482), and the main volume sweeps at
  all (:405-432), so a scene that fades in from 0 is no longer silent.
- **Mute, ADPMUTE, the ATV matrix and a CD volume of 0 now act.** The CD controller's output stage
  had no caller and AVOL 0 played at full volume, so audio a game had silenced stayed audible across
  a cut. SPUCNT.14 now mutes the voices and leaves the CD alone (:631). A one-time INFO line says so
  if CD audio arrives while AVOL is 0.
- **Uploads written in Stop mode reach SPU RAM.** The documented manual-write sequence fills the
  FIFO before selecting Manual Write (:715-721); those halfwords were dropped.
- **AutoPause, Read after Pause and ADPBUSY.** AutoPause compares this sector's SubQ track with the
  previous one instead of a function-static that paused a new Play at its first sector; Read after
  Pause returns the last received sector again (cdr/cdromdrive.md:811-814); HSTS.2 reports XA
  playback (:62).

#### Fixed: FMV and GPU
- **The MDEC reset keeps the quant and scale tables.** The reset bit zeroed them, so a game that
  resets between two movies without re-sending them decoded the second as flat grey
  (cpu/mdec/macroblockdecodermdec.md:116-117).
- **MDEC(0) and MDEC(4..7) take no parameters.** They swallowed cw&FFFFh words: FE00h padding read
  as a command (FE00FE00h) ate the next frame whole. Status bits 15-0 now read words remaining minus
  1, FFFFh when idle, the MDEC(0) count, 0000h after reset (:43, :58, :119-126).
- **FMV colours are clipped where the documentation clips them.** The IDCT output is no longer cut
  to 9 bits block by block, chroma included; Y+R, Y+G, Y+B saturate after the sum (:235-238). The
  literal 9-bit wrap is available with `ZS1_MDEC_WRAP9=1`: taken as written it turns a Y+R above 255
  black, and no hardware capture confirms it. Signed output no longer smears G and B, and the IDCT
  uses the upper 13 bits of the scale table (:317-318).
- **GP0(02h) fills reach the screen exactly.** They were rasterised as quads, clipped to the drawing
  area and passed through GP0(E6h): a 24bpp FMV buffer cleared wider than the drawing area stayed
  dirty on screen, and with E6h.0 set black came out as 8000h. A fill is now a VRAM write pushed to
  the renderer as an upload.
- **VRAM transfers wrap at the edges of VRAM.** GP0(A0h) dropped pixels past column 1023 or line
  511, and rectangles hanging past line 511 made the renderers read past the end of the CPU copy of
  VRAM. GP0(A1h..BFh) and (C1h..DFh) now act as A0h/C0h (gpu/memory-transfer-commands.md:3-5,
  :95-98).
- **With GP0(E6h).1 set, uploads and copies test the real destination.** The mask bit was read from
  the CPU copy, which never holds rasterised pixels.
- **On Vulkan, GP0(80h) and GP0(C0h) read the current field and write the right place.** The
  synchronous readback ignored the primitives drawn so far in the field, and wrote the rect packed
  at the top-left of VRAM instead of in place. It now replays the pending ops first and writes in
  place like GL; its buffer no longer outlives the device across a renderer switch. The texture
  window offset is also ANDed with its mask, as on GL (gpu/rendering-attributes.md:110).
- **An empty drawing area draws nothing on GL** (gpu/rendering-attributes.md:139-140); it was
  widened to a one-pixel column.
- **Savestates keep what the renderer drew.** The save reads the whole of VRAM back first; a screen
  drawn once and left came back empty after a load. Format version 12: v11 states are refused.
- **DMA MADR and BCR follow the transfer** (system/dmachannels.md:23-32, :56-58), and a DPCR write
  no longer restarts a GPU transfer that is still running, which sent its data twice. 16-bit reads
  of the MDEC status' upper half no longer read 0.
- **GP1(10h) answers the right register.** The indices were one off (index 4 returned the top left
  of the drawing area); GPUREAD keeps its value across reads and GP1(40h..FFh) mirror GP1(00h..3Fh).

#### Fixed: CPU
- **SWL/SWR no longer read memory.** Hardware drives byte enables and reads nothing
  (cpu/cpuspecifications.md:284-287); the read-modify-write charged a load stall on RAM and could
  read an I/O register for real.
- **The BIOS TTY capture no longer perturbs the machine.** It read guest strings through the CPU's
  load path, charging stalls and tripping watchpoints; it now peeks RAM and ROM.
- **Timer MODE reads see a boundary already crossed**, as counter reads do.

#### Performance
- **Emulation no longer waits for the previous field's buffer swap.** `main.c` waited for the GPU
  thread before emulating, so a swap blocked on vblank was added to every field instead of
  overlapping it. The wait now protects only ImGui's draw data and sits after emulation.
  `ZS1_VSYNC=0|1|-1` sets the GL swap interval or the Vulkan present mode for a repeatable A/B; unset
  changes nothing.
- **`ZS1_FRAME_PROFILE` shows the waits.** The `[PROF]` line keeps its fields and `CPI=` and
  appends `wait`, `ui`, `upload`, `pace` and `frame`, plus a `*_max` for each.
- **The GL backend stops uploading 2 MB of VRAM nobody reads.** With a texture barrier the R16UI
  mirror has no reader, so the per-field and after-fill full uploads, which could also run the pool
  out and drop real uploads, are skipped.
- **GL batches cost a fraction of the GL calls**: vertices go up once per replay, draws start at
  their own offset, the texture barrier runs only before a batch that reads VRAM after a write, and
  state is set only when it changes. **Vulkan batches like GL**: its setters flushed unconditionally,
  so every primitive was a batch with its own barrier.
- **GP0(80h) and GP0(C0h) read VRAM back only where something may have been drawn**, from a map of
  16x16 tiles; `ZS1_FORCE_READBACK=1` restores the old behaviour.
- **The interpreter does less per instruction**: switch dispatch, inline register access, an 8 KB
  exec trace instead of 64 KB, no debugger call when nothing is set, Cause written only when it
  changes. **CPU loads from RAM take a fast path** with the same value, stall and watchpoint
  behaviour; the DMA loops are deliberately untouched. CPI is unchanged.
- **Log lines that cannot print cost nothing**: the level test moved to the call site, TRACE is
  compiled out unless built with `make LOG_MAX_LEVEL=TRACE`, and log files are fully buffered and
  flushed per field and on any crash signal.
- **MDEC decoding is about 2.7x cheaper**, bit-exact with the previous structure; GP0(A0h) does one
  division per word instead of four; the SPU skips idle key scans, reverb modulos and zero FIR taps
  with identical arithmetic; the VRAM viewer pass runs only while the viewer is open.
- **LTO stays on for C when gcc and g++ differ in major version** (C objects with LTO, C++ without,
  linked by the C driver), and `make PGO_GEN=1` / `make PGO_USE=1` build a profile-guided binary.
- **Lua probes subscribe to the events they need**: `emu.on_event(fn, "vblank", ...)` filters in C.

#### Fixed: the cluster's audio path (`deploy/session/`)
- **Chrome decodes the session audio in stereo.** `webrtc.html` asks for `stereo=1`, without which
  libwebrtc decodes Opus as mono; the page also shows the audio stream's bitrate, jitter-buffer
  delay, concealed share and channel count.
- **Audio drops are counted, and a 20 ms stall no longer drops audio.** The audio queue is
  time-bounded at 60 ms and logs its drops every 10 s.
- **Opus settings come from `ZS1_WEBRTC_AUDIO_*`**; the manifests use 20 ms frames on the WAN (116
  kbit/s on the wire instead of 136 for the same payload).
- **One resample, always in the same place**: the null sink is created at 48 kHz with
  speex-float-5. ffmpeg's `-fragment_size` is in bytes, so 480 was 2.5 ms; it is 1920 (10 ms).
- **Session pods request one CPU** (Burstable instead of BestEffort). SDL stays at 3.2.24; the
  3.4.0 measurement is written in the Dockerfile.

#### Added
- **`make test`** builds and runs eight self-contained unit tests (CPU decode, RAM fast path and
  SWL/SWR, log gating, MDEC, VRAM rectangle split and tile map, DMA writeback, SPU, CDROM); the old
  target pointed at a file that did not exist.
- **`make hwtest`** runs bare-metal PS-X EXEs inside the emulator on a zero BIOS, on both renderers
  (`tests/hw/`, needs `gcc-mipsel-linux-gnu` and `xvfb-run`).
- **`ZS1_DMA_STALL=doc`, a documented DMA cost model (opt-in).** DMA no longer charges the CPU a
  load stall per word read; the CPU keeps running and waits only when it reads RAM or I/O during a
  transfer (system/dmachannels.md:205-238). Off by default because it changes emulated timing.
- **Capture buffers in SPU RAM** (CD left/right, voices 1 and 3, with IRQ), **`emu.spu_voice(n)`,
  `emu.spu_irq()`, `emu.cd_state()`**, **`scripts/cutscene_audio_classify.lua`**, and
  **`ZS1_SPU_RING_TARGET=<frames>`**. `audio_timeline.lua` and `spu_pop_capture.lua` read
  `emu.cd_audio()` in its real order.

#### Needs a run with a BIOS and a disc
- **A CD volume of 0 now means silence.** If a title relied on the old "0 = full", its XA or CD-DA
  is now silent; the one-time INFO line in the log says so. Check the BIOS CD player, Ace Combat 2's
  FMV and Dino Crisis.
- **The LSAX latch is gone (SPU loops).** psx-spx does not describe it, but
  `docs/study/SPU_2026-07-29.md` records that another emulator keeps one; first suspect if a game
  loses a loop.
- **The main-loop reorder (P1)** should be measured with `ZS1_FRAME_PROFILE=1` (now with `wait=`)
  on both GPUs, with and without `ZS1_VSYNC=0`.
- **`ZS1_DMA_STALL=doc`** needs boot milestones in emulated fields against the reference run
  before it can become the default.

### Performance
Host cost per PAL field, measured with `ZS1_FRAME_PROFILE` and nothing else, median of three 30 s
runs of the same scenario. The emulated machine is unchanged throughout: CPI stays at 1.618 with
identical percentiles, which is the check that a host optimisation has not moved a guest property.

| | `emu` | `total` |
|---|---|---|
| before | 3.710 ms | 3.850 ms |
| after | **2.910 ms** | **3.040 ms** |

- **Link-time optimisation**, auto-enabled when the C and C++ compilers share a major version and
  skipped with an explanation when they do not — a default that fails to link is not a default.
  Worth −7.7% on its own, and it removes `debugger_check_breakpoint`,
  `debugger_check_read_watchpoint`, `mask_region` and `cpu_reg` from the profile entirely by inlining
  them across translation units. Those three debugger hooks were 2.92% of all samples doing nothing
  at all, since neither a breakpoint nor a watchpoint was set.
- **The interpreter's second register file is gone.** Every write went to `out_regs` and every
  instruction ran `memcpy(regs, out_regs, 128)` to commit it — the largest single constant on the hot
  path at ~20M instructions a second. Removing it was checked rather than assumed: every handler in
  `cpu_instructions.c` reads its sources before writing its destination, and the only reads that look
  later are arguments of the write itself, which C evaluates first.
- `make compile_commands` writes a `compile_commands.json` from the same source lists the build uses.
  Without it a language server guesses the include path and reports dozens of bogus
  "identifier uint32_t is undefined" errors in `include/cpu.h`, a header that
  `gcc -fsyntax-only` accepts on its own.

### Added
- **The emulator runs as a Kubernetes workload**, one pod per session, each holding a GPU, its own X
  server and its own memory cards. `deploy/k3d-cuda/` builds the cluster and `deploy/session/` builds
  the session image; both were verified against a local k3d cluster (1 server, 3 workers) on an
  RTX 4060, booting `Ace Combat 2 (Europe)` and `Crash Bandicoot 3` side by side from a single
  `kubectl apply`. A session is selected entirely by environment — `ZS1_GAME` picks the disc,
  `ZS1_SESSION` names the working directory — so two sessions differ only in their manifest. Three
  run side by side: Ace Combat 2, Crash Bandicoot 3 and Dino Crisis, one per worker node.

  Nothing about the machine's content is in an image. The BIOS and the discs stay on the host, reach
  the nodes as bind mounts, and are exposed to one namespace by PersistentVolumes whose `claimRef` is
  pinned in advance and mounted `readOnly`; the sessions are ClusterIP-only, so reaching one needs
  cluster credentials. An image can be pushed to a registry, a bind mount cannot.

  Four things had to be true before any of it worked, none of them in this project's code:
  - The **NVIDIA container runtime must be in the k3s node image**. `rancher/k3s` is busybox with no
    package manager, so it is laid over a CUDA base — `/bin` and `/lib` mapped onto their usr-merged
    destinations, because they are real directories in one image and symlinks in the other.
  - **Docker Desktop cannot host it.** Its daemon runs in a LinuxKit VM with no NVIDIA passthrough,
    while `nvidia-ctk runtime configure` writes to the *system* daemon's config — so the toolkit
    looks correctly installed and `docker info` still lists only `runc`.
  - **A native `k3s.service` on the same host collides with k3d.** Both take `10.42.0.0/16` and
    `10.43.0.0/16`, and the host's iptables then answer the cluster's own service IP with a foreign
    certificate. Every system pod fails `x509: certificate signed by unknown authority` while
    `kube-root-ca.crt` and the k3d server's CA match byte for byte — the tell is that
    `openssl s_client` against the service IP and against the node's `:6443` return different
    `k3s-server-ca@<epoch>` issuers. The cluster now uses `10.44`/`10.45`.
  - **`eviction-hard` alone does not lift a disk-pressure taint.** k3s defaults
    `eviction-minimum-reclaim` to 10%, so the kubelet holds `DiskPressure` until free space reaches
    threshold *plus* reclaim. Lowering the threshold and watching the node stay tainted with the disk
    visibly above it is the symptom; both have to move together.

  Memory cards are why `ZS1_SESSION` exists rather than being cosmetic: `interconnect.c` opens
  `memcard1.mcd` and `memcard2.mcd` by fixed name relative to the CWD, and every boot rewrites the
  card as part of the card driver's write test, so two sessions started in one directory would
  destroy each other's saves on the first boot.

  **The Vulkan backend is what makes headless rendering work**, and it needs nothing added to get
  there. Xvfb serves no NVIDIA GLX extension, so OpenGL resolves through libglvnd to
  `libGLX_mesa.so` and the 4060 sits idle while a software rasteriser draws; Vulkan does not go
  through GLX at all, and the ICD the container runtime drops in `/etc/vulkan/icd.d` is enough for
  the device to come up as the RTX 4060 with a 1280x720 swapchain and
  `VK_EXT_fragment_shader_interlock` available. The manifests set `ZS1_GFX=vulkan`. The usual
  answers to headless GL — VirtualGL, or an Xorg carrying the NVIDIA driver — are not needed.

  **WebRTC carries picture and sound together**, H.264 on the 4060's NVENC block,
  50 fps to match a PAL field, keyboard forwarded through the X server. Working end to end. Four
  defects stood between the first version and that, and none of them announced itself:
  - `Gst.Promise.new_with_change_func` was given two user-data arguments, the pattern in the older
    GStreamer examples. This binding takes one, rejects the call inside the C callback, and reports
    nothing — so `create-offer` ran, its reply handler never did, and no offer was ever sent. The
    pipeline reached PLAYING and `on-negotiation-needed` fired correctly the whole time.
  - `offer.sdp` was read *after* `set-local-description`, which takes ownership of the message and
    leaves None behind.
  - The page picked its signalling endpoint by testing for a standard port, but the Ingress is
    published on 8081, so it chose the port-forward route and dialled a port nothing served.
  - Every ICE candidate was a pod address (`10.44.x.x`) or link-local, and the media is UDP that no
    Ingress carries. `hostNetwork` moves the pipeline onto the node's own `172.19.0.x`, which the
    host reaches over the Docker bridge; anti-affinity keeps sessions off a shared node, since the
    ports are now the node's.

  It is served over HTTPS on the tailnet through `tailscale serve`, with a real certificate and no
  port forwarding — tailnet membership is WireGuard keys per device, which is a stronger front door
  than the password behind it. The certificate needs the tailnet's HTTPS setting enabled *before*
  `tailscaled` starts; enabled afterwards it keeps serving a self-signed one and the proxy falls back
  to it without saying so, which reads as a TLS failure and is a stale cache.

  A browser opening a WebSocket does not attach the credentials already entered for the page, so the
  signalling socket is refused behind basic auth. Credentials in the URL are the one form a browser
  does send, and JavaScript cannot read the ones already typed — so the page asks once, and only
  after a socket has actually been refused.

  Sound also reaches the browser on a second port for the VNC page: the SPU's output off a PulseAudio null sink, encoded
  as WebM/Opus by ffmpeg, with `play.html` putting it on one page with the noVNC picture. Verified
  at the sink rather than assumed — `mean_volume -18.7 dB`, `max_volume -5.3 dB` over a four-second
  capture, so the SPU is genuinely feeding it.

  It is not synchronised with the picture and cannot be: two transports, no shared clock. The gap is
  dominated not by the encoder but by the browser's media buffer, which grows without bound on a
  progressive stream and settles a second or more behind. So the page chases the live edge — 5%
  playback rate for small drift, a seek for a large one — and reports the measured lag, which holds
  in the low hundreds of milliseconds. One transport carrying both is the WebRTC work.

- **A Vulkan 1.3 renderer, and the ability to swap renderers while a game is running.** The GPU had
  one OpenGL 3.3 implementation that owned its own header; `<GL/glew.h>` came in through
  `include/renderer.h`, which `gpu.h` includes, so `GLuint` reached every translation unit that
  touched the interconnect — `debug_ui.cpp` included, where texture names were cast to `ImTextureID`.
  There are now two backends behind one vtable and no graphics type above it.

  **The abstraction** (`include/gpu_backend.h`). A 38-entry `GfxBackend` of function pointers, with
  the vertex structs moved out of `renderer.h` and retyped from `GLshort`/`GLubyte` to
  `int16_t`/`uint8_t` — the same layout, without the API. What ImGui receives for a texture is an
  opaque `GfxTexHandle`: a texture name on one backend, a `VkDescriptorSet` on the other.
  `src/gpu/renderer.c` became a dispatcher and the OpenGL implementation moved to
  `src/gpu/renderer_gl.c`; the ~120 `renderer_*(&gpu->renderer, ...)` call sites did not change,
  which is what kept the diff readable.

  Two things about the split are not obvious. `renderer_select_backend()` is separate from
  `renderer_init()` because `gpu_reset_state()` pushes GP0/GP1 reset values through four setters from
  inside `interconnect_init()`, before any device exists, and those values are not redundant — the
  backend resolves a NULL `impl` to its own file-static state so they land where init will find them.
  And savestates were unaffected: `savestate.c` derives both `Gpu` spans from `offsetof(Gpu,
  renderer)` and `sizeof(Renderer)`, so shrinking `Renderer` from ~1 MB to two pointers moved both
  boundaries together. Verified by loading a state written before the change — PC and cycle exact.

  **The Vulkan backend** (`src/gpu/vk/`). Vulkan 1.3 with dynamic rendering, so there is no
  `VkRenderPass` anywhere. The loader is opened at runtime through `SDL_Vulkan_LoadLibrary()` under
  `VK_NO_PROTOTYPES`, so nothing links against `libvulkan` and the binary starts on a machine with no
  driver installed. The three GLSL programs left their C string literals for
  `src/gpu/shaders/*.{vert,frag}`, compiled to SPIR-V by `glslangValidator` at build time; the
  Makefile builds GL-only with a message when `libvulkan-dev` or `glslang-tools` is missing, rather
  than failing.

  The feedback loop — the PS1 fragment shader samples the VRAM image that is also its colour
  attachment — turned out to be less of a problem than planned for.
  `VK_EXT_fragment_shader_interlock` is an optimisation; a `vkCmdPipelineBarrier` between batches
  with the image permanently in `VK_IMAGE_LAYOUT_GENERAL` is correct everywhere and is what ships.
  That is `glTextureBarrier()` in Vulkan terms. Semi-transparency keeps GL's two-pass shape, because
  blend state is per-draw while the STP bit is per-texel; `dualSrcBlend` would collapse it and is
  left for after parity is proven across more than one frame.

  Parity is a byte comparison of the same dumped frame from each backend, and at frame 900 of
  `Ace Combat 2 (Europe)` under `SCPH-7502` the two are identical.

  **The switch** (`switch_gfx_backend()` in `main.c`, *Video* in the quick menu). SDL fixes
  `SDL_WINDOW_OPENGL` and `SDL_WINDOW_VULKAN` when a window is created and neither can be added
  later, so changing API means a new window, not just a new context. Three things cross it:

  - **VRAM**, read back into host memory before the device goes and re-uploaded after. It has to come
    from the GPU — `gpu.vram.data` holds what the CPU wrote and never what the rasteriser drew — and
    since the readback is a synchronous round-trip through the GPU thread, it happens while that
    thread is still alive.
  - **The drawing state**, through a new `gpu_reapply_renderer_state()`: draw offset, drawing area,
    texture window, screen scale, display region, depth24, blank and the two mask flags. All ten live
    in the backend rather than in `Gpu`, and the guest has no reason to re-send `GP0(E2..E6)` because
    the host changed API. The per-primitive state is deliberately left out — every draw command sets
    dither, semi-transparency and texture mode immediately before pushing geometry.
  - **The ImGui context** — fonts, `imgui.ini`, the docking layout, the pinned watches — through
    `debug_ui_backend_init()`/`debug_ui_backend_shutdown()`, which touch only the platform and
    renderer halves. `ImGui::DestroyContext()` stays where it was.

  The emulated machine is not involved: no reset, no save state, and the CPU, SPU and drive never
  learn anything happened. A backend that fails to come up is rolled back to the previous one and
  reported. `ZS1_GFX_SWITCH_TEST=<n>` flips backends every n fields; nine switches in one run of
  Ace Combat 2 under X11 were clean.

  **The interface** is *Esc → Video*: the live backend, the GPU and driver the machine actually got,
  the output mode, and buttons for each backend and each device it offers. Vulkan enumerates real
  devices from `vkEnumeratePhysicalDevices` and switches between them live; OpenGL cannot, because
  the GLX vendor library is resolved at the first `dlopen` of libGL, so its two PRIME choices are
  listed marked *next launch*. A control that says so is worth more than one that looks live and does
  nothing. Vulkan runs now report their device through `host_info`, so the Host HW panel stops
  showing a GL string on a Vulkan run.

  Two platform limits found on the way, both environmental rather than defects: the OpenGL backend
  will not start under `SDL_VIDEODRIVER=wayland` here (`glewInit()` returns "Unknown error"), and
  Vulkan on the Intel iGPU will not create a swapchain under X11 while the screen is owned by the
  NVIDIA driver in dGPU mode. Each backend has one session type that works, and a hot switch to a
  backend that cannot start rolls back rather than failing the run.

- **`Dino Crisis (Europe)` [SLES-02207] reaches its main menu**, from a `.bin.ecm` plus its `.sbi` —
  the first LibCrypt-protected disc to run here. Five separate defects stood between the disc booting
  and the menu appearing, all listed under *Fixed* below; only one of them was in the CDROM.
- **LibCrypt discs run, given their `.sbi`.** `Dino Crisis (E)` [SLES-02207] boots and reaches its
  intro. The protection stores a 16-bit key as deliberately wrong subchannel Q on 32 sectors of
  track 1; Q is not part of the 2352-byte sector, so no `.bin` dump carries it and the patches
  travel beside the image in an `.sbi`. `cdrom_disc_load_sbi()` reads the container (4-byte `SBI\0`
  magic, then 14-byte records of BCD MSF + type + ten Q bytes) and `cdrom_disc_get_subq()` answers
  from a patch where one covers the sector.

  The file is looked for by the image's name with the extensions dropped one at a time, then — only
  if exactly one exists — the lone `.sbi` beside the image, because redump names it after the disc's
  serial rather than after the dump. `ZS1_SBI=<path>` overrides the search. The serial matters:
  `SLES-02210` is the Italian pressing of the same game and its patches sit on different sectors, so
  the wrong file loads cleanly and protects nothing.
- **A gameplay shell.** The window now has two shells and switches between them at any time — the
  backquote key, Shift+F1, the *Gameplay* button on the machine bar, or the quick menu — with the
  machine running across the switch. The gameplay shell shows the emulated screen and nothing else:
  a HUD (fps, speed, host frame ms, region and refresh, the pad's LED) that fades after ~3 s idle and
  returns on any input, and `Esc` for a quick menu with session counters, four save-state slots, pad
  mode, a machine summary and quit. It owns no machine state: `Esc` reaches it through
  `debug_ui_escape_pressed()` and the menu parks requests that `main.c` carries out, the same way a
  Lua script's `emu.load_state()` does. `ZS1_UI=debug` opens straight into the workspace.
- **The pad is drawn.** The Controller window shows a DualShock — grips, cross, symbol buttons, four
  shoulders, both sticks with their live deflection, and the Analog LED in its documented colour —
  lit from the same 16-bit word the SIO sends the game. Clicking a control rebinds it; the scancode
  table is still there, folded away.
- **Pinned watches are real.** The four tiles were constants. They are now Lua expressions evaluated
  through the new `lua_debug_eval_expr()` — the whole `emu.*` surface — every sixth frame and only
  while the panel is visible, with the error text shown in the tile instead of a console line per
  refresh. Click a tile to edit, right-click to remove, up to eight.
- **`src/core/host_info.c`** reads the host from `/proc`, `/sys`, `uname` and the live GL and SDL
  device strings: machine, CPU model and core count, kernel, memory, the GL renderer and driver,
  whether a `ZS1_GPU` request was honoured, the audio device's own format and buffer, and per-thread
  CPU from `/proc/self/task`. The threads are named (`GPU`, `cdrom-read`) so the list is readable.
- **Interface scale.** A UI face, a display face and a real monospace face are loaded at a factor
  taken from `SDL_GetWindowDisplayScale()` (or the window's pixel height), with
  `ImGuiStyle::ScaleAllSizes` to match; `ZS1_UI_SCALE` overrides it. 15 px on a 1440p panel was a
  squint, and the "monospace" log windows were pushing the proportional face — `Fonts[0]` is the UI
  font, so the toggle did nothing.
- **The Frame view holds a frame** (a copy, so recording carries on), plots events on a millisecond
  axis, shows each row's payload beside its count, draws the display flip across every row as the
  anchor the rest is read against, and draws the budget line only when the frame overran it. Hovering
  a tick reports its time, cycle and payload.
- `mdec_stat_macroblocks()` — macroblocks pushed out since boot, kept as a file-static rather than a
  field in `Mdec`, because the savestate writes that struct as one sized span and would refuse every
  existing v6 state for the sake of a counter the UI reads.

- **ECM disc images** (`.bin.ecm`), decoded on the fly. A lookup table built at load time maps each
  decoded sector to its position in the compressed stream, so random access costs one `fseek`; the
  sector's own MSF is reconstructed from the absolute LBA, since the format strips it. Written from
  the format specification — no code from any third-party ECM tool.
  Correctness is not taken on trust: the container appends the EDC (CRC32, polynomial `0xD8018001`)
  of the *entire* decoded output as its last four bytes, so decoding a whole image and running that
  CRC over the result proves the reconstruction byte-identical to the original `.bin`. On
  `Crash Bandicoot 3 - Warped (E)` — 146,562 sectors, 344,713,824 bytes — it matches at `6264BE2A`,
  in 1.8 s.
  `ecm_edc_init()` was missing its one call site, which left the EDC and ECC lookup tables zeroed and
  every regenerated error code zero with them. Latent for a title that never inspects them, but the
  image was not bit-exact until it was fixed, and the whole-file check only passes with the fix in.
- **`docs/ecm_libcrypt_discovery.md`** rewritten from scratch. Its earlier version concluded that the
  Crash 3 disc error was Sony's LibCrypt protection and that `.sbi` support was needed. That was
  wrong, and the document now records how it was disproved — the game issues no `GetlocP`, `SeekP`,
  `GetQ` or `ReadTOC` in a 34 s run, its boot executable contains no immediate load of any of those
  command numbers, and the garbage MSF `df:e7:d7` occurs exactly once in the whole 345 MB image, in a
  sector the game never reads. It was a CPU bug; see LWL/LWR under Fixed.
- **Two subsystem audits against the official psx-spx clone**, both built on the rule that no entry
  may claim "correct" without citing a documentation line *and* a code line, with everything else
  marked `UNVERIFIED`. `docs/CDROM_AUDIT_2026-08-17.md` now covers all five CDROM doc files and every
  file in `src/cdrom/` plus the drive's paths in `bus.c`/`bus_irq.c`.
  `docs/DMA_IRQ_GTE_MDEC_AUDIT_2026-08-17.md` does the same for DMA, interrupts, the GTE (including
  the pipeline-timings page), MDEC and the hardware-numbers catalogue.
- `ZS1_OVERSCAN=0` disables the new overscan crop; `ZS1_DMA_GPU_PACE=legacy` restores the old flat
  GPU-DMA quantum; `ZS1_DISPLAY_LATCH=1` restores the field-start display latch. All three exist so a
  suspected regression can be A/B'd in one run without a rebuild.
- **`docs/GPU_CPU_VRAM_PATH_STUDY_2026-08-18.md`** — the CPU → GPU → VRAM → screen path written out
  from the official psx-spx clone, with pcsx-redux as a second implementation and a verdict table
  against our code. Written because the display defects were being chased by running the game and
  looking at it; the rules settle most of them on paper. It records where redux and the documentation
  disagree, and why the documentation wins.
- **`emu.vram_map(tile_w, tile_h)` and `emu.vram_row_stats(y, x, w)`** on the Lua surface. The first
  classifies the whole 1024x512 VRAM as a tile grid and returns it as a string — where content lives,
  in one call instead of 524288; the second summarises a single VRAM row, for questions a tile
  averages away. Both read the GPU readback when one has landed, so rasterised primitives are
  included and not just uploads.
- **`scripts/display_map_probe.lua`** (the display mapping, change-only) and
  **`scripts/vram_display_survey.lua`** (the whole of VRAM plus the display window, over a 120-second
  run). Both log at INFO and change-only, so they are readable during play — a DEBUG run writes
  ~1.4M lines and cannot be.

### Fixed
- **The OpenGL backend's `impl` resolver called itself.** `R()` in `renderer_gl.c` read
  `return impl ? R(impl) : &s_gl_renderer;` where it meant `(GlRenderer*)impl` — unbounded recursion
  on every one of the ~120 forwarded calls. It ran correctly anyway, which is the interesting part:
  an infinite loop with no side effects is undefined behaviour in C, so gcc deleted it and the
  function compiled down to `return impl`, which is what was intended. At `-O0`, or under a compiler
  that kept the loop, it would have overflowed the stack on the first draw.
- **A VRAM upload jumped ahead of primitives that were submitted before it.** `renderer_draw()` is
  what turns accumulated vertices into a batch *and* what records that batch's position in the
  frame's op list, so a submitted primitive has no place in the order until something calls it.
  `renderer_upload_vram_rect()` recorded its own op immediately and never flushed, so a draw issued
  earlier was flushed later, took a higher op index, and was replayed on top of the upload.
  Dino Crisis clears its back buffer with `GP0(02)` and then uploads the picture into it in the same
  field: the clear landed after the picture, and its two opening screens and the title art stayed
  black with the images sitting in VRAM the whole time. Measured in one frame — the upload carried
  42116 of 76800 non-black pixels into `vram_tex`, and after that frame's five ops the same rect read
  back 0 of 76800. `renderer_read_vram_rect()` already flushed for exactly this reason; the upload
  side was missed.
- **Timer 1 counted CPU cycles instead of hblanks, and it hung a game.** `timer_rate_cycles()` took
  counter 2's source rule and applied it to all three, but the three rows differ
  (`psx-spx-docs/docs/timers.md:34-36`): source 1 means Dotclock on counter 0 and Hblank on counter 1,
  and only on counter 2 does it mean the system clock. Dino Crisis stalled a second after the BIOS
  handed it control, spinning at `0x80087914` in a stable-read loop that reloads `1F801110h` until
  two consecutive reads agree — they differed by 11 every time, because the counter was advancing
  once per CPU cycle.
- **The COP0 breakpoint registers threw writes away.** BPC, BDA, BDAM and BPCM are R/W
  (`psx-spx-docs/docs/cpuspecifications.md:573-581`); ours ignored `MTC0` and returned 0 from `MFC0`.
  The breakpoint behaviour is still not implemented, but the registers now hold what is written,
  because a game may keep its own data there — psx-spx says as much, and Dino Crisis stores its
  LibCrypt table pointer in BDAM and reads it back with `MFC0`. Reading zero sent its protection
  state machine walking the wrong memory, so it issued `ReadS` with no `Setloc` and swept 36000
  sectors of the disc without ever meeting a protected one. Savestates are **v11**: `Cpu` gained the
  four registers.
- **A LibCrypt sector was reported instead of ignored.** Those sectors carry a wrong CRC, so the
  controller discards their Q and GetlocP keeps answering with the previous sector's position
  (`psx-spx-docs/docs/cdromformat.md`, *CDROM Protection - LibCrypt*) — that repeat is the signal the
  protection counts. Handing the guest the modified values instead yielded a wrong 16-bit key, and
  Dino Crisis walked into its own trap: a routine at `0x80029748` sums 512 words of a decrypted
  sector, compares against `0x283FC505`, and on a mismatch runs `j 0x80029778` forever.
- **`Init` did none of what it is documented to do.** "Sets mode=20h, activates drive motor, Standby,
  abort all commands" (`psx-spx-docs/docs/cdromdrive.md:536-537`); ours pushed a status byte and
  scheduled the second response. The mode kept whatever the last Setmode left, so an Init issued at
  double speed left the drive there until the guest's own Setmode arrived — 50 fields later in the
  Dino Crisis trace — and an ongoing read carried on through the one command whose purpose is to stop
  everything.
- **Loading a savestate erased the memory cards.** `MemoryCard` sits inside `Sio` and `T_SIO` is a
  raw read of the struct, so a state restored its own card images over the live ones. Nothing looked
  wrong at that moment — but the card driver writes frame 63 as a write test during *every* boot,
  which marks the card dirty and rewrites the whole 128 KB file, so a state captured with empty cards
  destroyed a card full of saves one boot later. Savestate format is **v10**: the cards are held
  across the `T_SIO` read and put back, because a memory card is host storage like the disc image and
  nothing on hardware rewinds the card in your hand when the machine is restored.
- **Memory card writes are atomic, and the first write of a session takes a backup.** The save wrote
  straight over the `.mcd`, so a crash mid-write left a truncated card. It now writes a temp file and
  renames it, and copies the file to `<path>.bak` before the first overwrite — the card as the
  session found it.
- **Get ID (53h) answered two bytes out of step.** The card state machine ran every command through
  the address byte-pair, but Get ID takes none: after ID2 the card owes 5Ch, 5Dh, 04h, 00h, 00h, 80h
  (`psx-spx-docs/docs/controllersandmemorycards.md:2386-2397`). The host read 00h and the echoed
  address where the two acknowledge bytes belong — a card that fails to identify itself, which is
  indistinguishable from an empty slot.
- **An invalid card command kept the bus.** "Transfer aborts immediately after the faulty command
  byte" (`:2409-2415`); the card went on acknowledging instead, so the host waited on a device that
  would not let go.
- **An out-of-range sector read now answers FFFFh and aborts**, as an original Sony card does
  (`:2371-2375`), instead of returning FFh-filled data as though the sector existed.
- **The controller answered on port 2 as well.** The ports are wired in parallel and narrowed by the
  address byte (`:50-57`), so one pad appeared in both — and the BIOS probes both ports.
- **The frame-rate readout was not a frame rate.** It was `1000 / frame_ms` of a single loop
  iteration; with an audio device open the pacing loop waits in `SDL_Delay(1)` steps, so consecutive
  iterations land at 16 ms and 24 ms around the same 20 ms mean and the reciprocal swings between 42
  and 62 while the machine keeps exact PAL time. That is what "60+ fps on a PAL BIOS" was. Both
  shells now count VBlanks over half a second, show it against the mode's nominal rate, and derive
  Speed from the same measurement; the millisecond readout is smoothed.

- **The debug panels were showing invented data.** The Host HW panel carried a hard-coded laptop
  model, CPU, GPU, driver version, kernel, RAM figure and four thread-load bars with constants in
  them; the inspector repeated four of them. A panel that says "RTX 4060" on a machine running on the
  iGPU is worse than no panel, since that line is the first thing checked before a rendering
  difference is blamed on the emulator. Everything there is now read from the kernel and the live
  context.
- **The Pipeline view's status words were literals** — "OK", "READY", "RUNNING", "24 Voices 44.1 kHz"
  — so the view answered the same thing whether the drive was seeking, idle or absent, which is the
  one question it exists to answer. Each node now reports the drive state, the MDEC decode state, the
  DMA channel's sync mode and MADR, GPUSTAT, the display state, the count of keyed-on voices and the
  ring depth, with per-frame figures from the frame event ring.
- The gameplay quick menu drew its backdrop on the foreground draw list, which is painted over every
  window — so the veil landed on top of the menu and the whole panel came out in shadow.
- `micro_label()` letter-spaced every header into a 64-byte buffer, which truncated any header longer
  than about thirty characters. Spacing is now applied only while the label stays a label.

- **LWL/LWR merged against the wrong pipeline slot**, corrupting every unaligned 32-bit load whose
  pair needed a real merge. The two functions read `cpu->load_reg_idx` — the slot for the load *this*
  instruction issues, which `cpu_retire_load_delay()` has just cleared — instead of
  `cpu->delay_load_reg`, the load the previous instruction issued (`include/cpu.h:131-132`). So the
  second half of every pair merged into the stale register. psx-spx is explicit that the pair must
  work: *"There's no delay required between lwl and lwr, so you can use them directly"*
  (`cpuspecifications.md:247-252`), with an example that is exactly an `lwl`/`lwr` pair on one
  register.
  The bug hid because it is data-dependent, not code-dependent: an `(lwl+3, lwr+0)` pair writes the
  whole register twice and looks correct, and only a partial pair such as `(lwl+1, lwr+2)` exposes it.
  `Crash Bandicoot 3 - Warped (E)` walks the ISO path table, whose records alternate alignment, and
  read half its directory extents as `bfc00018` instead of `00000018`. Those are not the `-1` sentinel
  the walk skips, so the game passed one to `CdIntToPos`, got a non-BCD MSF out of a negative LBA, and
  looped on `DiskError: com=CdlSetloc,code=(03:10)` forever. It now boots.
  This is the tail of the load-delay rework below: that change is what moved the pending load into the
  second slot, and these two functions were not moved with it.
- **The pad booted in analog mode, which hardware does not do.** A real analog pad powers up digital
  with its LED off (`DOCS/controllersandmemorycards.md:436`); booting analog was a convenience, on the
  argument that a digital-only game reads the same button bytes and ignores `adc0-3`. True of games,
  false of the BIOS shell: its pad driver does not cope with ID `73h`, never finishes its init, and
  the no-disc main menu is drawn **without its selection cursor**. A reference run of the same PAL
  BIOS answers the pad `41h 5Ah` and the cursor is there. The boot mode is digital again;
  `ZS1_PAD_MODE=analog|stick`, the Analog button (F12 or the DS4 touchpad) and a game's own `44h`
  config command all still switch, exactly as on hardware.
  `ZS1_PAD_MODE=stick` had a latent bug of its own — it set only `stick_mode` and relied on
  `analog_mode` already defaulting to true, so it would have selected digital once the default moved.
- **The no-disc BIOS shell never finished its init.** Status bit 4 is a latch —
  *"Once shell open (0=Closed, 1=Is/was Open)"* (`cdromdrive.md:826`) — and with no disc the shell has
  necessarily been open, so a reference run answers every `Getstat` with `10h`. `cdrom_reset()` forced
  it to `false`, telling the BIOS the tray was shut with a disc in it: it went on to `GetID`, took
  `INT5(08h,40h)` and looped `Getstat`/`Getstat`/`GetID` for the rest of the session. A no-disc boot
  now issues `Getstat` and `Test` only, with no `INT5` at all, matching the reference run command for
  command.
- **`make` did not build.** `compile_commands` is defined before `all`, and make takes the first real
  target as the default goal, so a bare `make` regenerated `compile_commands.json` and left the binary
  at whatever an earlier explicit `make all` had produced. Every source edit appeared to have no
  effect because it was never compiled in — this invalidated most of a debugging session before it was
  noticed. `.DEFAULT_GOAL := all` restores what `README` and `CLAUDE.md` have always claimed.
- **The R3000A load delay was not emulated.** The pending load was committed at the top of the next
  instruction, before it executed, so the delay-slot opcode already saw the loaded value —
  "The loaded data is NOT available to the next opcode, ie. the target register isn't updated until
  the next opcode has **completed**" (`cpuspecifications.md:172-174`). The LWL/LWR code that merges
  with a load still in flight was unreachable for the same reason. Now a two-stage slot retires the
  load *after* the next instruction runs, with the two consequences the same paragraph states: a
  write to the same register by that instruction is the later write and wins, and an exception lands
  the load on the way in, because "the load would complete during IRQ handling, and so, the next
  opcode would receive the NEW value" (`:175-177`). Boot milestones are unchanged to the field
  (`Execute !` at f874, `CD_init` at f1043), which is expected: compiler-generated code never reads
  the delay-slot register, so this protects against code that does rather than altering code that
  does not.
- **The FMV that was being skipped now plays.** Reported from a run of the new build: the scene that
  used to be replaced by black is shown.
- **The picture shook every field.** A per-field display latch was added on the reasoning that
  hardware latches the display registers at the start of the field. The reasoning left out *when* we
  submit: `system_run_frame()` returns the moment VBlank sets `frame_complete` and the frame is
  submitted immediately after, so submit already sits on the field boundary and the live state there
  *is* the field's start state. Latching on top of it handed each frame a value one whole field old —
  on any double-buffered title, and that is every FMV, the value named the buffer being drawn into
  rather than the finished one, so every field presented a half-written image. The latch is off;
  `ZS1_DISPLAY_LATCH=1` brings it back for A/B runs.
- **The display height was doubled for every interlaced title, including the 240-line ones.** The
  VRAM rectangle is twice the window only in 480-lines mode, which is interlace *and* vres=480:
  GP1(05h) states the size with no interlace term at all (`graphicsprocessingunitgpu.md:703-704`),
  GP1(08h).2 is "0=240, 1=480, **when Bit5=1**" (`:772`), and GPUSTAT.31 settles the direction —
  "In 480-lines mode, bit31 changes per frame. And in 240-lines mode, the bit changes per scanline"
  (`:919-920`). Doubling on the interlace bit alone (which is what pcsx-redux does,
  `display.cc:111-113`, and what we had copied) scanned out twice the window on a 240-line title: the
  picture filled the top half of the screen and the rest was whatever else was in VRAM. That is what
  "the VRAM shows up instead of a black screen" looked like.
- **A GPU reset left interlace on.** `GP1(00h)` resets GP1(08h) to 0 and the documented post-reset
  GPUSTAT is `14802000h` (`:645`, `:648`), in which bit 22 is clear — we set `interlaced = true`. So
  both resets, the cold one and the one at the BIOS-to-disc handover, put the machine into interlace
  with vres=240 until the game wrote GP1(08h): precisely the combination that doubled the height. A
  survey run counts two such fields before the fix and none after.
- **GPUSTAT bits 16-18 were rotated.** Bit 16 is Horizontal Resolution 2 and bits 17-18 are
  Horizontal Resolution 1 (`:902-904`); the code packed `(hr2 << 2) | hr1` and scattered that word's
  bits 0/1/2 to 16/17/18, so hr1's low bit landed in the 368 flag and 320 and 640 both read back as
  368. Any game reading GPUSTAT for its own resolution got a wrong answer. Found by cross-checking
  the Lua probe, which derives the width from GPUSTAT, against the width the GPU computes from the
  same registers — 364 against 320 for the same field.
- **GPUSTAT.13 is now forced to 1 when interlace is off**, which the documentation states twice
  (`:897`, `:953`). Without it the field tag stayed wherever 480i had left it.
- **The overscan crop was applied to PAL.** Overscan is an NTSC property — "Many NTSC games display
  240 lines, but on most analog television sets, only 224 lines are visible (8 lines of overscan on
  top and 8 lines of overscan on bottom). Many PAL games display only 256 lines (underscan with black
  borders)" (`:762-765`). A PAL field is already underscanned, so cropping it removed picture the TV
  would have shown: it cost the bottom line of the SCEE screen. The crop is NTSC-only now.
- **The CD audio volume matrix did nothing at all.** ATV0-ATV3 were stored and never read by any
  mixer, so a game's mono/stereo option or CD fade had no effect. On top of that ATV2's port write was
  dropped, ATV1 and ATV3 were swapped, and 1F801803h bank 3 — which is ADPCTL, not a volume register
  (`cdromdrive.md:249-255`) — was stored into the R→R gain, so every CHNGATV write also set that gain
  to 20h. The matrix is now applied at the output stage with saturation up to double volume, ADPMUTE
  is honoured, and muting forces the output to zero instead of starving the audio FIFO — which also
  stops the XA resampler's history from freezing across a mute.
- **The drive answered commands it should have refused, which is what the CD command churn was.**
  `GetlocL` and `Pause` now fail with error 80h during a seek phase — the explicit `SeekL`/`SeekP`
  kind and the implicit one at the start of `ReadN`/`ReadS`/`Play` (`cdromdrive.md:586-588`,
  `:896-901`) — `GetlocL` also fails on audio tracks, `Setloc` validates packed BCD with ss < 60h and
  ff < 75h (`:627-628`), and a re-issued `Init` while one is still owed is dropped with **no**
  response at all rather than acknowledged with an INT3 the drive never sends (`:538-540`).
- **CDROM register reads at 16 and 32 bits hit the wrong registers.** The drive is an 8-bit device
  with BIU auto-increment off, so a wider access repeats the same register: a word read of 1F801800h
  returns HSTS four times (`:315-320`) and a halfword read of RDDATA returns two consecutive data
  bytes (`:118-129`). Reading `addr+1..+3` instead pulled in RESULT and HINTSTS and popped the
  response FIFO as a side effect.
- **Partial writes to the DMA and interrupt registers used RAM semantics.** On-die MMIO ignores the
  byte enables: the CPU drives the source word shifted by the byte offset and the decoder latches all
  32 bits, with the previous contents contributing nothing (`partialwordwrites.md:85-119`). The old
  code merged the written lane into the current value.
- **DMA interrupt bookkeeping.** DICR's master flag no longer factors in the per-channel enables — a
  flag that is set contributes regardless (`dmachannels.md:139-142`) — a completion no longer clears
  I_STAT.3 behind the CPU's back, and the bus-error flag (DICR.15) is finally raised when a transfer
  leaves RAM (`:126`, `:186-192`). The DMA address bound also moved from 2 MB to 8 MB, so transfers
  to the legitimate RAM mirrors are no longer dropped.
- **GPU DMA ran about fifteen times slower than hardware**: a flat 1000 cycles per 64 words against a
  documented 1 clk/word plus a DRAM row load per 16 (`dmachannels.md:194-220`). It now uses the same
  cost model the MDEC path already had.
- **The display window was computed from the wrong register.** Width now comes from GP1(06) with the
  documented divider table (`graphicsprocessingunitgpu.md:687-690`) instead of the GP1(08) resolution
  index, height is Y2-Y1 doubled on interlace, GP1(05)'s X is no longer masked to even halfwords, and
  368-pixel mode decodes. Display state is latched at field start rather than sampled at frame submit,
  so a mid-field GP1 write no longer applies retroactively to the whole field. An overscan crop of 8
  lines top and bottom is on by default, which is where games park the stale VRAM that showed as a
  strip along the top of an FMV.
- **`CLUT out of VRAM bounds` was the validator's bug, not the game's.** It modelled an 8-bit CLUT as
  a 16x16 block; a CLUT is a single strip of 16 or 256 entries on one line, which is what the sampler
  already reads. Every palette parked near the bottom of VRAM was reported as out of bounds — 4662
  times per 250 fields in Monsters & Co.
- **Subchannel Q had no lead-out, no pregap and an underflow.** It now reports track AAh with a
  relative address counting up in the lead-out (`cdromformat.md:229-236`), index 00h with a relative
  address counting *down* through a pregap (`:219-226`), and `PREGAP` lines in the CUE shift every
  later track along the disc while their data stays put in the BIN
  (`cdromfileformats.md:14929-14933`) — gap sectors read back as silence instead of the next track's
  first bytes.
- **The Play report packet had the wrong shape and rate**: nine bytes with both time bases on every
  sector, where hardware sends eight and alternates absolute and in-track time on the documented
  asect values (`cdromdrive.md:1077-1094`).
- **GTE**: `MVMVA` was the one opcode that did not reset FLAG at its start
  (`geometrytransformationenginegte.md:302-303`), so it inherited the previous command's saturation
  bits; `LZCR` was undefined behaviour for `LZCS = FFFFFFFFh` where the answer is 32 (`:261-262`);
  the mx=3 garbage matrix used RT21 where the documentation says RT22 (`:489-491`); and ORGB is
  read-only.
- **CDROM commands that were quietly wrong**: `GetTD`'s track parameter is packed BCD, so every track
  from 10 upwards resolved to the wrong LBA, and out-of-range values now answer error 10h
  (`:926-930`); `Reset` sends INT3 only, with no completion interrupt (`:542-551`); `Sync` and the
  unused opcodes 17h/18h answer INT5(11h,40h) instead of a status; `Pause`'s first response keeps
  bit5 set as it should (`:583-585`).
- **Measured response timings** replace round numbers: first response 0xc4e1 running / 0x5cf4 stopped
  / 0x13cce for Init and ReadTOC, `Stop` 0xd38aca at 1x and 0x18a6076 at 2x, `GetID` 0x4a00
  (`:1877-1905`).
- **The per-category log files had two writers.** "Snapshot" opened `logs/<Name>.log` with `"w"` while
  the session-long handle was still streaming into it, so the file became a block of NULs followed by
  interleaved text and the snapshot was overwritten moments later. Snapshots now go to
  `logs/<Name>.snapshot.log` and flush the streamed file first.

### Changed
- Savestate format is **v9**: v8 added `Cdrom.seek_phase` and `Cdrom.xa_mute` inside the raw CDRH
  range; v9 removed `Cpu.out_regs[32]` and added the second load-delay slot. `T_CPU` is the raw
  struct, so both move every field after the GPRs. Older states are refused rather than restored
  shifted.

### Absent by decision
- **The gameplay shell cannot pick, swap or restart a disc.** No library (the disc still comes from
  `--game=`), no hot swap (`cdrom_load_disc()` exists, but a swap the guest can believe needs the
  shell-open latch, the INT it is owed and the region check, none of which are wired), and no reset
  (there is no `system_reset()` — nothing re-initialises CPU and Interconnect against a live
  machine). *Reset console* was left out of the quick menu rather than bound to something that only
  looks like a reset.
- **Video and audio settings are shown, not offered.** Scaling, crop, scanlines and volume have no
  runtime setter in the renderer or the mixer, and reverb is guest state in SPUCNT rather than a host
  preference. The quick menu carries the controls that exist — pad mode, save-state slots, the
  workspace, quit — and reports the rest.

### Open, in `Dino Crisis (Europe)`
Both found 2026-08-21, both only in the in-engine 3D cutscenes and not in the FMVs, and neither
measured yet.
- **Audio repeats across some scene changes** — a fragment of the previous scene's sound plays again
  as the new one starts.
- **Audio drifts ahead of the cutscene it belongs to**, running faster than the scene so the two come
  apart as it goes on. FMV playback stays in step, which points at the SPU's own clock rather than at
  the XA path or the output device. The first run has to be a clean one — `ZS1_FRAME_PROFILE=1`, no
  stderr logging, no Lua probe — because a guest burning cycles and a host that cannot keep up look
  identical on screen and need opposite fixes.

### Measured, not resolved
- **FMV frames land 8 lines below the window they are displayed through.** Measured on Monsters &
  Co.: the game uploads its frames to `(x,8)` and `(x,264)` while its two display windows sit at
  `y=0` and `y=256` — a constant offset of 8 in both buffers, so the top eight lines on screen are
  whatever was in VRAM and the bottom eight of the picture fall outside the window. The GP1(05h)
  decode is correct against `:695-698` and the game really does write 0 and 256, so the offset is not
  a decode error; which side is wrong is still open. The overscan crop used to hide it, which is why
  it became visible when the crop was restricted to NTSC. This is the strip of macroblock noise above
  the Pixar logo.
- Width is confirmed correct on the way in: the windows the game programs (`x1=624, x2=3184` and
  `x1=635, x2=3195`) are exactly the PAL 320 and PAL 512 fullscreen ranges in the official table
  (`:733-746`), and the formula reproduces 320 and 512 from them.

### Fixed (earlier)
- **The drive answered out of time, and the CPU took the blame.** BIOS ROM instruction fetches had
  gone uncharged for months — ~24 MEMCTRL wait-state cycles per word that the emulator itself
  computes — because charging them hung boot on the PlayStation logo. The cost was right; the drive
  was wrong, in two ways that only a BIOS running at its real speed could expose.
  - Acknowledging a CDROM interrupt re-armed any deferred response at `CDROM_MIN_INT_DELAY`, which
    threw away the deadline the command had set. The guest acknowledges an INT3 within microseconds,
    so every seek, spin-up and read start answered ~30 µs after its command: one `SeekL` computed
    27.719 ms of seek time and delivered its INT2 in the same tick, and `ReadN` charged 606 ms of
    spin-up then handed over the first sector 6.6 ms later. Commands, second responses and sector
    delivery now each carry the cycle they are due at and re-arm on what is *left* of it, so a
    sector arrives every 6.7 ms at 2x as `DOCS/cdromdrive.md:1913` requires.
  - `Init` takes ~740 ms, which is longer than the ~415 ms after which the BIOS gives up and
    re-issues it — so those retries are part of a normal boot. Rescheduling the response on each
    retry pushed the deadline forward for as long as the BIOS kept asking, and it never arrived:
    82 `Init` commands, no reply, screen frozen on the logo. The drive now answers the first request
    at its own deadline and lets the retries fall on it.
- **The BIOS boot sound was cut ~1.9 s short.** Not an SPU fault: the boot phase between the intro
  and the game's SPU handoff ran too fast, so the guest muted the SPU while the sound still had
  seconds to play. With ROM fetches and the drive's deadlines both honoured, the window between the
  BIOS raising main volume and the game zeroing it is 688 fields against a reference run's 708.

- **Games saw a digital pad even with an analog controller plugged in.** The emulated pad powered up
  in digital mode, as hardware does, and the only way out was a DS4 touchpad click — so the sticks
  reached the game solely through the left-stick-to-D-pad fold in `controller.c`, which is why they
  appeared to work while every title reported a digital controller. Three parts to it:
  the pad now boots in analog mode (`ZS1_PAD_MODE=digital` restores the hardware default);
  the Analog button is on **F12** as well as the touchpad, which
  `DOCS/controllersandmemorycards.md:437-440` calls essential, naming Gran Turismo 1 as a title that
  never asks for analog on its own; and the stick-to-D-pad fold is now suppressed while the pad is in
  an analog mode, since otherwise every push arrived twice, once as a direction and once as adc2/adc3.
- **The Analog button press could be swallowed by the debug UI.** It was an edge detected inside
  `controller_update()`, and the controller window polls that function too — whichever caller ran
  second saw no edge. It is latched from the SDL event now and consumed once by the frame loop.
- **A resting stick did not read centre.** The raw SDL axes went straight to adc0-3, so a pad with any
  rest offset held a permanent lean. A radial deadzone with rescaling above it puts the resting
  position back on 80h without losing the start of the travel.
- **The CD-ROM region check never ran.** The drive reported HC05 firmware `94/09/19 vC0` — a PU-7 from
  September 1994 — and `DOCS/cdromdrive.md:1170` states that vC0 cannot answer `Test 19h,22h` at all,
  so the BIOS never asked the machine what region it was. The answer was hardcoded to `"for U/C"`
  (North America) and nothing read it. A PAL disc booted on a PAL BIOS not because it passed the check
  but because the check was skipped, and the emulator was describing a hardware combination that never
  existed: a 1994 Japanese launch drive inside a 1997 European console. The drive now reports
  `95/05/16 vC1`, the oldest version that supports the region string, and the region itself is read out
  of the loaded image's own `System ROM Version <v> <date> <R>` banner — SCPH-7502 gives `E`,
  SCPH1001 gives `A` — so the two cannot disagree. `Cdrom.console_region` carries it and `Test 19h,22h`
  answers from it. Distinct from `disc_region`, which already existed and means the disc's own licence
  string; a mismatch between the two *is* the check.
- **`ReadTOC` delivered its second response about five times too early**, sharing `CDROM_INIT_DELAY`
  (121 ms). It has its own constant now, 180/4 sector times, matching pcsx-redux's `CdlReadToc`.
- **A cold disc read blocked the emulation thread.** `cdrom_async_reader_wait` is replaced by a
  non-blocking poll that returns `PENDING` and re-schedules the drive event. While the emulation thread
  is stopped no VBlank fires and the audio ring drains, so a slow read showed up as both a dropped
  frame and an audible gap — one defect wearing two symptoms.
- **Every BIOS TTY line was logged twice.** The `A0` printf hook emitted the formatted text and the
  BIOS then wrote the same text out one character at a time through `putchar`, which is hooked as
  well. The character path is the one that cannot miss anything, so the printf hook now only decodes
  (for `ZS1_TTY_TRACE`) and emits nothing. Counted against a reference emulator running the same BIOS,
  every line now appears exactly as often as it does there — including `KERNEL SETUP!` twice, which is
  genuine.
- **The VRAM viewer showed black exactly where the picture was.** It was fed from `gpu.vram.data`, the
  CPU-side model, which only ever receives uploads, fills and DMA — never the rasteriser. The decode is
  now a shader pass on the GPU thread reading the unified VRAM texture, covering the same four modes
  plus greyscale and the mask view. It also drops a 2 MB staging upload per frame that was being paid
  for an image that could not show what was asked of it.
- **The display scaled by the active range instead of the TV raster**, so every GP1(07) change resized
  the whole picture. `DOCS/graphicsprocessingunitgpu.md:705` fixes the frame those scanline numbers sit
  in and `:717-719` gives what a set actually shows around it. Screen extent and texture rows are now
  kept separate, because 480i puts 480 rows into the same 240 scanlines.

### Added
- **Every log line carries the machine's clock**, `[f<field> t<seconds>]` — the CRTC field count and
  emulated seconds, from `LogClock` in `log.h` fed by the Interconnect. Host wall seconds cannot
  measure emulated timing: a whole boot phase (EXE load, game init) lands inside the same second,
  which is why "who is faster, and where" had been unanswerable. The field number is also the axis a
  reference emulator's run can be put on, by counting its own v-blank lines, which is how the CDROM
  and ROM-timing entries above were checked.
- **The DS4's light bar shows the emulated pad's LED.** The three pad modes each have a documented
  colour (`DOCS/controllersandmemorycards.md:369-372` — 5A41h digital off, 5A73h analog red, 5A53h
  stick green), and SDL3 can drive a DS4's light bar, so which mode a game actually selected is now
  visible on the pad rather than only in the log. Written only when the colour changes: a light-bar
  write is a HID report, and one per frame competes with the pad's input reports over Bluetooth. The
  capability is queried once when the pad opens, so a pad without a light bar is never written to.
  Needs read access to the pad's `/dev/hidraw*` node, which Linux gives to root alone by default —
  without the udev rule in the README, SDL falls back to the kernel evdev path, where buttons, sticks
  and rumble all work and only the LED is missing.
- **Analog-stick "flight mode" (ID 5A53h, LED green)** alongside the analog pad (5A73h) and digital
  (5A41h). `DOCS/controllersandmemorycards.md:483-489` gives the difference — the stick ID, and L3/R3
  reported as permanently released — and `:496` names the titles that want it: MechWarrior 2, Colony
  Wars, Descent Maximum, and Ace Combat 2, which is the disc this emulator is tested against. F12 and
  the DS4 touchpad cycle digital → analog → stick; `ZS1_PAD_MODE=digital|analog|stick` picks the boot
  mode, and the Controller window shows the current one with a button to cycle it. Both changes of
  mode reset the rumble motors, as a real Analog-button press does (`:1283-1285`).
- **Optional × / ○ swap** (`ZS1_PAD_SWAP_XO=1`, or a checkbox in the Controller window). The button
  bits are not regional — `DOCS/controllersandmemorycards.md:405-421` puts × on bit 14 and ○ on bit 13
  for every pad ever sold — but the software convention is: the PS1 shell and Japanese-developed
  titles confirm with ○. The swap changes which physical button drives which bit. Off by default,
  because reporting the button that was actually pressed is the honest default.
- **WSOLA time-stretch on the audio output** (`src/spu/spu_stretch.c`). The producer generates on the
  emulated clock and the device drains on the host clock; the ring level wanders, and the wander ends
  either in silence or in discarded samples, both heard as a cut. Reading the ring at tempo T while
  still emitting 44100 frames a second lets consumption absorb the difference with no pitch change.
  At tempo exactly 1.0 the search is skipped and the output is bit-exact passthrough, so the dead band
  costs and colours nothing. `emu.stretch()` reports tempo, activity and queue depth;
  `ZS1_SPU_NO_STRETCH=1` bypasses it.
- **`bios_detect_region()`** — the console region, read from the BIOS image's version banner.
- **`ZS1_TTY_TRACE=1`** — names the BIOS hook behind every captured TTY line.
- **Three probes**: `clock_compare.lua` (CPU, video and audio clocks as ratios against nominal),
  `recenter_watch.lua` (one line per change of the displayed area, with the audio state of that same
  vblank, plus the sector rate inside each CD-audio mute), `reverb_cut_watch.lua` (every change of
  SPUCNT, the reverb volumes and the per-voice enables).

### Measured
- The machine's three clocks hold nominal over 128 seconds: CPU 1.0002, video 0.9997, audio 1.0002.
  The CD drive streams 150 sectors a second against a reference emulator's 150.00. This retires the
  "+10/15% drift" the vitals bar was reporting — the readout was wrong, not the machine.
- A 212-second session including a completed level: one underrun event in total, no ring drops, the
  time-stretch never leaving its dead band.
- The audio cut heard at scene changes is the game clearing SPUCNT bit 0 — twice in that session, for
  3.08 s and 2.72 s. The ring never starved. Why the gap is that long is not established.
- Boot reaches the CD drive about 2.3 seconds earlier than a reference emulator on the same BIOS
  image, and the gap opens before any CD-ROM command is issued. The cause is that BIOS ROM execution
  is charged nothing: memory timing is applied to RAM loads only. `DOCS/memorycontrol.md:33` plus the
  access-time formula at `:140` gives 29 cycles per instruction word; the hook exists in
  `cpu_icache.c`, correctly limited to I-cache misses, and is disabled pending the isolation test it
  documents.

### Changed
- **The host layer moved from SDL2 to SDL3** (3.2.24, built from source — Ubuntu 24.04 and its
  derivatives package no `libsdl3-dev`; the Makefile now takes both cflags and libs from
  `pkg-config sdl3`, which is what carries the `/usr/local` rpath). Most of it is renames, but three
  places changed shape:
  - **Audio.** SDL3 has no fill-this-buffer device callback. The device is opened as an
    `SDL_AudioStream` and the callback is told how many bytes are still wanted rather than handed a
    buffer, so it now pushes with `SDL_PutAudioStreamData`. The source and the contract are unchanged
    — the SPU's own ring, drained by `spu_fill_audio`, and `spu_ring_used()` is still what the main
    loop paces against. `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES=512` replaces the old
    `SDL_AudioSpec.samples`, and the savestate guard locks the stream instead of the device.
  - **Gamepads.** `SDL_GameController*` became `SDL_Gamepad*`, and the face buttons are now named by
    position (`SOUTH`/`EAST`/`WEST`/`NORTH`) rather than by an Xbox pad's letters — the PSX mapping is
    positional anyway, so it reads more honestly than `BUTTON_A` did.
  - **Windowing.** `SDL_CreateWindow` lost its position arguments (the window is centred explicitly
    afterwards) and `SDL_WINDOW_FULLSCREEN_DESKTOP` collapsed into a bool.
  ImGui's SDL3 backend is vendored from upstream commit `ed9d1e74`, which is exactly the 1.92.8 the
  rest of `third_party/imgui/` came from — verified by blob hash, not by version string.
- **`getenv`/`strtol` are now included properly in `bus.c` and `cpu_bios.c`.** They compiled before
  only because SDL2's `SDL.h` dragged in `<stdlib.h>` transitively through a chain of project
  headers; SDL3 does not, and both files broke immediately. They should always have included it.
- **The binary is `ZoniStation_One`**, not `myps1_emu`.
- **A second, dead audio device is gone from `cdrom_audio.c`.** `cdrom_audio_sdl_open/close` opened
  their own device with a callback that pulled from the SPU ring exactly as `main.c`'s did. Nothing
  ever called them — `main.c` has owned the one device since the SPU path was built — so porting them
  to SDL3's stream API would have been work spent on code with no caller. The CD audio FIFO itself is
  untouched and still feeds the SPU.
- **Every last DuckStation reference is gone from `src/` and `include/`**: the comments that named
  DuckStation for behaviour or constants (timing models, dispatch tables, hardware constants,
  "DuckStation-style" architecture) are replaced with the specification they restate — the DOT/line
  counts and dot-clock dividers from `DOCS/graphicsprocessingunitgpu.md:1305-1306,1325-1335`, the
  CD speed-change cost from `DOCS/cdromdrive.md:1896-1908`, the noise-LFSR generator from
  `DOCS/soundprocessingunitspu.md:534`, the mul/div latencies from the Guide's table — or with the
  bare hardware fact. DuckStation has been CC BY-NC-ND since 2024-09-01, so no name of it may
  remain in the code tree. `grep -rn "DuckStation" src/ include/` is now empty; the only external
  references left are PCSX-Redux (GPL-2.0+, compatible), credited in the SPU ports and the DMA
  sub-word comment where they are the actual source.
- **The default build is optimised (`-O3 -march=native`)**: the Makefile compiled with `-g -Wall
  -Wextra` and no `-O` flag, i.e. `-O0`. For an interpreter whose hot path is the per-instruction
  decode/execute loop that is ~2x slower than an optimised build — measured during the BIOS 3D boot
  logo, per-frame emulation time dropped from ~15–31 ms (averaging above the 20.1 ms PAL frame budget,
  so the core could not hold real time and any debug instrumentation tipped it into audio drift) to
  ~7–13 ms (comfortably under budget, with headroom for the debug panels and Lua probes). `make DEBUG=1`
  restores an `-O0 -g` build for stepping in gdb. `-march=native` tunes the binary to the build machine —
  rebuild when moving to a different CPU.

### Added
- **Every source file carries an SPDX header** (`GPL-3.0-or-later`, `SPDX-FileCopyrightText`), 76 in
  all, with the upstream authors named in the header of the files that have them so the attribution
  travels with the code rather than only with `THIRD-PARTY.md`. `src/utils/rxi_log.*` are marked MIT,
  which is what they are. `CLAUDE.md` now states the constraint the tree is maintained under.
- **The documented latched-TXEN behaviour** (`DOCS/serialinterfacessio.md:16-20`): writing TX_DATA
  latches TXEN, and the transfer starts if either the current or the latched value is set, so
  clearing TXEN afterwards does not cancel a transfer the write already armed. The documentation
  names Wipeout 2097 as the title that depends on it.

- **`LICENSE` (GPL-3.0) is now actually in the repository** — it existed on disk but had never been
  committed — and **`THIRD-PARTY.md`** records every component with an upstream author, its licence,
  and the parts written from `DOCS/` that have no third-party origin.

- **Savestates (F5 saves, F8 loads, `savestates/slot0.zst`)**: the machine can now be captured and
  restored, so a defect that only appears twenty minutes into a game no longer costs a clean boot to
  reach. `src/core/savestate.c` writes CPU (with the GTE and the I-cache), RAM, scratchpad, interrupt
  controller, event queue, GPU state and VRAM, DMA, timers, CDROM, SPU with its RAM, SIO and the MDEC.
  What is deliberately *not* written is anything the host owns rather than the guest — GL object names,
  the GPU and CD reader threads, the open `FILE` per disc track, the debugger's breakpoints — so those
  members are excluded by span (`offsetof`) rather than stored and restored as dead values. Sections
  carry their size and are checked on load: a struct that changed shape is refused with a message
  instead of being read into a mismatched layout. Also exposed to Lua as `emu.save_state(path)` /
  `emu.load_state(path)`, and `scripts/audio_delivery_probe.lua` uses it to re-enter a state instead of
  replaying the boot on every run.
- **Frame inspector (debug UI phase 4)**: `include/frame_events.h` + `src/core/frame_events.c` are a
  double-buffered per-frame event ring with CPU-cycle timestamps, recorded at VRAM uploads and copies,
  draw batches, DMA channel 2 completions and XA sectors, and published at the frame boundary. The
  Frame view plots each event by its cycle within the frame, with a marker where the nominal budget was
  overrun. The renderer already recorded op *order* (it has to, or a texture page re-uploaded mid-frame
  replays wrong); this records *time*, which is the axis "thirteen of twenty columns never arrived" is
  a question about. Recording is enabled only while that view is the active mode — the standing
  constraint from `docs/ui/README.md` is that the panels cost, not the core.
- **VRAM CPU-vs-GPU comparison (debug UI phase 5)**: `renderer_request_vram_readback` /
  `renderer_get_vram_readback` are a request/response channel across the GPU thread, which owns the GL
  context. The Inspector reports how many VRAM halfwords differ between `gpu.vram.data` and the unified
  texture, split into colour bits, mask bit only, and pixels the GPU has where the CPU model has none —
  a direct measure of gaps 3.1–3.3, since every one of those is a pixel `GP0(0xC0)` readback,
  `GP0(0x80)` copy and texture sampling cannot see. Asynchronous by design: a *synchronous* mid-frame
  readback additionally needs a partial frame flush, and that is a change to the frame protocol.
- **SPU underrun and ring-drop counters, shown in the Audio panel**: the SDL callback pads with silence
  when the ring runs dry, and the SPU discards generated samples when it is full. Both insert
  discontinuities into a continuous stream, both are heard as grit rather than as a dropout, and they
  have opposite causes — so "is the emulator keeping up" is now read off two counters instead of being
  argued about. Reachable from Lua as `emu.audio_stats()` alongside the CD FIFO's push/pop/drop totals.
- **The startup log names the GPU that took the GL context** (`GL_VERSION | GL_RENDERER | GL_VENDOR`).
  On a hybrid machine the same binary lands on the integrated or the discrete card depending on the
  PRIME environment, and "is this a driver bug" is not answerable without knowing which.

- **SPU reverb input/output resampling (39-tap FIR)**: the reverb unit runs at 22050 Hz, and hardware
  feeds it through a 39-tap half-band FIR — the 44100 Hz mixer signal is downsampled into the network
  and its 22050 Hz output upsampled back. This replaced the crude "average two input samples, hold the
  output across two" approximation the code admitted to. Coefficients and behaviour are the hardware's,
  from `DOCS/soundprocessingunitspu.md` ("Reverb Buffer Resampling"); the implementation
  (`rev_reverb_resample` in `src/spu/spu_mixing.c`, using the `reverb_ds_buf`/`reverb_us_buf` rings that
  were already in the struct) is this project's own — no reference emulator code was copied. Improves the
  reverb's quality (band-limiting, smoother tail) at an unchanged output level.
- **`emu.reverb()` Lua binding + `scripts/reverb_boot_trace.lua`**: exposes the SPU reverb's internal
  state (SPUCNT reverb-enable, output volume, per-voice EON mask, work-area base/cursor, and the live
  input/output magnitudes) so a trace can tell "the game switched reverb off" from "the reverb network's
  own tail is decaying wrong" without adding a temporary `printf` to the mixer.
- **Debug UI rebuilt around the data path (`docs/ui/` direction, phases 1–3)**: the floating-panel grid
  (one window per subsystem) is replaced by a **machine bar + mode rail + stage + log dock** layout,
  because every defect that has cost a session lived *between* two subsystems, not inside one. New in
  `src/debug_ui.cpp`:
  - A blued-graphite `ImGuiStyle` (`apply_zonistation_style`) with the `docs/ui/` design tokens. The two
    accents encode the data path — cyan is the video chain (CD→MDEC→DMA→VRAM), rose the audio chain
    (XA→SPU→device); severity colours (ok/warn/crit) are kept separate.
  - A **machine bar** (`draw_machine_bar`): BIOS/disc/PC and the live vitals — real-time %, frame ms,
    audio-queue depth, SPU drift — fed once per frame from `debug_ui_set_vitals`/`debug_ui_set_machine_info`
    (`include/debug_ui.h`) using counters `main.c` already holds (frame budget vs. measured wall time,
    `spu_ring_used`, an SPU sample-delta drift). Cheap by construction: two perf-counter reads, no
    logging. A **Controls** popup folds in pause/step, the log level and the log windows (the old menu
    bar is gone).
  - A **mode rail** (Pipeline / Display / Frame / Code / Memory / Audio / VRAM / Script, F1–F8) with a
    per-mode dock layout rebuilt on switch (`rebuild_layout`). The emulated screen is pinned to the top
    of the stage in every mode; Display gives it the whole stage.
  - A **Pipeline** view (`draw_pipeline_view`): CD → XA → MDEC → DMA → VRAM on one bordered row with
    live per-second rates from real counters — CD sectors (`cdrom.sectors_read_total`), XA samples
    (`audio_fifo.total_pushed`), GPU ch2 uploads (new `Dma.stat_ch2_uploads`, incremented in
    `dma_ch2_signal_done`), MDEC in/out queue depth — sampled over a 0.5 s window. Stages without a
    counter yet show `n/a` rather than a fabricated number.
  - A **Memory** hex view (`draw_memory_view`/`mem_peek`): address gutter + 16 bytes + ASCII over RAM,
    scratchpad and BIOS, with a goto and region jumps. Reads the storage buffers directly, never an I/O
    port, so inspecting memory has no device side effects.
  - Frame timeline, the inspector's VRAM CPU-vs-GPU diff and pinned Lua watches are stubbed with honest
    "pending" notes — they need the phase 4–6 work (a cycle-timestamped event ring, cross-thread GL
    readback, per-frame Lua evaluation) called out in `docs/ui/README.md`.
  - Host window opens maximised with its titlebar buttons (`SDL_WINDOW_RESIZABLE` + `SDL_MaximizeWindow`
    after the GL context exists); **Alt+Enter** toggles borderless fullscreen.
- **`src/core/system.c` / `include/system.h` — unified core "run one frame" driver**: extracted the CPU + event-scheduler timing loop out of `main.c`. `system_init()` seeds the VBlank and timer events; `system_run_frame()` runs the machine until the VBlank event marks the frame boundary (`Interconnect.frame_complete`). `main.c` is now a thin host shell (SDL/GL/audio/threads + framecap) whose per-frame work is a single `system_run_frame()` call — the ~40-line nested chunk loop is gone. Threading: the GPU render thread is started by `main.c`; the SPU's own thread was later removed when sample generation moved onto the emulated clock. Mirrors the DuckStation/PCSX-Redux split of a thin outer loop over a core that owns all timing.

- **VRAM Viewer (PCSX-Redux-style)**: Rebuilt the ImGui VRAM viewer (`src/debug_ui.cpp`) to match Redux's `vram-viewer` widget: selectable decode modes (4/8/16/24 bpp), 24bpp byte-phase shift, selectable CLUT (right-click a pixel), greyscale and mask-bit views, a 16×16 pixel grid and a 64×256 texture-page grid, an outline of the active CRTC display area, cursor-anchored wheel zoom, drag-to-pan, a magnifier lens, and an exact per-pixel readout (raw / 5:5:5 / mask / bytes / 24bpp / tpage) read straight from the CPU-side VRAM buffer. Decode modes are driven through a new `VramViewParams` on the renderer (`renderer_set_vram_view_params`, `include/renderer.h`). The 2 MB/frame VRAM→RGBA8 snapshot is now only taken while the window is open (`debug_ui_vram_viewer_open`).
- **Lua debug bindings**: `emu.spu_stats` (samples produced, samples dropped, output-ring occupancy, key-on count — sample count against emulated time is the direct check that audio is paced by the guest), `emu.draw_area` (drawing area + drawing offset — the GL path scissors every batch to it, so it decides which primitives can reach the unified VRAM texture) and a `gp0_fill` probe point for GP0(0x02); plus `emu.gpustat`, `emu.display_area`, `emu.vram16(x,y)`, `emu.vram_upload_rect`, `emu.gpu_pool`, `emu.gp0_opcode/word/word_count`, `emu.timer(i)`, and `emu.mdec_block/info/scale/qtable/in_peek/in_count/dma` — query-only helpers (zero cost unless a script calls them) plus a few `lua_debug_notify` probe points (`mdec_macroblock`, `gp0_vram_upload/copy/image_start`, `vram_full_upload`) for live MDEC/GPU pipeline tracing.
- **MDEC (Macroblock Decoder)**: Full implementation in `src/core/mdec.c` / `include/mdec.h`.
  - Decode stages implemented from `DOCS/macroblockdecodermdec.md` (rl_decode_block,
    real_idct_core, yuv_to_rgb, y_to_mono). *(Superseded 2026-08-01: this line described the
    earlier implementation and was corrected when the stages were rewritten against the spec.)*
  - 6-block color path (Cr,Cb,Y1-Y4) and mono path; 4-bit, 8-bit, 24-bit, 15-bit output modes.
  - DMA in/out FIFO (2048 HW in, 768 W out); integrated with DMA ch0 (MDECin) and ch1 (MDECout).
  - Status register: data-out-empty, data-in-full, command-busy, DMA-request bits.
- **Memory Card**: SIO memory card protocol state machine (`src/core/sio.c`).
  - Device select (0x81), Read/Write/GetID commands, sector addressing, checksum verification.
  - `sio_load_memcard()` loads `.mcd` image files; auto-loads `memcard1.mcd` on startup.
  - FLAG byte (0x08 = directory unread on powerup) per PSX-SPX spec.

### Changed
- **Host pacing follows the audio queue when a sound device is open**: the frame loop used to spin on the performance counter until the emulated refresh period had elapsed. Audio is the least tolerant consumer in the machine — the device drains 44100 samples every real second whatever the emulator is doing — so the loop now runs ahead only until the SPU's output ring holds `SPU_RING_TARGET_SAMPLES` (~46 ms) and then waits for the device to drain some. Frame-pacing jitter no longer reaches the audio, and the busy-wait is gone (it is kept as the fallback when no device could be opened). This bounds latency but cannot manufacture throughput: if the emulator runs below 100% of real time the ring still empties. (A "85-95% of real time" figure recorded during this work was measured with stderr logging and per-vblank Lua probes active and has been withdrawn — the core keeps real time with the debug interface closed; it is the debug panels under WSL that cost.)
- **The XA audio queue's standing latency is bounded**: sectors arrive in interleave bursts, so some buffering is needed, but the queue could grow to its full 2 s capacity and put hundreds of milliseconds between a picture and its sound. Past four sectors' worth it now drops the oldest frame rather than the newest, so the delay stops growing while the stream stays continuous.
- **Logging defaults to INFO again, and the level is settable per run**: `current_log_level` had drifted to `DEBUG`, contradicting the documented default. The hot paths log per DMA transfer, per GP0 command and per MDEC macroblock — thousands of formatted lines per frame during FMV playback, enough to visibly drag the emulator below real time until the level was lowered by hand. Default is INFO, and `ZS1_LOG_LEVEL=silent|error|warn|info|debug|trace` sets it at startup. Measured after the change: ~87% of real time during FMV playback.
- **VRAM unified into one GL texture (DuckStation `GPU_HW` model)**: the renderer kept three unsynchronized VRAM stores and the display sampled the wrong one — rasterized primitives lived only in `display_texture` (never written back to VRAM: "Gap B"), while CPU/MDEC uploads lived in `gpu.vram.data` and only reached the screen through a fragile 24bpp mirror hack whose column mapping depended on the current `display_x`, decoupled from the game's double buffering. Now ONE RGBA8 texture is the FBO colour attachment (rasterization target), the CPU/MDEC upload target, and the scanout source, so what is written to VRAM is what the display reads. PSX halfwords are stored 5:5:5:1 expanded to 8 bits/channel (`(v<<3)|(v>>2)`), which round-trips losslessly so CLUT index bits survive. A new scanout-extract pass renders the CRTC display window out of that texture, unpacking per depth (15bpp direct fetch; 24bpp recombines two texels and byte-shifts per pixel — DuckStation `GenerateVRAMExtractFragmentShader` / `GPU_SW::CopyOut24Bit`), and `draw_ps1_display` shows it 1:1 instead of cropping the FBO. The rasterizer's Y flip was removed (vertex shader + scissor) so PSX line N is VRAM texel row N — the same row an upload writes and the scanout reads; rendering flipped while uploading unflipped was why FMV frames and rasterized output disagreed about where a scanline lives. Removed with it: the 24bpp mirror hack and its `dst_x`/`depth24` fields, the disabled `renderer_apply_vram_readback` bridge and its 1.5 MB buffer, and the throttled `glGetTexImage` that fed it. (Texture sampling still uses the R16UI mirror; folding that in via a read-shadow ping-pong remains for a follow-up.)
- **Timing unified under one scheduler authority (interpreter-native, PCSX-Redux `Counters` model)**: timers were the only subsystem not driven by the event scheduler — they were stepped by hand in the main loop (`timers_step`) once per coarse chunk, alongside a half-wired, conflicting `EVQ_TIMER0/1/2` event path. Timers are now first-class scheduled events. The counter is **derived on read** as `(cpu_cycle_counter - cycle_start) / rate` (no per-tick increment loop, no fractional accumulator — `src/core/timers.c`, reusing the existing `cycle_start`/`rate` fields), IRQ/reset fires from the scheduled event at the next target/overflow through a single shared IRQ path, and every register read/write/gate-change catches the timer up on demand. Dotclock (Timer0) / hblank (Timer1) rates now derive from the GPU's active video mode via `gpu_dotclock_hz`/`gpu_hblank_hz` (CRTC 53'693'175 NTSC / 53'203'425 PAL, dotclock divider from GPUSTAT h-res, hblank per-scanline), not fixed NTSC constants. Not DuckStation's two-counter `pending_ticks` model — unnecessary for an interpreter whose `cpu_cycle_counter` is always "now". New `src/core/system.c` owns the per-frame run loop; `main.c` shrank to a thin host shell.

### Fixed
- **SIO0 bus sequencing rewritten around the documented signal model.** It was structured as an
  abstract transfer machine; it now follows what the documentation describes on the wire — /CS
  selects a port, the first byte after assertion addresses a device
  (`DOCS/controllersandmemorycards.md:50-67`), each byte is a full-duplex shift, and the addressed
  device pulls /ACK low to request another (`:127-178`). The phase enum and the functions are named
  after those signals. The device protocols themselves already followed the published sequences at
  `:331-346` and `:2354-2400`. Verified: BIOS boots, pad input works, both memory card slots load,
  are detected, written and saved.
- **MDEC decode stages rewritten against the hardware documentation.** `rl_decode_block`,
  `real_idct_core`, `yuv_to_rgb` and `y_to_mono` now follow
  `DOCS/macroblockdecodermdec.md:138-158, :192-245`, and the `zagzig` table is generated by the
  documented rule at `:292-295` from the zigzag table at `:271-282` rather than stored as a literal —
  verified to reproduce the previous table exactly.
- **MDEC scale-table orientation** (regression from the rewrite above, caught in playback): the table
  was transposed on load, which was right for the old inner loop reading `scale_table[y*8+u]` but one
  transpose too many for the documented indexing `scaletable[x+z*8]`, which already accounts for the
  mirroring. Every 8x8 block decoded with its edge row and column too dark, putting a regular
  8-pixel grid over FMV playback. The table is now stored as delivered, and the comment records that
  the orientation and the indexing are one decision rather than two.
- **`calc_memory_timing_word_cycles` comment corrected.** It transcribes the 1ST/SEQ/MIN pseudocode
  published at `DOCS/memorycontrol.md:136-145` line for line, with field positions from `:37-53` and
  `:126-132`. No code change, so the timing model this depends on is untouched.
- **SPU noise waveform was wrong for half of its state space.** The 64-entry tap table repeated its
  first 32 entries instead of carrying their bit-reverse, so the parity was wrong for every LFSR state
  with bit 15 set. The table is gone: the shifted-in bit is computed from the generator published at
  `DOCS/soundprocessingunitspu.md:534`, which removes the whole class of transcription error.
- **SPU 4-point Gaussian interpolation used a differently normalised table.** The console's own
  512-entry table is published at `DOCS/soundprocessingunitspu.md:225-291` and is now used with the
  documented `SAR 15` formula from `:215-224`. The transcription self-checks: the documentation notes
  at `:295-299` that the real table's groups of four sum to 7F7Fh..7F81h rather than the theoretical
  8000h, and it reproduces exactly that range.
- **SPU ADPCM fed the unclamped prediction back into the filter.** The decoded sample is saturated to
  16 bits before it becomes filter state — the same rule the CD-XA decoder already followed
  (`DOCS/cdromformat.md:836-837`). Without it one overflowing nibble poisons the remaining 27 samples
  of the block and leaves out-of-range values in `SB[]` that are only clamped after the envelope and
  volume have scaled them.
- **README's licence section contradicted `LICENSE`**, saying "Educational purposes only" where the
  file is GPL-3.0. It now states GPL-3.0-or-later and what that grants. `timers.c`'s attribution named
  no licence ("used under open source license") and now names GPL-2.0-or-later, and the vendored Dear
  ImGui and Lua copies carry the MIT notices their terms require.
- **A textured polygon's Texpage attribute did not persist as draw-mode state**: PSX-SPX
  (`DOCS/graphicsprocessingunitgpu.md:356-360`) specifies that bits 0-8 of the attribute are the same as
  `GP0(E1)` bits 0-8 — a textured polygon *is* a draw-mode write. The primitive itself sampled correctly,
  because it carries its own texpage, but rectangles and lines take theirs from `GP0(E1)` state, so any
  sprite drawn after a textured polygon sampled whichever page the last explicit `GP0(E1)` had set. The
  polygon's own semi-transparency mode came from the same stale state rather than from its attribute.
  `ZS1_GPU_NO_TEXPAGE_ATTR=1` restores the previous behaviour, so the change can be bisected against a
  regression in a single run. Bit 11 is deliberately not applied: on a retail v0 GPU it only means
  "texture disable" once `GP1(09h)` has enabled it, which this core treats as a no-op.
- **Space was bound to both Pause and the pad's START button**: the debug UI took `Space` for
  pause/resume while `controller.c` reads the same key as START from the raw SDL keyboard state, so
  every press of START also halted the machine. Pause moved to **F10** (and the dedicated `Pause` key).
- **XA coding-info bits were read from the wrong positions**: the sample rate is bit 2 and the bit depth is bit 4 (`DOCS/cdromformat.md:664-671`, `DOCS/cdromdrive.md:265-278`), but the decoder tested `0x04` for 8-bit and `0x08` for 18900 Hz. Every stream that was not the common 37800 Hz 4-bit case therefore decoded as noise, and 18900 Hz was never detected at all, leaving `resample_xa_18900` unreachable. That resampler was also wrong when it did run: 18900 → 44100 is **7 output samples per 3 inputs**, not the 7-per-6 of the 37800 path, so 18900 Hz material would have played an octave low at half speed. It now carries its ratio in a credit counter that survives sector boundaries. Also on the same path: the ADPCM filter now feeds back the *clamped* sample as the documentation specifies (`DOCS/cdromformat.md:836-837`) instead of the raw prediction, which could let the IIR state run away on loud material.
- **The SPU reverb wrote into the voices' sample data, even with reverb switched off**: `mBASE` is a full 16-bit address divided by 8 covering all 512 KB of SPU RAM, but it was masked to 14 bits, putting the work area roughly 384 KB too low — for the documented "Room" preset, byte 0x1D940 instead of 0x7D940, i.e. on top of the ADPCM sample region that starts at 0x1000. Compounding it, SPUCNT bit 7 was applied to the output mix when it actually disables *writes* to the reverb buffer while reads and output continue (`DOCS/soundprocessingunitspu.md:826-835`, confirmed by the bus-timing table at `:1113-1123`). So a game that switched the unit off still had its samples overwritten. The mask is gone and the write gate moved into `rev_wr`.
- **Volume sweeps ran in the wrong direction and started from a meaningless level**: the direction is bit 13 (`DOCS/soundprocessingunitspu.md:366-387`), not bit 7, which is documented as unused. And a sweep-mode write used to store the configuration bits into the current level, where the documentation says a sweep starts from wherever the volume already is — so both the per-voice and the main volume registers now leave the level alone unless the write is in fixed mode.
- **A queued CDROM second response was discarded instead of rescheduled**: `cdrom_schedule_second_response_event` refused to arm if one was already pending, so a second command's INT2 fired at the first command's deadline or never. Measured on a boot: twelve `Init` commands produced three second responses, and a burst of ten produced one.
- **CDROM timing: the Pause delays were swapped and a speed change cost nothing**: hardware pauses in 2 168 860 cycles at 1x and 1 097 107 at 2x (`DOCS/cdromdrive.md:1888-1889`) — a faster drive pauses sooner — where this had the two the other way round. And changing the read speed with Setmode spins the drive up or down, about 0.6 s for 1x→2x and 0.7 s the other way; that cost is now carried and charged to the next seek, which is where DuckStation accounts for it too.
- **XA-ADPCM sectors claimed eight times their sample count (FMV audio was noise)**: `cdrom_audio_decode_xa` computed `samples_per_chunk = num_blocks * words_per_block * (bits8 ? 4 : 8)`. A 128-byte sound group holds `num_blocks` blocks of 28 samples — 224 for 4-bit XA, 112 for 8-bit — and there is no further multiplier. The extra factor of 8 made a sector claim 18816 output frames where it has 2352, so seven eighths of everything pushed into the audio FIFO was whatever happened to be left in the decode buffer. It was partly masked by a `if (fifo->count > 2048) return;` guard that discarded whole sectors once the queue filled — audible as "FMV audio is noise". Measured after the fix: 44688 samples/s pushed against 44144 consumed (was ~94000 pushed with half of it dropped), queue depth 300-2100 frames instead of 5000-18500, zero drops.
- **Reverb ran at the wrong rate and addressed its buffer wrongly (metallic crackle over everything)**: the unit runs at 22050 Hz on hardware — one step per two output samples, which is also what advances its delay line — and every src/dst/disp register is an SPU address *divided by 8*, relative to the current buffer address. This implementation stepped the whole IIR/comb network at 44100 Hz and treated the address registers as halfword offsets, so each read and write landed at a quarter of its intended distance; the writes fell outside the reverb work area, on top of the voices' own sample data, which is what made the corruption audible on everything rather than just on the reverb tail. `reverb_process` is now written from the documented formula (same-side and different-side reflection, 4-tap comb, two all-pass stages) at half rate, with addresses resolved in halfwords and wrapped inside mBASE..end-of-RAM, and a write to mBASE resets the current buffer address as hardware does. Only voices whose bit is set in the per-voice reverb mask feed the unit (that mask was stored and never read). Measured over a 30 s capture of the emulator's own output: sample-to-sample jumps above 8000 LSB went from 10302 (0.79%) to 44 (0.002%), against 11 with the reverb bypassed entirely.
- **SPU generated audio on wall-clock time instead of emulated time (sound completely broken)**: two sample producers existed and the correct one was dead. `spu_step()` (`src/spu/spu_mixing.c`) produces one stereo sample per 768 CPU cycles — the 44100 Hz the hardware runs its DSP at — and had **zero call sites**; the live producer was a dedicated thread that watched the free space in the output ring, generated up to 512 samples and slept 1 ms. Everything that advances with a sample (ADSR envelopes, ADPCM positions, the reverb delay line, the CD-audio FIFO) therefore advanced at whatever rate the host audio device drained the ring, while the guest's key-on/key-off and register writes arrived on the emulation thread at emulated rate — two independent clocks, drifting permanently, so note lengths and envelope shapes were wrong whenever the emulator was not at exactly 100% of real time (measured ≈87% during FMV playback). The voice registers were also read from the audio thread without synchronisation, so a key-on/key-off latch could be missed or applied twice. Now the emulated clock drives generation: a scheduled `EVQ_SPU` event (64-sample batches) plus `spu_catch_up()` on every SPU register read and write, so a write flushes everything owed at the old register values before mutating state. The wall-clock thread is gone; production and register access are on the same thread, which also makes the CD-audio FIFO single-threaded end to end. A sample is still generated when the output ring is full — the DSP state has to advance regardless — and only the audible result is dropped, counted in a new `dropped_samples`. Measured after the change: 106368 samples per 81 698 760 emulated cycles against 106378 expected (−0.01%), ring occupancy steady at 192-512 of 4096, and no drops after the start-up transient.
- **Rasterized pixels always set the mask bit (green FMV letterbox)**: the unified VRAM texture stores the PSX mask bit (bit 15) in its alpha channel, but the fragment shader emitted `vec4(colour, 1.0)` unconditionally, so every rasterized pixel came back out of VRAM as `0x8000 | colour`. In 15bpp display modes bit 15 is ignored on scanout and nothing showed; in 24bpp it is *picture data*, so an area painted black by the GPU decoded as halfwords `0x8000` — bytes `00 80 00 80 …` — i.e. mid-green. That is exactly what the letterbox bands around Ace Combat 2's 320×160 FMV were: rows the movie never uploads, last painted black by the game's own clear. The shader now emits the real mask bit: 0 by default, 1 when GP0(E6).0 forces it (new `u_set_mask` uniform, plumbed as batch state through `renderer_set_mask_mode` so it survives the CPU→GPU-thread batch replay), or when a textured pixel's source texel has bit 15 set (PSX-SPX "Mask bit": the STP bit is copied to the framebuffer). Semi-transparency blending now uses `glBlendFuncSeparate`/`glBlendEquationSeparate` with `GL_ONE, GL_ZERO` on alpha, since hardware writes the source pixel's mask bit whatever the colour blend does. Measured first: `scripts/fmv_fill_watch.lua` showed the game issues **no** fill at all during playback (8 in a whole run: two at boot, six after the movie), and `scripts/letterbox_history.lua` showed those rows do not change in CPU VRAM for the whole movie — so the bands could not be a missing write, only a misread of what was already there.
- **Upload rects were routed into the debug VRAM-Viewer texture (FMV display striping)**: `renderer_record_vram_update` (`src/gpu/renderer.c`) wrote every field of the `GpuVramUpdate` entry except `is_viewer`. That array lives in the per-frame command list and is reused frame after frame, so an index that had once held the VRAM-Viewer's 2 MB snapshot (`renderer_update_vram_viewer`, the only setter of the flag) kept `is_viewer = true` forever — and from then on every upload rect recorded at that index was executed as a viewer upload, never reaching the unified VRAM texture. The poisoned set grew during boot as frames with different upload counts put the viewer entry at different indices, so during FMV playback 13 of the 20 macroblock columns of each frame never updated on screen: the display kept showing the previous frame's pixels there, as a fixed pattern of grey/stale vertical bands, while the CPU-side VRAM model (and the VRAM Viewer) held a perfect frame. Localised by reading the unified texture back on the GPU thread and comparing each rect against the CPU halfwords staged for it — 13/20 rects mismatched 3840/3840 pixels while the staging bytes were provably identical between execution and check, and the rects that did take the VRAM path matched byte-for-byte immediately after upload; 0/20 mismatches after the fix, on both double-buffer halves. Not a CRTC/PAL issue: the display window measured correct throughout (`disp=(512,0)`↔`(512,240)`, 320×240, 24bpp, h-range 2560 ticks / dot divider 8 = 320 px, v-range 240 lines).
- **DMA completion interrupts were lost after the first DICR acknowledge (FMV playback blocker)**: `interconnect_set_irq_line` only latches I_STAT on a low→high edge, but the DICR acknowledge path in `dma.c` cleared `irq_status` while leaving `irq_line_state` high, and the four completion sites poked the line directly from behind a `!(irq_status & IRQ_DMA)` guard. Once a game acknowledged through DICR alone, the DMA line stayed logically high forever and every later completion produced no edge — the interrupt simply vanished. Ace Combat 2's movie player is a direct casualty: its CD ISR marks a sector descriptor "transfer running" when it kicks the ch3 payload DMA and relies on the DMA interrupt handler to mark it "ready", so the descriptors of one frame froze and the movie died after 49 of its 1905 frames while the drive kept streaming. New `dma_update_irq()` (`src/core/dma.c`) is now the only place allowed to touch the line: it recomputes DICR's master flag and acts on the transition — asserting on false→true, deasserting on true→false. Re-asserting an already-high line instead restarts the interrupt on every call and traps the CPU in the handler (reproduced as a freeze on the SONY splash). Every DICR write goes through it too, so a game that enables interrupts *after* writing CHCR still gets its completion. Mirrors DuckStation's `DMA::UpdateIRQ` (`dma.cpp:500-507`) and its `UpdateIRQ()` call on the DICR write path (`dma.cpp:457`, see the note at `dma.cpp:401`). Measured over the same 130 s run: MDEC frame commands 46 → 386, decoded macroblocks 9121 → 365 800, MDEC busy continuously instead of stalling after ~2 s of movie.
- **GPU block DMA sampled guest RAM too late (FMV column striping)**: `REQUEST`/`MANUAL` transfers on ch2 were sliced across event ticks like the linked-list path, so the source words were read long after the guest kicked the transfer. The movie player owns only two staging buffers and refills one as soon as its transfer is kicked, so the deferred read picked up the *next* column's pixels — VRAM ended up holding just two distinct payloads repeated across all twenty 16-pixel columns of a frame (measured: 20 uploads, 2 unique hashes). Block transfers now consume the buffer at kick time, as both references do (DuckStation delays only the completion, never the data read; PCSX-Redux copies immediately and schedules just the IRQ). The linked list stays sliced — its node chain is built before the kick and is not rewritten under us.
- **DMA kicks arriving during an in-flight slice were dropped outright**: a kick for a channel whose sliced transfer had not finished was silently discarded, losing a whole transfer — during FMV playback the player kicks the next column upload while a GPU linked list is still draining, and that column stayed blank. Real hardware cannot lose the transfer, so the GPU channel now drains its outstanding slices (bounded) and then runs the new transfer. MDEC's two channels still drop such kicks: they gate on each other's FIFO readiness, so a synchronous drain there deadlocks (it hangs at the first FMV frame) — queueing them is left for the MDEC path itself.
- **CPU/MDEC uploads never reached the displayed texture**: `glTexSubImage2D` wrote the unified VRAM texture while that texture was still attached as the bound FBO's colour attachment — undefined in GL, and in practice the driver dropped the write. Detach (bind FBO 0) around the upload. Also, the every-frame full-VRAM sync must only refresh the R16UI sampling mirror and must NOT blit into the unified texture: `gpu.vram.data` holds no rasterized pixels, so blitting it erased each frame's drawing (black/flickering screen).
- **24bpp scanout dropped bit 15**: the halfword recovered from the 5:5:5:1-expanded texel discarded bit 15, which in 24bpp is a data bit of the packed byte stream (not a mask flag), corrupting every pixel whose high byte was ≥ 0x80. Restored from alpha. Together with the upload fix, the FMV region on screen went from `max=1` / 0 non-black pixels to `max=128` / 35840.
- **Frozen timer counter reads**: a game busy-polling a timer counter (Ace Combat 2 read Timer1 ~300k times/run) saw a value frozen between main-loop chunks (~99% of reads returned an unchanged value at a later cycle), because timers only advanced once per chunk. Reads now derive the live value from the global cycle counter, so a poll always sees continuous advance (DuckStation `InvokeEarly` / Redux `psxRcntRcount` behaviour).
- **Timer IRQ double-fire**: the two parallel timer mechanisms requested the same IRQ line with different guards (`already_pending`/I_STAT vs `interrupt_requested`), and both reset the counter — allowing duplicate edges and spurious resets. Collapsed to a single event-driven IRQ/reset path.
- **Event scheduler wrap-unsafe comparisons**: `eventq_schedule`'s "is this event sooner" test and the `evq_next_cycle` recompute compared absolute cycle values (`>`/`<`), which are wrong across the uint32 `cpu_cycle_counter` wrap (~every 127 s). Both now use signed-delta compares.
- **MDEC IDCT scale-table not transposed (FMV macroblock-grid blocker)**: `mdec_handle_set_scale` (`src/core/mdec.c`) stored the 64-entry scale/IDCT matrix sequentially, but the IDCT reads it as `scale_table[y*8 + u]` (frequency → output position). It must be transposed on the way in — DuckStation does this in `SetScaleMatrix()` (`scale_table[y*8+x] = values[x*8+y]`), which the rest of `mdec.c` was ported from while this one step was missed. With the wrong-orientation basis every block decoded with a distorted IDCT: a DC-only macroblock (which must decode to a flat patch of colour) instead came out as a smooth blob fading to its edges, so FMV frames rendered as a regular grid of blobs, one per macroblock. Now transposed; DC-only blocks decode flat. Verified in isolation via a Lua `block_rgb` probe and against a reference IDCT.
- **DMA sliced-transfer re-kick (FMV VRAM corruption)**: sliced, event-scheduled transfers (GPU ch2, MDEC ch0/1) stay `enable/busy` for their whole duration, so any later kick that re-inspects the channel — a `DPCR` write unblocking channels, or software re-poking `CHCR` — restarted the transfer from `base_addr` and re-sent the entire payload. `interconnect_perform_dma` (`src/core/bus.c`) now no-ops when a slice for that channel is already in flight (`dma_slice_in_flight`), matching the fact that DuckStation's `TransferChannel()` is resumable (advances `base_address` and continues rather than rewinding). The duplicate GPU ch2 payload had been arriving with no `GP0(0xA0)` in front of it, so MDEC pixel words (`0x80…` chroma) were decoded as GP0 commands — producing ~14 000 phantom VRAM copies/frame, a saturated staging pool, and a wrecked VRAM. Phantom copies dropped from ~14 000 to 1; MDEC TTY timeouts (`MDEC_in_sync timeout` / `time out in decoding`) from 27 to 0.
- **PAL/NTSC frame timing (PAL titles ran ~20% fast)**: the VBlank period and host frame pacing were hardcoded to NTSC 60 Hz (`564480 = 33868800/60`) in both `event_scheduler.c` and `main.c`. New `gpu_cycles_per_frame()` (`src/gpu/gpu.c`) derives the period from the GPU's current video mode using the real clock relationship (3413×263 lines NTSC / 3406×314 PAL GPU ticks, converted to system ticks by `×451584/715909` NTSC and `/709379` PAL — DuckStation `gpu.cpp:964-989`): 566203 cy / 59.82 Hz NTSC, 680823 cy / 49.75 Hz PAL. Re-derived each frame so a mid-run GP1(08) video-mode switch takes effect. This drove the whole PAL timebase (VBlank rate, and with it FMV/audio pacing) 20% too fast.
- **MDEC DMA pacing (input-FIFO starvation / decode timeouts)**: MDEC ch0/ch1 slices used the GPU path's flat 64-words-per-1000-cycles quantum (~15.6 cy/word), slow enough that libmdec's `DecDCTinSync`/`DecDCToutSync` spin loops gave up mid-FMV. Now slices are capped at 100 words (DuckStation `SLICE_SIZE_WHEN_DECODING_MDEC`) and the inter-slice stall is the real DRAM hyper-page cost of the words actually moved (`words + (words+15)/16`, DuckStation `Bus::GetDMARAMTickCount`), with a fixed back-off only when both directions are FIFO-blocked.
- **24bpp display decode**: `renderer_execute_one_vram_update` (`src/gpu/renderer.c`) always unpacked the display area as 5:5:5. When GPUSTAT.21 (24bpp) is set it now reads packed 3-bytes-per-pixel triplets (DuckStation `GPU_SW::CopyOut24Bit`), with the display-texture column derived from the VRAM halfword column at the 3-byte/2-halfword ratio. VRAM uploads (`renderer_upload_vram_rect`) now also mirror into `display_texture` — previously only GL-rasterized pixels reached it, so game/MDEC-painted display content (FMV frames, 2D backdrops) could never appear. Fixes the FMV display path (decoder output confirmed reaching VRAM); the movie itself still decodes near-black pending an upstream STR-demux/timer investigation.
- **VRAM viewer upside-down**: the viewer sampled its texture with a flipped `v` (`(0,1)-(1,0)`), inherited from the old viewer. Unlike `display_texture` (an FBO the GL rasterizer writes in y-up clip space), the viewer texture is a straight CPU upload of VRAM rows 0..511 in order, so `v=0` is already VRAM y=0 — all of VRAM had been shown vertically mirrored.
- **GTE RGB-FIFO CODE byte (PS boot-logo blocker)**: `push_rgb_from_mac` wrote `RGB2 = r | g<<8 | b<<16`, dropping the CODE byte (bits 24-31). Real hardware copies `RGBC.code` (data reg 6, byte 3) into the pushed colour's high byte — now `RGB2 = r | g<<8 | b<<16 | code<<24` (matches DuckStation `PushRGBFromMAC`). The CODE byte is the GP0 command opcode: the BIOS shell's software ordering-table renderer stores the GTE-lit colour word straight into a primitive packet and reads bits 24-31 back as the GP0 command. With CODE=0 every Gouraud-lit logo primitive decoded as opcode `0x00` and was discarded by the packet classifier, so the spinning PlayStation boot logo never drew (vertices present, opcode/colour word zero). **The 3D PS logo now renders correctly.** Multi-session Phase 2.7 blocker resolved.
- **DMA sub-word register access**: byte/halfword writes to the DMA register block (`0x1F801080-0x1F8010FF`) were logged `Non-word write` and dropped, and sub-word reads switched on the unaligned offset (fell into the register-handler defaults). The BIOS shell enables the DMA-completion IRQs with a byte write to `DICR+2` (`0x1F8010F6`), so that write was silently lost. Fixed by word-aligning the offset before dispatch and merging the written lane into the current register value (PCSX-Redux byte-array semantics; DuckStation `FIXUP_WORD_OFFSET`), excluding DICR's write-1-to-clear IRQ-flag lane (bits 24-31) from the merge.
- **GPU IRQ line (GP0(0x1F))**: `IRQ_GPU` (I_STAT bit 1) had no raise site — GP0(0x1F) Interrupt Request only set the GPUSTAT.24 status flag and never asserted the interrupt line, and GP1(02) Acknowledge never lowered it. Now `interconnect_set_irq_line(IRQ_GPU, true/false)` in both handlers, matching DuckStation `HandleInterruptRequestCommand` / GP1(02) `SetLineState(IRQ::GPU, ...)`.
- **DMA-1 (GPU LL terminator)**: `header & 0x00FFFFFC` masked bit0/1, making `next_addr == 0xFFFFFF` impossible. Fixed: extract `raw_next = header & 0x00FFFFFF` first, then check `raw_next & 0x800000` (bit23 = end-of-list per PSX-SPX, all revisions).
- **DMA-2 (DPCR gate)**: DMA channels now blocked when `DPCR` bit `(ch*4+3) = 0`. Previously transfers always started unconditionally on CHCR enable write.
- **DMA-3 (BCR zero)**: `block_size=0` and `block_count=0` now correctly mean `0x10000` per PSX-SPX. Previously caused silent zero-word transfers.
- **DMA-4 (DPCR scan)**: DPCR write now scans all 7 channels and starts any that are active+enabled. Fixes cases where DPCR is written after CHCR (correct software order on hardware).
- **DMA-5 (CHCR bits)**: Chopping enable (bit8), DMA window size (bits 16-18), CPU window size (bits 20-22) now read back from CHCR correctly.
- **DMA-6 (CPU stall)**: DMA transfer duration subtracted from CPU downcount. Device rates: GPU/OTC/MDEC=1 clk/word, SPU=4, CDROM=40. RAM hyper-page: 17 clk/16 words.
- **DMA GPU wait (bit26→bit28)**: DMA ch2 was waiting for GPUSTAT bit26 (cmd-ready), which is 0 during IMAGE_LOAD mode → 10K-spin timeout per word. Fixed to wait for bit28 (DMA-ready), which stays set in all GP0 modes as long as FIFO < 16.
- **GPUSTAT bits 25/26/27/28**: Corrected per PSX-SPX. Bit26 only set in COMMAND mode; bit27 only during active VRAM→CPU transfer; bit28 always set when FIFO<16; bit25 mirrors bit28 or bit27 based on DMA direction setting.
- **GP0(0x02) Fill Rectangle**: Rewritten per PSX-SPX: absolute VRAM coords (no draw offset), no drawing-area clip, no mask bit. Position masked `x & 0x3F0`, `y & 0x1FF`; width rounded up to 16-pixel boundary.
- **GPU Dithering**: 4×4 PSX dither matrix in fragment shader; enabled for Gouraud-shaded/textured-modulated primitives and lines; disabled for mono flat-shaded polygons and all rectangle types. `renderer_set_dither_mode()` flushes before mode change.
- **SPU ADSR return value**: All ADSR phases were returning `EnvelopeVol >> 5` (0–1023). Fixed to return `EnvelopeVol` (0–32767) matching DuckStation 15-bit precision.
- **SPU decay mode**: Decay phase was wrongly using `release_mode_exp` flag for exponential check. Fixed to always use exponential (PSX hardware behaviour).
- **SPU voice mixing**: Volume scaling was `/ 0x4000L` (≈×2 overscale). Fixed to `>> 15` matching the 15-bit range of `vol_left/right`.
- **SPU main volume scaling**: `main_vol_left/right` was used raw (0x3FFF → half volume). Fixed: applied same `<< 1` fixed-mode scaling as voice volumes via `main_vol_left/right_cur`. 0x3FFF now gives ≈full-volume output.
- **SPU volume register fixed mode**: `vol_left = value & 0x3FFF` (wrong). Fixed: sweep mode (bit15=1) stores raw 14-bit; fixed mode (bit15=0) stores `(value & 0x7FFF) << 1` for 15-bit range.
- **SPU sweep direction bit**: `(reg >> 13) & 1` was extracting wrong bit. Per PSX-SPX bit7 is Direction (0=inc, 1=dec). Fixed to `(reg >> 7) & 1`.
- **SPU CD audio feed**: `spu->cd_audio_left/right` were never updated (always 0). Now fed from CDROM audio FIFO on every SPU tick (per-sample), with CD volume scaling applied. XA/CDDA audio now mixed into SPU output when `SPUCNT` CD-audio-enable bit is set.
- **SPU vol_left/right_count**: Sweep counters not reset on key-on. Fixed: `vol_left_count = vol_right_count = 0` in key-on handler.
- **SPU sweep tick**: `spu_voice_sweep_tick()` was not being called per-sample. Now called for all 24 voices each SPU tick, after `spu_voice_get_sample`.
- **SPU reverb output**: Was mixing `rev_l >> 2` (25% reverb). Fixed to `rev_l` (100%).

### Fixed (pre-existing, carried forward)
- **Performance Overhead**: Removed all `TRACE` logs globally and stripped `DEBUG` logs from hot paths (`bus.c`, `timers.c`, `cpu_instructions.c`, `dma.c`).
- **CDROM Command Dropping**: Removed strict `interrupt_flag != 0` check in `cdrom_write8` when processing new commands (e.g. `SeekL`), resolving game load hangs (Ace Combat 2).
- **BIOS Syscall Spam**: Expanded B0 table in `cpu_bios.c` to `0x5D`; ignored out-of-bounds B0 calls, stopping `[BIOS] B0(unknown)` spam during game load.
- **Log Formatting**: Standardized log strings codebase-wide; stripped trailing newlines for ImGui compatibility.

---

## [sound] — 2026-05

### Added
- **SPU (Sound Processing Unit)**: Full implementation across 6 source files (`spu.c`, `spu_voice.c`, `spu_adsr.c`, `spu_mixing.c`, `spu_dma.c`, `spu_irq.c`).
  - 24 voices with XA-ADPCM decoder (5 prediction filters, Gaussian 4-tap interpolation)
  - ADSR envelope (Attack/Decay/Sustain/Release phases)
  - Noise generator, reverb (IIR/comb/allpass), capture buffer (4 channels)
  - 512 KB SPU RAM, DMA transfer (manual/DMA read/write)
  - IRQ9 address boundary detection
  - Circular sample buffer (4096 stereo frames) for SDL audio callback synchronization
  - `spu_step()` integrated in main emulation loop with CPU cycle timing
- **ImGui SPU Debug window**: Voice status table (24 voices, ADSR phase, pitch, volume), audio peak level meters (L/R), buffer fill indicator, global control/status registers, transfer/DMA state, reverb registers.
- **SPU log category**: `LOG_SPU_*` macros for ERROR/WARN/INFO/DEBUG/TRACE; visible in ImGui SPU log viewer.

### Status
Sound pipeline active. Minor timing/sync issues remain (see commit `0a8d69e`).

---

## [debug-ide] — 2026-03 to 2026-05

### Added
- **CPU Disassembler**: PCSX-Redux-style disassembly window with 128-row virtual list (`ImGuiListClipper`), row highlights for PC (yellow) and breakpoints (dark red), clickable breakpoint dots, Go-To-Address footer.
- **Run / Pause / Step controls**: F5 (run/pause toggle) and F11 (single step) wired via `debug_ui_step_requested()` edge-triggered flag into main emulation loop.
- **Breakpoint manager**: ImGui table (address / enable checkbox / delete); click address to jump disassembly view; Add BP input field.
- **CPU Registers window**: PC / SR / Cause / EPC / HI / LO header + 32 GPRs in two-column table with MIPS register names; non-zero registers highlighted yellow.
- **IDE-style Debug UI**: Complete `src/debug_ui.cpp` rewrite (~400 lines). Menu bar: `Emulator` | `Debug` | `Logs` | `Options`. `[PAUSED]` indicator. Initial docking layout: PS1 Display (main), Disassembly (right-top 62%), CPU Registers + Breakpoints (right-bottom 38%), component logs (bottom 22%).
- **FBO Display Rendering**: PS1 display rendered to off-screen OpenGL FBO; shown inside dockable/floatable ImGui window.
- **Modular component log windows**: 16 ImGui log windows (one per hardware category), individually dockable. Open All / Close All in `Logs` menu.
- **Debugger struct** (`include/debugger.h` / `src/debugger.c`): breakpoints + per-BP `bp_enabled[]` array, read/write watchpoints, `step_skip_bp` flag to avoid re-triggering when stepping off a breakpoint.
- **GPU partial VRAM upload** (`renderer_upload_vram_rect`): `gp0_image_load` and `gp0_copy_rectangle` upload only the dirty rectangle via `glTexSubImage2D` + `GL_UNPACK_ROW_LENGTH` instead of full 512 KB VRAM texture.

### Changed
- **Texture window uniform**: Fragment shader `uvec2 tex_window_and` + `uvec2 tex_window_or` consolidated into single `ivec4 u_texWindow` (and_x, and_y, or_x, or_y).
- Standard command-line logging (stdout/stderr) fully disabled; all output routes to ImGui windows.

### Fixed
- **Textured rectangle VRAM upload ordering**: `renderer_set_semi_trans_mode` (which triggers batch flush) was called before `renderer_upload_vram` → stale VRAM texture → wrong CLUT → rainbow corruption on font sprites. Fixed by uploading VRAM before flush-triggering call.
- **ImGui crash** (`TableSetBgColor` outside table scope): Replaced with `GetWindowDrawList()->AddRectFilled` using cursor screen position.

---

## [ui-refactor] — 2026-03

### Added
- **ImGui docking + multi-viewports**: `ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable`; windows can be dragged outside the main SDL2 window.
- **Kernel/TTY Logging**: BIOS `printf`/`putchar` syscalls routed to ImGui "BIOS" log window.
- **ImGui Log Level Selector**: Global verbosity (TRACE → SILENT) toggleable at runtime from `Options` menu.
- **rxi/log integration** (`src/log.c`): 16 categories × 6 levels, per-category filter.

### Changed
- Main SDL2 window is now a pure ImGui DockSpace host; all UI rendered via ImGui. No terminal output.

---

## [cpu-cycle-model] — 2026-03

### Added
- **DuckStation-style CPU cycle model**: `Cpu.downcount` decrements per instruction; `eventq_dispatch_due` fires when ≤ 0.
- **MulDiv stall emulation**: MFHI/MFLO stall until `muldiv_completion_tick` (MULT = 7 cy, DIV = 37 cy).
- **Bus modular split**: `src/interconnect.c` split into `src/bus.c` (memory routing), `src/bus_irq.c` (IRQ controller), slim `src/interconnect.c` (init + event glue).

### Fixed
- **Timer clock source routing**: All three timer clock_source bit pairs now correct per PSX-SPX spec. Timer2 `clock_source=2` fell through to skip → BIOS Timer2 polling loop froze post-menu.
- **GPU draw offset**: `gp0_drawing_offset` (GP0 0xE5) now calls `renderer_set_draw_offset` so double-buffer offsets (0,1)/(0,241) are applied to vertex shader uniform.
- **VRAM blit color channel order**: R and B were swapped in `renderer_blit_vram` (PSX RGB555: R=bits 0–4, B=bits 10–14).
- **DMA completion IRQ**: `interconnect_perform_dma` raises IRQ3 on 0→1 I_STAT[3] transition only; removed PCSX ReARMed re-raise hack that caused infinite loop on wrong-channel DICR ACK.
- **SIO range ordering** in `store16/store8`: SIO (0x1F801040–0x1F80104F) now checked before MEM_CONTROL_END (0x1F80107F); SIO read handler before generic hwregs catch-all so JOY_STAT TX_RDY bits are visible to BIOS.
- **Drawing area initial bounds**: `(0,0,0,0)` → `(0,0,1023,511)`; `renderer_set_drawing_area` called from `gpu_reset_state`.

---

## [stable-bios-menu] — 2026-03-17

### Working at this milestone
- BIOS boot sequence (SCPH-1001 US): Sony logo animation → interactive menu → cursor → navigation
- GPU: GP0/GP1 command dispatch (polygons, rects, lines, VRAM transfers), double-buffer, blit, draw offset, scissor
- DMA: linked-list and block/request for GPU (ch2) and OTC (ch6)
- Timers 0/1/2: counter, mode, target registers; VBlank event scheduler
- CDROM: command handling, disc read, IRQ delivery
- SIO/Controller: digital pad protocol, keyboard-to-gamepad mapping (WASD/SPACE/E/C/Z/X)
- GTE: geometry transformation engine, load delay slots
- I-Cache: 256-line 4-word cache with tag/valid bits
- Event scheduler: DuckStation-style downcount dispatch for VBlank, timers, CDROM

### Added (leading up to this milestone)
- **Controller input system**: Keyboard→PSX gamepad mapping (`src/controller.c`). WASD=D-pad, SPACE=Start, Backspace=Select, E/C/Z/X=△○×□, Q/R=L1/R1, Shift/Ctrl=L2/R2.
- **SIO RX priming**: `rx_data` initialized so BIOS GetC (B0[0x32]) finds input immediately without spinning.
- **IRQ7 (IRQ_CTRLMEMCARD)**: Added STAT_IRQ + `pending_irq` in `sio_handle_transfer` when CTRL bit 12 enabled.
- **I-Cache**: 256-line 4-word instruction cache with tag/valid bits (`src/cpu/cpu_icache.c`).
- **GTE**: Geometry Transformation Engine (`src/gte.c`) with load delay slots.
- **Event Scheduler**: DuckStation-style event dispatch (`src/event_scheduler.c`).

---

## [sony-logo] — 2026-02

### Achieved
- Sony Computer Entertainment logo renders correctly (textured polygon, correct colors)
- VRAM debug view visible in early renderer (raw VRAM shown in top-right corner)
- GPU command pipeline running: DMA ch2 linked-list → GP0 decode → OpenGL draw

### Added
- **BIOS TTY capture**: EXP2+0x23 DUART write captured in `interconnect_store8` → line buffer → `fprintf(stderr, "[BIOS TTY] ...")`. A0/B0 syscall side-channel via `handle_a0/b0_syscall` (LLE — no fake `$v0` returns).
- **DMA IRQ**: `interconnect_perform_dma` signals IRQ3 on transfer complete.
- **GPU double-buffer**: VRAM regions (0,0) and (0,240) alternated per frame via draw offset GP0(0xE5).

---

## [init] — 2025

### Project start
- Based on *PlayStation Emulation Guide* by Lionel Flandrin (`guide.tex`, ~11K lines). Implementation follows the guide end-to-end: CPU instruction-by-instruction BIOS trace, memory bus, DMA, GPU command dispatch, OpenGL renderer, debugger, and instruction cache.
- Full standard MIPS I instruction set implemented: arithmetic, logic, load/store, branch, jump, shift.
- COP0 exception handling: SYSCALL, overflow, address error, bus error; EPC/SR/Cause registers.
- Memory bus: RAM (2 MB), BIOS ROM (512 KB), Scratchpad (1 KB), hardware register routing skeleton.
- Basic GPU: OpenGL 3.3 + GLEW backend, monochrome/textured polygon rendering.
- CDROM: initial command dispatch and disc image read (CUE/BIN).
- Timers: counter registers, mode, target; clock source stubs.
- DMA: controller init; SBUS registers for BIOS hardware detection.
- Multiple reboots/restarts as architecture was refined; settled on pure C11 single-pass build (`make`).
- First BIOS progression: instruction fetch, memory map routing, early loop recognition.
