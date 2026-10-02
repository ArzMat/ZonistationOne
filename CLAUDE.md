# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

PS1 emulator written in C. SDL3, with an OpenGL 3.3 (GLEW) and a Vulkan 1.3 backend that can be
swapped while a game runs. Early development.

```bash
make                    # build
make clean && make      # clean build
make test               # unit tests: tests/*_test.c, no SDL, BIOS or disc needed
make hwtest             # bare-metal PS-X EXEs inside the emulator, GL and Vulkan (tests/hw/)
./ZoniStation_One roms/bios-ntsc.bin                          # BIOS menu
./ZoniStation_One roms/bios-pal.bin --game="games/game.bin"   # a disc
```

BIOS images and disc images are supplied by whoever runs this and are named generically throughout —
no filename here refers to a real dump. The game path must be passed as `--game=<path to the .bin>`;
a bare positional path is taken as the BIOS. Run PAL discs with a PAL BIOS — region mismatch is
detected and rejected as on hardware.

Useful env vars: `ZS1_LOG_LEVEL=<level>`, `ZS1_LOG_STDERR=1` (log to stderr as well as the ImGui
windows), `ZS1_LUA_SCRIPT=scripts/x.lua`, `ZS1_DUMP_FRAME=<path>` + `ZS1_DUMP_FRAME_N=<n>`,
`ZS1_FRAME_PROFILE=1` (per-frame time split plus cycles per instruction),
`ZS1_AUDIO_DUMP=<path>`, `ZS1_SPU_NO_REVERB=1`, `ZS1_PAD_MODE=digital|analog|stick` (boot pad mode;
default digital, as on hardware), `ZS1_UI=gameplay|debug` (which shell the window opens in; default
gameplay), `ZS1_UI_SCALE=<f>` (overrides the display-derived interface scale),
`ZS1_SBI=<path>` (the LibCrypt patch file for the mounted disc, overriding the automatic search),
`ZS1_GFX=gl|vulkan` (which renderer starts; also changeable at runtime from Esc -> Video),
`ZS1_VK_VALIDATE=1` (Vulkan validation layer), `ZS1_GFX_SWITCH_TEST=<n>` (flip the renderer every
n fields — the leak check for the switch path).

Added 2026-10-02 (CHANGELOG, block of that date): `ZS1_VSYNC=0|1|-1` (GL swap interval or Vulkan
present mode; unset leaves the driver's), `ZS1_FORCE_READBACK=1` (GP0(80h)/(C0h) read back every
time instead of only where the rasteriser may have drawn), `ZS1_MDEC_WRAP9=1` (the literal 9-bit wrap
of yuv_to_rgb), `ZS1_DMA_STALL=doc` (opt-in documented DMA cost; changes emulated timing),
`ZS1_CD_XA_HOLD=1` (the old "drive stops for any pending INT"), `ZS1_SPU_RING_TARGET=<frames>`. Lua:
`emu.on_event(fn, "vblank", ...)` filters in C, plus `emu.spu_voice(n)`, `emu.spu_irq()`,
`emu.cd_state()`. Build: `make LOG_MAX_LEVEL=TRACE` (TRACE is compiled out otherwise),
`make PGO_GEN=1` / `make PGO_USE=1`.

`ZS1_GPU=nvidia|intel` picks the GPU on this hybrid machine — it sets the PRIME offload variables
before the context is created, and the run logs which driver it got and whether the request was
honoured. Undefined GL behaved differently on the two, so *always* check that line before treating a
rendering difference as an emulator bug.

`ZS1_RAM_LOAD_STALL=<n>` overrides the per-load RAM cost (default 3). `0` restores the old flat
one-cycle-per-instruction timing, which is the honest A/B — `VSync: timeout` then returns.

BIOS: SCPH-1001 (US), SCPH-7502 (PAL). Branch: `stable_branch`. Compiler: `gcc -std=c99`.

---

## The two shells

The window has a **gameplay shell** and the **debug workspace**, and switches between them at any
time with the backquote key (`` ` ``), Shift+F1, the *Gameplay* button on the machine bar, or the
quick menu's *Debug workspace*. `ZS1_UI=debug` opens straight into the workspace. The machine keeps
running across a switch.

- **Gameplay shell** — the emulated screen and nothing else. A HUD (fps, speed, host frame ms,
  region and refresh, the pad's LED) fades out after ~3 s idle and returns on any input. `Esc` opens
  a quick menu: session counters, save/load into four slots (slot 0 is the one F5/F8 use), pad mode,
  a machine summary, the workspace, and quit. Nothing here polls the core — the shell asks and
  `main.c` carries the request out (`debug_ui_take_state_request`, `debug_ui_take_quit_request`);
  `Esc` reaches it through `debug_ui_escape_pressed()`, which returns false in the workspace so the
  key still quits there.
- **Debug workspace** — the nine view modes, the log dock and the inspector, unchanged.

**Nothing on either shell is typed in.** The Host HW panel and the inspector's host block read
`/proc`, `/sys`, `uname` and the live GL and SDL device strings through `src/core/host_info.c` —
including which GPU actually got the context and whether a `ZS1_GPU` request was honoured, which is
the first thing to check before blaming a rendering difference on the emulator. Thread load comes
from `/proc/self/task`, sampled twice a second. The pinned watches are Lua expressions evaluated
through `lua_debug_eval_expr()` — the Script console's whole `emu.*` surface, evaluated every sixth
frame and only while the panel is visible; click a tile to edit its expression, right-click to drop
it. The Pipeline view's status words are the drive state, the MDEC decode state, the DMA channel's
sync mode, GPUSTAT, the display state, the count of keyed-on voices and the ring depth; per-frame
figures come from the frame event ring, recorded only while a view that reads it is open.

The Frame view holds a frame on demand (a copy — recording carries on), plots each event by CPU
cycle with a millisecond axis, shows the payload beside the count, marks the display flip across
every row, and marks the budget line only when the frame overran it.

The Controller window draws a DualShock: every control is lit from the same 16-bit word the SIO
sends the game, the sticks show their live deflection, the LED shows the pad mode's documented
colour, and clicking a control rebinds it. The scancode list is still there, folded away.

Fonts and spacing follow the display: a UI face, a display face and a real monospace face are loaded
at a scale taken from `SDL_GetWindowDisplayScale()` (or the window's pixel height), and
`ImGuiStyle::ScaleAllSizes` follows. `ZS1_UI_SCALE` overrides it.

---

## Architecture

```
src/main.c                     — host shell: SDL, GL context, audio device, threads, frame cap

src/cpu/
  cpu_execution.c              — main CPU loop (DuckStation-style downcount)
  cpu_instructions.c           — MIPS R3000A instruction execute
  cpu_decode.c                 — instruction decode
  cpu_disasm.c                 — disassembler
  cpu_init.c                   — CPU init, register reset
  cpu_registers.c              — register file helpers
  cpu_exceptions.c             — EXCEPTION_* handler, EPC/SR/Cause
  cpu_bios.c                   — A0/B0 syscall side-channel (LLE TTY capture)
  cpu_icache.c                 — 256-line 4-word instruction cache

src/core/
  interconnect.c               — init, CDROM event scheduling, TTY buffer
  bus.c                        — memory routing: load/store 32/16/8
  bus_irq.c                    — IRQ edge-triggered controller (I_STAT / I_MASK)
  ram.c                        — 2 MB RAM
  bios.c                       — BIOS ROM load
  dma.c                        — DMA channels 0-6, linked-list + block, completion IRQ
  timers.c                     — PSX timers 0/1/2
  sio.c                        — SIO / JOY controller protocol
  controller.c                 — keyboard → PSX gamepad mapping
  mdec.c                       — Motion DECoder (working: RLE/IDCT/YUV→RGB, FMV verified)
  pcdrv.c                      — PC drive side-channel
  event_scheduler.c            — the single scheduling authority (VBlank, timers, CDROM, DMA slices)
  system.c                     — "run one frame": the machine's timing loop
  debugger.c                   — breakpoints, watchpoints, execution trace
  lua_debug.c                  — Lua scripting surface (emu.*) for live debugging

src/gpu/
  gpu.c                        — GPU init/reset/GP1/GPUSTAT, gpu_reapply_renderer_state()
  gpu_commands.c               — GP0 256-entry dispatch table, all draw commands
  gpu_helpers.c                — GP0 helper utilities
  renderer.c                   — backend dispatch: forwards renderer_* onto the live GfxBackend
  renderer_gl.c                — OpenGL 3.3 backend, unified VRAM texture, GPU thread
  vk/vk_loader.c               — runtime Vulkan entry points (no -lvulkan)
  vk/vk_device.c               — instance, physical device, logical device, swapchain, caps
  vk/renderer_vk.c             — Vulkan 1.3 backend (dynamic rendering, no VkRenderPass)
  vk/vk_imgui.cpp              — C bridge onto imgui_impl_vulkan
  shaders/*.{vert,frag}        — GLSL, compiled to SPIR-V at build time
  vram.c                       — 1024×512 VRAM buffer management

src/gte/
  gte.c                        — GTE register I/O, command dispatch
  gte_ops.c                    — all GTE operations

src/cdrom/
  cdrom.c                      — CDROM controller + command dispatch
  cdrom_commands.c             — all CDROM command handlers
  cdrom_disc.c                 — disc image read (CUE/BIN)
  cdrom_audio.c                — CDROM audio (XA/CDDA)

src/spu/
  spu.c                        — SPU init/register I/O
  spu_voice.c                  — 24-voice management
  spu_adsr.c                   — ADSR envelope
  spu_mixing.c                 — stereo mix output
  spu_dma.c                    — SPU DMA transfers
  spu_irq.c                    — SPU IRQ logic

src/utils/
  log.c                        — 16-category / 6-level logger
  rxi_log.c                    — rxi log backend

src/debug_ui.cpp               — ImGui debug UI (C++)
tests/*_test.c                 - unit tests, each includes the source under test (make test)
tests/hw/                      - bare-metal hardware tests run inside the emulator (make hwtest)
```

---

## Memory Map (physical addresses)

| Region | Range |
|--------|-------|
| RAM | 0x00000000–0x001FFFFF |
| Scratchpad | 0x1F800000–0x1F8003FF |
| SIO / JOY | 0x1F801040–0x1F80104F |
| IRQ regs | 0x1F801070–0x1F801077 |
| DMA | 0x1F801080–0x1F8010FF |
| Timers | 0x1F801100–0x1F80112F |
| CDROM | 0x1F801800–0x1F801803 |
| GPU | 0x1F801810–0x1F801817 |
| SPU | 0x1F801C00–0x1F801E7F |
| EXP2 (BIOS TTY) | 0x1F802000–0x1F803FFF |
| BIOS ROM | 0x1FC00000–0x1FC7FFFF |

---

## CPU Cycle Model (DuckStation-style)

`Cpu.downcount` decrements per instruction. When ≤ 0, `eventq_dispatch_due()` fires.
`Cpu.muldiv_completion_tick` set by MULT/DIV (7/37 cy); MFHI/MFLO stall until reached.
`inter->cpu_cycle_counter` is the global monotonic tick counter (in `Interconnect`).

---

## Logging

17 categories: `SYSTEM CPU IRQ DMA GPU CDROM TIMER BIOS INTERCONNECT RENDERER EVENT GTE VRAM RAM DEBUG MDEC SPU`  
6 levels: `SILENT ERROR WARN INFO DEBUG TRACE`  
Default runtime level: **INFO** (`src/utils/log.c`). All ImGui log windows open at startup.

Macro pattern: `LOG_<CATEGORY>_<LEVEL>(fmt, ...)` e.g. `LOG_GPU_DEBUG("gp0=0x%08x", v)`

Every line carries the machine's clock, not the host's: `[f1397   t  27.4809]` is the CRTC field
count and emulated seconds (`LogClock` in `log.h`, fed by the Interconnect). Wall seconds are
useless for comparing against another emulator — a whole boot phase lands inside one second — and
the field number is the axis a DuckStation run can be put on too (count its `Now in v-blank`).

**Log level policy**:
- ERROR: hardware faults, invalid state
- WARN: recoverable anomalies, dropped commands
- INFO: init, major state changes only
- DEBUG: register writes, IRQ events, command dispatch
- TRACE: per-instruction, per-transfer, per-primitive (hot path, never in default builds: compiled
  out unless built with `make LOG_MAX_LEVEL=TRACE`, see `ZS1_LOG_AT` in `include/log.h`)
- No counter-based rate-limiting — use level gating

Key format conventions:
- IRQ: `"{name} IRQ triggered/cleared"`, `"Interrupt mask <- 0x%08x"`
- GP1: `"Display address start <- 0x%08x"`, `"Set display mode <- 0x%08x"`
- CDROM: `"Execute command {name} (0x%02X)"`, `"Drive state: OLD -> NEW (LBA N)"`
- GTE unhandled: fires once per unique opcode via bitmask seen-set (`seen_opcodes`)

---

## Licensing constraint

The project is **GPL-3.0-or-later**; every source file carries an SPDX header and
`THIRD-PARTY.md` is the inventory.

- **Never copy code from `duckstation_ref/`.** DuckStation has been CC-BY-NC-ND-4.0 since
  2024-09-01: no derivative works, no commercial use, incompatible with the GPL. The submodule is
  there to answer "what does hardware do here" when `DOCS/` is ambiguous, and for nothing else.
  Describing its behaviour in a comment is fine; reproducing its structure or its code is not.
- `pcsx-redux/` is GPL-2.0-or-later, so code from it *can* be used — with the attribution header kept
  intact, which is the licence condition being satisfied. Do not strip those headers.
- Prefer `DOCS/` over both. Anything implemented from a cited `DOCS/` line carries no third-party
  copyright, and the citation is what makes that checkable later.

## Conventions

- Pure C (C99). C++ only in `src/debug_ui.cpp` (ImGui wrapper).
- No `malloc` in hot paths. Structs embedded, not heap-allocated.
- SDL3 + GLEW + OpenGL 3.3 Core. ImGui via `third_party/imgui/`.
- `inter->cpu` pointer set after CPU init via `interconnect_set_cpu()`.
- IRQ lines are edge-triggered: `interconnect_set_irq_line(inter, IRQ_X, true/false)`.
- Exception flow: `cpu_exception(cpu, EXCEPTION_*)` saves EPC, updates SR mode stack.
- LLE style: BIOS syscalls run normally; A0/B0 hooks are side-channel only (no fake returns).

---

## Known Working

- BIOS boot to menu (US and PAL), full 3D boot logo
- `Ace Combat 2 (Europe)`: **full gameplay** — boot, FMV intro, textured menus, missions and
  memory-card saves, played through and stable
- `Monsters & Co. (Italy)`: boots, plays its FMV intros, reaches the title screen and 3D engine
  (2026-08-10, after the DMA fix below), and **starts a new game** (2026-08-17, after the GetlocL
  fix below). Gameplay still shows the five measured defects listed under "State of the Monsters &
  Co. work" further down.
- `Crash Bandicoot 3 - Warped (E)` [SCES-01420], run from a **`.bin.ecm`**: **full gameplay**
  (2026-08-20, after the LWL/LWR fix below) — the first disc played start to finish from a compressed
  image, so the ECM path is exercised under real seek and streaming load, not just at boot.
- `Dino Crisis (E)` [SLES-02207], from a **`.bin.ecm`** plus its `.sbi`: boots past the protection,
  plays its opening screens and reaches the **main menu** (2026-08-21). The disc is **LibCrypt**, and getting there took four separate
  fixes, none of them in the CDROM data path — see the four entries in `CHANGELOG.md` under this
  date. The one to remember: the protection keeps its own state in COP0's breakpoint registers,
  which is documented behaviour ("mis-used as general-purpose registers",
  `psx-spx-docs/docs/cdromformat.md`) and not something a CDROM trace can ever reveal.
- **LibCrypt discs need an `.sbi`, and it must be the *same pressing*.** The 16-bit key lives in the
  subchannel Q of 32 sectors, which no `.bin` dump holds. The file is found by the image's name with
  extensions dropped one at a time, then the lone `.sbi` beside the image if there is exactly one;
  `ZS1_SBI` overrides. `SLES-02210` is the Italian Dino Crisis and `SLES-02207` the English one:
  the wrong file loads without complaint, patches sectors the game never reads, and the game hangs
  exactly as if there were no file at all. DuckStation's `gamedb.yaml` lists `libcrypt: true` per
  serial and is the quickest way to check whether a disc needs one.
- **ECM images decode on the fly** (`src/cdrom/cdrom_ecm.c`, `src/cdrom/ecm_edc.c`). Never suspect
  the decoder from game behaviour: the container appends the EDC (CRC32, poly 0xD8018001) of the
  *entire* decoded output as its last four bytes, so decoding the whole image and running that CRC
  proves it byte-identical to the original `.bin` in one step. Crash 3 matches at `6264BE2A` over
  146562 sectors in 1.8 s. The follow-up A/B is to decode to a plain `.bin` and re-run: an identical
  failure field-for-field means ECM is not involved at all. Both were needed to stop a wrong
  LibCrypt/SBI theory — `docs/ecm_libcrypt_discovery.md` records it.
- **LWL/LWR must merge against `delay_load_reg`, not `load_reg_idx`** (`cpu_instructions.c:806,836`).
  `load_reg_idx` is the slot for the load *this* instruction issues and `cpu_retire_load_delay()` has
  just cleared it, so the test never matched and the second half of every pair merged into the stale
  register (`include/cpu.h:131-132`; psx-spx `cpuspecifications.md:247-252` — "There's no delay
  required between lwl and lwr"). It hid for months because it is **data-dependent**: an
  `(lwl+3, lwr+0)` pair writes the whole register twice and looks right; only a partial pair like
  `(lwl+1, lwr+2)` shows it. Crash 3's ISO path-table walk alternates the two and read half its
  directory extents as `bfc00018` instead of `00000018`. Suspect this class of bug — "correct except
  at one alignment" — whenever a defect depends on the data rather than the code path.
- **The no-disc status byte reports ShellOpen** (`cdrom.c`, `cdrom_reset`/`cdrom_init`). Bit 4 is a
  latch, "Once shell open (0=Closed, 1=Is/was Open)" (`psx-spx-docs/docs/cdromdrive.md:826`); with no
  disc the shell has necessarily been open and a reference run answers every Getstat with 10h.
  Forcing it to 0 told the BIOS a disc might be there: it issued GetID, took INT5(08h,40h), and
  looped Getstat/Getstat/GetID for the whole session instead of finishing the shell's init.
- **GetlocL answers from a latch, not from the data sector ring** (`cdrom_commands.c`). The ring
  entry is cleared once the guest has DMA'd the sector out, and GetlocL is asked after that, so
  answering from the ring failed on every consumed sector and the game looped on the failure. Only
  **data** sectors latch: a reference run over a section interleaved 1 data : 3 audio answers with
  the data cadence (MSF stepping by exactly 4), so latching the ADPCM sectors as well takes the
  game's demuxer off the video stream and its speech never plays — that was measured, both ways.
- DMA: a CHCR write that clears the start bit cancels an in-flight sliced transfer. Sliced channels
  keep their remaining count in `Dma`, not in the registers, so anything that stops a channel outside
  its own completion path has to call `dma_cancel_slice()`
- GP1(03) display-off blanks the screen to black (`DOCS/graphicsprocessingunitgpu.md:647`) instead of
  scanning the display window out of VRAM
- GPU: polygons, rects, lines, textured/CLUT, semi-transparency, scissor, unified VRAM texture,
  15bpp and 24bpp display
- **The renderer sits behind a vtable** (`include/gpu_backend.h`, since 2026-08-25). `renderer.c` is a
  dispatcher; the whole OpenGL implementation is `renderer_gl.c` plus its private `renderer_gl.h`,
  which is the only place besides `main.c` allowed to include `<GL/glew.h>`. That header used to be
  `include/renderer.h`, which `gpu.h` includes, so `GLuint` reached every unit that touched the
  interconnect. The ~120 `renderer_*(&gpu->renderer, ...)` call sites are unchanged.
  Two things about it that are not obvious:
  - **Bind the backend before the machine is built.** `gpu_reset_state()` pushes the GP0/GP1 reset
    values through four renderer setters (`gpu.c:661-668`) from inside `interconnect_init()`, which
    runs *before* `renderer_init()` (`main.c:446` against `:448`). Those values are not redundant —
    `glr_init()` defaults only screen width/height — so `renderer_select_backend()` binds the vtable
    without touching the GPU, and the backend resolves a NULL `impl` to its own file-static state.
  - **Savestates were not affected.** `savestate.c:76-78` derives both `Gpu` spans from
    `offsetof(Gpu, renderer)` and `sizeof(Renderer)`, so shrinking `Renderer` from ~1 MB to two
    pointers moved both boundaries by the same amount. Verified: a state saved and reloaded across
    the change restores PC and cycle exactly and the machine runs on.
- **Vulkan 1.3 is the second backend** (`src/gpu/vk/`, since 2026-08-25), and it is swapped **live**
  from the quick menu's *Video* entry. Things about it that cost time to learn:
  - **`renderer_upload_vram()` is a no-op on Vulkan, on purpose.** GL records the whole-VRAM upload
    `main.c` does every frame with `update_display=false`, so it feeds only the `GL_R16UI` mirror
    that non-`ARB_texture_barrier` drivers sample — never `vram_tex`. Vulkan has one VRAM image, so
    honouring that upload erased the rasteriser's work once a frame; the picture was "completely
    broken" and looked like a display bug. Only `upload_vram_rect()` and rasterisation write it.
  - **`VK_EXT_fragment_shader_interlock` is an optimisation, not a prerequisite.** The feedback loop
    (the PS1 fragment shader samples the image it is also drawing to) is handled by a
    `vkCmdPipelineBarrier` between batches with the image permanently in `VK_IMAGE_LAYOUT_GENERAL`,
    which works everywhere. That is the GL `glTextureBarrier()` call, in Vulkan terms.
  - **Vulkan is loaded at runtime, not linked.** `VK_NO_PROTOTYPES` plus `SDL_Vulkan_LoadLibrary()`,
    so there is no `-lvulkan` and the binary starts on a machine with no ICD.
  - **Shaders are files now.** `src/gpu/shaders/*.{vert,frag}` compiled to SPIR-V by
    `glslangValidator` at build time; the Makefile degrades to GL-only with a message if either
    `libvulkan-dev` or `glslang-tools` is absent.
  - **The GL backend needs X11 on this box.** Under `SDL_VIDEODRIVER=wayland`, `glewInit()` returns
    "Unknown error" and the backend refuses to start — which is also what a hot switch *to* GL hits
    on a Wayland session, where it rolls back to Vulkan. Vulkan on the Intel iGPU is the reverse and
    needs Wayland; see the memory note.
- **The renderer switches while the machine runs** (`switch_gfx_backend()` in `main.c`, 2026-08-25).
  SDL fixes `SDL_WINDOW_OPENGL`/`SDL_WINDOW_VULKAN` at window creation, so the window is destroyed
  and rebuilt — which makes three things the switch has to carry, and each was a real failure before
  it did:
  - **VRAM comes back from the GPU, not from `gpu.vram.data`.** The CPU-side array holds what the
    CPU wrote and never what the rasteriser drew. `renderer_read_vram_rect()` is a synchronous
    round-trip *through the GPU thread*, so it has to run before `renderer_stop_gpu_thread()`.
  - **`gpu_reapply_renderer_state()`** (`gpu.c`) re-sends draw offset, drawing area, texture window,
    screen scale, display region, depth24, blank and the two mask flags. All ten live in the backend,
    not in `Gpu`, and a new backend starts at its own defaults. The per-primitive state (dither,
    semi-transparency, texture/raw-texture mode) is deliberately *not* re-sent: every draw command in
    `gpu_commands.c` sets it immediately before pushing geometry.
  - **The ImGui context survives; only its two backend halves are rebuilt.**
    `debug_ui_backend_shutdown()`/`debug_ui_backend_init()` exist for exactly this, and
    `ImGui::DestroyContext()` stays in `debug_ui_shutdown()`. The order matters on the way down —
    the halves go before `renderer_destroy()`, because the Vulkan renderer half belongs to the
    device that call destroys.
  A failed switch rebuilds the previous backend and reports it rather than ending the session.
  `ZS1_GFX_SWITCH_TEST=<n>` flips backends every n fields; nine switches in one run of Ace Combat 2
  under X11 were clean.
- MDEC: full decode pipeline, verified against real FMV playback
- DMA: all channels, linked-list + block, completion interrupts
- Timers 0/1/2: derived counters, sync modes, video-mode-derived rates
- CDROM: async command/response, disc region detection, XA audio decode
- SIO: digital pad, DualShock analog protocol (ID 73h/F3h, adc0-3, config commands), analog-stick
  "flight mode" (ID 53h, L3/R3 disabled), rumble on both the old one-motor and the new 4Dh-mapped
  method, both memory card slots
- Pad mode: **digital at boot, as on hardware** (2026-08-19; it used to be analog "so nobody has to
  press a key"). The BIOS shell's own pad driver does not cope with ID 73h: it never finishes its
  init and the no-disc main menu is drawn without its selection cursor. A reference run of the same
  PAL BIOS answers the pad 41h 5Ah. F12 or the DS4 touchpad click is the Analog button and cycles
  digital -> analog -> stick; `ZS1_PAD_MODE=digital|analog|stick` sets the boot mode; a game's own
  44h config command switches it too. In analog and stick mode the left stick no longer folds onto
  the D-pad, so a push arrives once, as adc2/adc3. Stick mode is what a few titles want by name
  (DOCS/controllersandmemorycards.md:496: Ace Combat 2, MechWarrior 2, Colony Wars).
- Controllers: DS4 over USB/Bluetooth via SDL_Gamepad, hot-plug, keyboard live alongside it,
  radial stick deadzone so a resting stick reads 80h. The DS4's light bar mirrors the emulated pad's
  documented LED colour (`DOCS/controllersandmemorycards.md:369-372` — digital off, analog red,
  stick green), written only when the colour changes; it needs hidraw access, see the traps below
- GTE: all 22 ops with cycle costs charged to the CPU
- I-Cache: 256-line 4-word with tag/valid bits
- SPU: sample generation on the emulated clock (EVQ_SPU event + catch-up on register access)
- **Savestates**: full machine, format **v12** (every bump is listed above `ZS1_STATE_VERSION` in
  `savestate.c`). F5 saves, F8 loads, `emu.save_state`/`emu.load_state` from Lua. Older states are
  refused. v12 (2026-10-02): the CPU trace ring shrank, the SPU gained its manual-write FIFO, and a
  save reads the renderer's VRAM back first, so pixels only the rasteriser wrote survive a load.
- CPU memory timing: RAM data **loads** cost 3 cycles (1 documented from RAM_SIZE bit 7, 2
  calibrated); stores are free because the write buffer absorbs them. CPI lands ~1.6, tracked and
  printed by `ZS1_FRAME_PROFILE=1`. This is what stopped the BIOS printing `VSync: timeout` on
  every call.
- **BIOS ROM wait states are charged** (`cpu_icache.c`, ~24 cycles/word from MEMCTRL) since
  2026-08-17. They had been disabled for a long time because they hung boot; the hang was two
  CDROM defects, not a CPU one, and both are fixed. Boot milestones now land within ~4% of a
  DuckStation Devel-build run of the same disc, against 1.8x-7x too fast before, and the machine
  still holds 50 fields/s.
- **CDROM response deadlines are real.** Two rules, both learned the hard way (2026-08-17):
  - A pending INT blocks delivery, and when the guest acks, the drive owes the time that is *left*,
    not a fresh minimum. Anything that defers a CDROM event re-arms from
    `cmd_deadline`/`second_deadline`/`drive_deadline` via `cdrom_delay_left()`, never from
    `CDROM_MIN_INT_DELAY`. Sector delivery is paced from the previous sector, so INT1 lands every
    6.7 ms at 2x (`DOCS/cdromdrive.md:1913` gives 0x36cd2).
  - **A re-issued command must not restart work already owed.** `Init` takes ~740 ms — longer than
    the ~415 ms after which the BIOS re-issues it — so the retries are expected and the drive
    answers the first request at its own deadline. Rescheduling on each retry pushed the deadline
    forward forever: boot sat in an endless `Init` loop with the screen frozen on the PlayStation
    logo, which looked exactly like a CPU-timing hang and was blamed on one for months.

## Known Broken / Absent

- **A VRAM write must flush whatever primitives are still pending.** `renderer_draw()` both builds a
  batch and records its position in the frame's op list, so a primitive that has been submitted but
  not flushed has no place in the order yet, and a VRAM op recorded meanwhile will be replayed before
  it. `renderer_upload_vram_rect()` missed this and Dino Crisis lost every uploaded picture under its
  own back-buffer clear — the images were in VRAM the whole time, which is why it looked like a
  display bug. Fixed 2026-08-21. Anything new that touches VRAM from the CPU side has to flush first;
  the reference emulator does it on the same four boundaries (FillVRAM, UpdateVRAM, CopyVRAM,
  DownloadVRAMFromGPU).
- **The gameplay shell cannot change what is running.** Three pieces of the shell's design are not
  implemented, and each is blocked on machine-side work rather than on interface work:
  - **No disc library.** The disc still comes from `--game=` on the command line. `games/` is not
    scanned and nothing in the shell can start a title; the quick menu describes the disc that was
    loaded at startup and nothing else.
  - **No disc swap while running.** `cdrom_load_disc()` exists and would load the image, but a swap
    on hardware is a shell-open / shell-closed cycle the drive has to report — the status byte's
    bit 4 latch, the pending INT the guest is owed, and the region check that rejects a PAL disc
    under the US BIOS. None of that is wired, so a swap today would hand the guest a new image
    behind its back. The disc list in the quick menu is a summary, not a picker.
  - **No reset.** There is no `system_reset()`: nothing re-runs CPU and Interconnect init against a
    live machine, and the savestate path is not a substitute because it restores rather than
    restarts. *Reset console* was left out of the quick menu rather than wired to something that
    only looks like a reset. Whoever adds it should treat the BIOS boot path as the reference —
    `main.c` builds the machine once today, and the reset has to reach the drive, the SPU's RAM and
    the event queue, not just the CPU.
- **The shell's settings are read-only where the machine has no setter.** Volume, scanlines, scaling
  mode and crop appear in the interface study's design and are not in the shell: the renderer has no
  runtime switch for any of them, and reverb is guest state (SPUCNT), not a host preference. The
  quick menu shows what the machine reports and offers the controls that do exist — pad mode,
  savestate slots, the workspace, quit.

- **Audio in `Dino Crisis (E)`'s in-engine 3D cutscenes: repeats across some scene changes, and runs
  ahead of the scene.** Both reported 2026-08-21, both absent from the FMVs, neither measured. The
  FMVs staying in step is the useful half of the observation: it puts the XA path and the output
  device in the clear and points at the SPU's own clock. Do not quote a drift figure from a run with
  logging or a probe on — see the trap below; a guest burning cycles in a retry loop and a host that
  cannot keep up look identical here and need opposite fixes.
  **2026-10-02:** the documented candidates are fixed (XA filter routing, the drive feeding XA under
  a pending INT, SPU IRQ address and loops, volume sweeps, Mute/ATV/AVOL; CHANGELOG of that date),
  and none of it has been re-tested on the disc yet. `scripts/cutscene_audio_classify.lua`, run from
  a savestate just before the cutscene, says which audio mechanism the scene uses.
- **SPU pops during speech** — the open defect. Sounds like clipping, but the final mix peaks far
  below full scale (5869/6343 of 32767 observed), so any saturation is at an intermediate stage.
  `scripts/spu_clip_probe.lua` reports the reverb network's in/out peaks and rail hits alongside the
  XA and ring drop counters, which separates saturation from a dropped sample — the two sound alike.
  `ZS1_SPU_NO_REVERB=1` is the one-run A/B. Not yet reproduced from a fixed point: no v3 savestate
  inside a speech scene exists.
- **iGPU rendering artifacts — fix committed, not verified.** Textures came out as flat blocks of
  saturated colour on Mesa/Intel and clean on the NVIDIA dGPU. Three pieces of undefined GL were
  corrected (unsized `GL_RGB` scanout target, float-to-uint on a possibly-negative interpolated UV,
  `GL_DITHER` left enabled). The UV fix alone was tested and did not resolve it; the format fix is
  the one that had not been tried when that was reported. Needs an iGPU run to confirm.
- GPU: CRTC ticks once per frame. (The mask-bit test is applied by both shaders, GP0(C0)/(80) read
  the renderer back where it may have drawn, and the R16UI sampling mirror is used only on a driver
  without ARB_texture_barrier.)
- **Display window: size fixed, position still not applied.** Width comes from GP1(06) with the
  divider table, height doubles only for interlaced 480 lines, GP1(05) X is no longer masked to even
  and 368 mode decodes (`gpu.c:83-140`). Still open: the X1/Y1 position is ignored, so screen-shake via
  GP1(06)/(07) stays invisible, and the overscan crop applies to NTSC only.
  `docs/GPU_DISPLAY_STUDY_2026-08-10.md` has the full list.
- **Display state is snapshotted at the end of the field**, at frame submit, and applied to the whole
  field; hardware latches per line. A game that changes depth or window part-way through a field gets
  one wrong field from us — visible as the stretched 15bpp-read-as-24bpp frame after an FMV.
- No multitap. No Dualshock2 pressure sensing; digital-mode transfer length does not grow when
  motors are mapped to config bytes cc..ff.

`docs/TESTING_PLAN_2026-08-20.md` is authoritative for **testing**: what exists (nothing automated),
the four layers proposed, and the order. Read it before adding a test, and before claiming a
subsystem is verified.
The checks the 2026-10-02 work still owes a BIOS and a disc are written as step-by-step cards, in
Italian, in `docs/PROVE_MANUALI_2026-10-02.md`: a session that has the discs starts there.

See `docs/GAP_ANALYSIS_REFACTOR_2026-07-13.md` (per-subsystem state + work queue) and
`docs/GPU_GAP_ANALYSIS_2026-07-15.md` (renderer deep dive) — both rewritten 2026-07-28 and authoritative
over this file for status. `docs/GPU_DISPLAY_STUDY_2026-08-10.md` covers everything between VRAM and
the screen (display window, scanout, overscan, latch order) and is authoritative for that path.
`docs/CDROM_AUDIT_2026-08-17.md` is the drive's audit against the **official** psx-spx clone in
`psx-spx-docs/` (newer than `DOCS/`, gitignored the same way) and is authoritative for the CDROM.
Its rule, and the standing rule for any audit here: no entry may claim "correct" without citing
both a documentation line and a code line; anything not compared is marked UNVERIFIED. **Complete
since 2026-08-17**: all five CDROM doc files read end to end, and every file in `src/cdrom/` plus
`include/cdrom*.h` and the CDROM paths in `bus.c`/`bus_irq.c` compared line by line. Part 7 lists
sixteen findings in impact order — the dead ATV volume matrix, the 16/32-bit register decode, the
missing GetlocL/Pause seek-phase refusals and Setloc BCD check are the first four. Part 8 lists what
the documentation cannot settle.
`docs/DMA_IRQ_GTE_MDEC_AUDIT_2026-08-17.md` does the same for **DMA, interrupts, the GTE (including
`gtepipelinetimings.md`), MDEC and `hardwarenumbers.md`** — same rule, same format. Its Part 3 ranks
sixteen findings: sub-word writes to the DMA/IRQ registers use the wrong model, DICR's master flag
wrongly factors in the per-channel enables, DMA completion clears I_STAT.3 behind the CPU, the DMA
bus-error flag is never raised, MVMVA never resets FLAG, and GTE LZCR is UB for LZCS=FFFFFFFFh.

---

## Picking this up on another machine

Development moved off WSL because the debug interface could not keep real time there — the emulation
core could, with the panels closed. On a native Linux box, re-measure before assuming anything about
speed.

**Build dependencies** (Ubuntu/Debian):

```sh
sudo apt install build-essential libglew-dev libgl1-mesa-dev
make
```

SDL3 is not packaged on Ubuntu 24.04 or its derivatives (Zorin 18 included), so it is built from
source once and installed to `/usr/local`; the Makefile picks it up through `pkg-config sdl3`, which
is what supplies the `-L` and the `-rpath`:

```sh
sudo apt install cmake libwayland-dev libxkbcommon-dev libx11-dev libxext-dev libasound2-dev
git clone --depth 1 --branch release-3.2.24 https://github.com/libsdl-org/SDL.git
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build SDL/build -j"$(nproc)"
sudo cmake --install SDL/build && sudo ldconfig
```

Everything else (ImGui, Lua) is vendored in `third_party/`. The reference emulator clones live in
`duckstation_ref/` and `pcsx-redux/` as submodules — they are consulted for behaviour, never linked.

**State as of 2026-08-04** (branch `debug`, pushed to `origin/debug`):

- Boots the BIOS and `Ace Combat 2 (Europe)`; the FMV intro decodes and displays correctly.
- The machine is an i9-14900HX with an Intel iGPU **and** an RTX 4060. Which one gets the GL context
  changes rendering behaviour, so always check the startup log before judging a visual defect.
- Host cost is ~3.7ms against a 20ms PAL field with the panels closed. There is roughly 5x headroom;
  when something *feels* slow, it is the emulated machine's cycle budget, not the host. Check the CPI
  in `ZS1_FRAME_PROFILE=1` before looking anywhere else.
- Read `docs/study/README.md` first: the combined CDROM/SPU work queue, ordered by impact per unit of
  effort, plus what is already verified correct so it is not re-investigated. Item 9 (VSync) is done.
- `docs/GAP_ANALYSIS_REFACTOR_2026-07-13.md` and `docs/GPU_GAP_ANALYSIS_2026-07-15.md` hold per-subsystem state.
  `docs/ui/` holds the interface direction the debug UI is being rebuilt against.
- The build emits **zero warnings from this project's own sources**. Keep it that way; the only one
  left is `tmpnam` from vendored Lua at link time.

**Debugging convention**: use the in-tree Lua probes (`ZS1_LUA_SCRIPT=scripts/x.lua`, the `emu.*` API
in `src/core/lua_debug.c`) rather than dumping state and analysing it outside. The Lua surface reads
live internals a dump cannot show — `emu.reverb()` gives the reverb network's in/out, `emu.cd_audio()`
the XA FIFO balance, `emu.audio_stats()` ring drops and underruns. Probes live in `scripts/` and are
re-runnable next session.

**DS4 support is implemented and confirmed working on hardware** (buttons, sticks, hot-plug, keyboard
still live alongside it). Two things remain on it: **rumble is written but never verified with a real
pad**, and there is **no UI for controller state or button mapping** — the mapping is hard-coded in
`controller.c`. `docs/CONTROLLER_DS4_SUPPORT.md` and `docs/CONTROLLER_MAPPING_UI.md` are the design
notes (untracked; commit them if they should travel).

**State of the Monsters & Co. work, 2026-08-17.** Starting a new game no longer hangs (the GetlocL
latch), but a 120 s run against a DuckStation Devel run of the same disc, both on the emulated-field
axis, leaves five measured differences. Register writes, VRAM upload rectangles, MDEC macroblock
counts and DMA volumes all match within the 2.7% field ratio, so the guest behaves the same and the
divergence is in what we do with it:
1. **CD command churn**: per field we issue 13x their `GetlocL`, 21x `Setloc`, 27x `SeekL`, 47x
   `ReadS` (they use 9 `ReadS` in a whole session, we use 614). The game is retrying something.
2. **The Disney Interactive FMV is replaced by 2.8 s of black**: we read 325 sectors from LBA 291021
   with the display off and **MDEC idle**, then the game jumps to LBA 293693 and only then re-enables.
   The reference re-enables after ~50 sectors and keeps decoding through the load.
3. **VRAM on screen** is the missing overscan crop: uploads land at y=8, the display window starts at
   y=0, and DuckStation runs `CropMode = Overscan`. Item 4 below.
4. **Sector rate during a load**: ours 130/s against their 152/s (2x nominal is 150/s).
5. **`CLUT out of VRAM bounds`** 4662 times per 250 fields during gameplay, on 19520 textured quads —
   a GPU defect, independent of the disc.
Sector delivery itself is correct: contiguous, with 1 XA audio sector in 8 routed to the decoder,
matching their 442 data / 63 audio split. The reported "-30% drift and underruns" is **not measured**
— every run had debug logging on, which invalidates any speed figure (see the trap below). One clean
`ZS1_FRAME_PROFILE=1` run, no stderr logging, no Lua probe, decides host versus guest first.

**Next up, in order** (merged 2026-08-17 from both audits' findings and the display study; the two
audit documents stay authoritative for the detail, this is only the sequence). Batches, not single
items: everything inside a batch touches the same code and is verified by the same run.

*Status 2026-10-02, checked against the code* (`docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md` and its
annexes): closed are 1, 2, 3, 4 (the matrix is now applied in the mix), 6, 7, 8, 9, 14, 15 and 20;
12 is partial (overscan crop for NTSC only) and 23 mostly done (readbacks exist, the mirror is
fallback-only). Open: 5, 10 (a field-start latch was tried and switched off), 11, 13, 16, 17, 18, 19,
21.

**A. Free correctness — one-liners, no behaviour risk** (do first: they remove noise from every
measurement after them)
1. GTE: MVMVA must reset FLAG (`gte_ops.c:135`), LZCR is UB for LZCS=FFFFFFFFh (`gte.c:61-66`),
   MVMVA garbage matrix uses RT21 where the doc says RT22 (`gte_ops.c:151-152`), ORGB is read-only
   (`gte.c:90-92`).
2. CDROM: GetTD's parameter is BCD (`cdrom_commands.c:436-441`); Sync/17h/18h answer INT5(11h,40h)
   (`:183-186`, `:462-471`); Reset sends no INT2 (`:553`, `:652-655`).
   *Verify:* one clean boot to the BIOS menu plus one disc boot; Reset is the only one that can
   regress boot.

**B. The CD command churn** — the measured Monsters & Co. defect #1
3. GetlocL must fail 80h during the seek phase and on audio tracks (`cdrom_commands.c:377`);
   Pause must fail 80h during seek phases (`:269-284`); Setloc must validate BCD (`:196-203`);
   a re-issued Init must produce no response at all (`:288-289`).
   *Verify:* the 120 s Monsters & Co. run against the DuckStation Devel run, on the emulated-field
   axis, counting GetlocL/Setloc/SeekL/ReadS per field. Today: 13x/21x/27x/47x theirs.

**C. CD audio path** — the whole ATV matrix is dead code today
4. Fix the port mapping (ATV2 dropped, ATV1/ATV3 swapped, 1F801803h bank 3 is ADPCTL not a volume
   register), honour ADPMUTE, and **apply the matrix in the mix** with saturation to double volume
   (`cdrom.c:338`, `:346-347`, `:405-416`). Mute must keep feeding silence, not starve the FIFO
   (`cdrom_audio.c:329`).
5. Take a savestate inside a speech scene (prerequisite for the SPU pops work), then run
   `spu_clip_probe.lua` with and without `ZS1_SPU_NO_REVERB=1`.

**D. Bus-level correctness that silently corrupts state**
6. CDROM 16-bit and 32-bit register decode (`bus.c:332-343`): a halfword RDDATA read must return two
   data bytes, a word read of 1F801800h must return HSTS four times. Today both pop FIFOs as a side
   effect.
7. Sub-word writes to the DMA and interrupt registers latch the shifted source word in full
   (`bus.c:277-292`, `:231-232`).
8. DICR: master flag must ignore the per-channel enables (`dma.c:105-107`); completion must not clear
   I_STAT.3 (`:118-121`); the bus-error flag must be raised on out-of-range transfers (nobody writes
   it — and a runaway DMA has already cost one session).
   *Verify:* boot plus one FMV; 7 and 8 are the two items in this list most able to break both.

**E. Display** — small, each removes a visible defect (`docs/GPU_DISPLAY_STUDY_2026-08-10.md` §4)
9. Display width from GP1(06) with the divider table (10/8/5/4/7), in `gpu_update_display_mapping()`.
10. Latch the display state at field start, not at frame submit.
11. Count GP1(03) writes per field — one probe run; decides whether the long black screens across a
    load are the game's or ours.
12. Overscan crop in the scanout (`DOCS:713-716`).
13. Confirm the iGPU artifact fix — one run `ZS1_GPU=intel` against one `ZS1_GPU=nvidia`.

**F. Pacing** — changes emulated speed, so it comes after the visible work and needs a clean run
14. GPU linked-list DMA runs at ~15.6 clk/word against a documented 1 clk/word plus hyper-page
    (`bus.c:744-745`); the correct model is already in the same file for MDEC (`bus.c:759`).
15. CDROM response timings: Stop's second response is half the measured 1x value and ignores speed
    (`cdrom.h:70-71`); the first response is a flat 25000 against a 50401 average and a distinct
    81102 for Init (`cdrom.h:35`).
    *Verify:* `ZS1_FRAME_PROFILE=1`, no stderr logging, no Lua probe; boot milestones in emulated
    fields against the DuckStation run.

**G. Structural, in decreasing payoff**
16. `CLUT out of VRAM bounds` — 4662 per 250 fields on 19520 textured quads, a GPU defect independent
    of the disc.
17. Sector buffer as a real queue (oldest delivered, jump to newest, silent loss).
18. SubQ: index 00h in pregap, decreasing relative MSF, lead-out track AAh, and PREGAP lines in the
    CUE (`cdrom_disc.c:132-140`, `:282-291`).
19. The CDROM command stubs: Report packet shape, MotorOn/Forward/Backward error paths, SetSession,
    GetQ's INT2 phase, the GetID matrix.
20. MDEC: command 0/4-7 must not consume parameters (`mdec.c:431-435`); revisit the 9-bit clip
    applied to the chroma blocks (`mdec.c:188-190`) — suspect it first if FMV colour looks flat.
21. SWC2 must stall on an in-flight GTE op (`cpu_instructions.c:964-975`); MTC2/CTC2 store delay.
22. Controller UI — live pad state (mode 41h/73h/F3h, buttons, sticks, motors, watchdog) and editable
    mapping, plus verifying rumble against the real DS4.
23. The three GPU items that are really one job (cross-thread GL readback).
24. Long tail, only if a title demands it: GTE input-latch pipeline, LibCrypt (needs a SubQ CRC
    model), sound map, DMA priorities and MADR/BCR writeback.

**Redistribution, as of 2026-08-07**: `guide.tex`, `DOCS/`, `imgui.ini` and the two `.mcd` memory
cards were removed from the index (the working copies stay — `.gitignore` covers them all). `DOCS/`
was the important one: it is the psx-spx fork, whose own README states that "no copyright or license
have been properly acquired to republish and alter this document". Cite it exactly as before; the
paths resolve against a local clone that every checkout has to make (`README.md` §References gives
the two commands).

**Still open**: `guide.tex`, `DOCS/` and the DuckStation-derived code that was later rewritten remain
in the *history*, and a public repository ships its history. Either release from a fresh repository
with a single initial commit, or `git filter-repo` those blobs out and force-push. Not decided. A
`git bundle` preserves all 165+ commits in one file either way — losing the history is separable
from stopping distribution.

**Audited 2026-08-07** (`git log` for the commit): every tracked source in `src/` and `include/` was
scanned against `duckstation_ref/` (1243 files) and `pcsx-redux/` (828 files) two ways — 12-token
shingles over comment-stripped code, keeping only windows with ≥5 real identifiers, and verbatim
comment prose. Result: no meaningful overlap with either. The only literal match is the XA zigzag
interpolation tables in `cdrom_audio.c`, which are the constants printed at
`DOCS/cdromformat.md:908-912`, and the only shared prose is the MIT boilerplate in `rxi_log.c`, which
has to be there. Re-run before a release rather than trusting this line.

**Traps that have each cost a session**:

- **A savestate load used to erase the memory cards.** The cards live inside `Sio`, `T_SIO` is a raw
  read of that struct, so loading a state replaced the live cards with whatever the state was written
  with — and *every* boot writes frame 63 as the card driver's write test, which marks the card dirty
  and rewrites all 128 KB of the `.mcd`. So a state captured with empty cards erased real saves one
  boot later, silently, with no card operation the user performed. Fixed 2026-08-20 (state v10): the
  cards are held across the `T_SIO` read and put back, saves go through a temp file and a rename, and
  the first write of a session copies the card to `<path>.bak` first. If saves are missing anyway,
  the `.mcd` files were tracked in git until 2026-08-07 — `git show 27f3293:memcard1.mcd` recovered an
  Ace Combat 2 save that had already been lost this way.


- **One runaway DMA presents as four unrelated defects.** A sliced transfer that keeps running after
  the guest aborted it wrote MDEC output over ~880 KB of guest RAM, and the report that came back was
  "the boot logo breaks, then the screen shows VRAM, then the audio cuts, and the DMA logs out of
  bounds". Every one of those was the smear: the logo's data, the display list and the GP1 values, and
  the SPU voice tables all lived inside the overwritten range (the SPU log tells it plainly —
  `volL=0x0D0C adsr=8C7A/DFED` before, `volL=0x0000 adsr=0000/0000` after). When several subsystems
  fail at the same instant, look for one write path that is scribbling on RAM before debugging any of
  them individually. The `out of bounds` line is the *end* of the smear, not its start, which is why
  the visual damage appears earlier in the log than the error.
- A DS4's light bar stays dark unless the user can open `/dev/hidraw*`. Default is `root:root 0600`,
  so SDL's HIDAPI PS4 driver cannot claim the pad and falls back to the kernel evdev path — buttons,
  sticks and force-feedback rumble all still work, which is why it looks like nothing is wrong, but
  `SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN` reads false and `SDL_SetGamepadLED` fails. Fix is a udev
  rule, then replug the pad:
  `KERNEL=="hidraw*", ATTRS{idVendor}=="054c", MODE="0660", TAG+="uaccess"` in
  `/etc/udev/rules.d/99-sony-hidraw.rules`, followed by
  `sudo udevadm control --reload-rules && sudo udevadm trigger`.

- ~~The Makefile has no header dependency tracking.~~ Fixed 2026-08-06: `-MMD -MP` plus `-include` of
  the generated `.d` files, so editing anything in `include/` rebuilds exactly what included it. The
  `make clean && make` ritual is no longer needed. `make` is also parallel by default (`-j$(nproc)`);
  an explicit `-j` on the command line still wins.
- ~~A bare `make` builds nothing.~~ Fixed 2026-08-19: `compile_commands` is defined before `all` and
  make takes the first real target as the default goal, so `make` regenerated `compile_commands.json`
  and left the binary at whatever an earlier explicit `make all` produced. Every edit then looked
  like it had no effect, which invalidated most of a session — including three "measurements" taken
  against a day-old binary. `.DEFAULT_GOAL := all` fixes it. If a change ever seems to do nothing,
  check the binary's mtime before checking the change.
- ~~`make test` is broken~~ Fixed 2026-10-02: `make test` runs nine unit tests (tests/*_test.c)
  and `make hwtest` runs bare-metal PS-X EXEs inside the emulator on both renderers (tests/hw/),
  layers 1 and 2 of the testing plan. Before that **there was no automated test of any kind in this
  repository**, and there is still none at the level of a game (layer 3): every accuracy claim about
  a title was established by running it and reading a log. That is the single largest structural gap, and `docs/TESTING_PLAN_2026-08-20.md`
  is the plan for closing it, written the day after the LWL/LWR bug showed what it costs: months
  live, most of a day to find, and sixteen assertions to have caught.
- Never quote a speed figure measured with `ZS1_LOG_STDERR`, per-vblank Lua probes, or breakpoints
  active. The instrumentation costs more than what it measures; this produced a bogus "85-95% of real
  time" that was later withdrawn. It also applies to *diagnosing* slowness, not just quoting it: a
  reported "-30% drift with underruns" on 2026-08-17 could not be attributed at all, because every
  run in evidence had `ZS1_LOG_LEVEL=debug` writing ~1.4M lines per 100 s. And note that a guest
  stuck in a retry loop burns emulated cycles while the host idles, which looks identical on screen
  to a host that cannot keep up, but needs the opposite fix. One `ZS1_FRAME_PROFILE=1` run with no
  logging and no probe, first.
- The BIOS must match the disc's region. A PAL disc with the US BIOS is rejected exactly as on
  hardware, and the symptom — sitting at the BIOS menu — looks like a boot regression.
- `--game=` needs the `.bin`, not the `.cue`, for these discs. A `.cue` is accepted and then reports
  "Disc load failed — BIOS-only mode", which looks like a disc bug rather than a path mistake.
- Never quote a frame figure from a single run. A "17.5ms per frame" measurement taken while a build
  was running was reproduced at ~7ms minutes later on the identical binary, and nearly led to blaming
  a 2.7x regression on gcc-14. Repeat, and take a median.
- **A guest that runs too fast hides broken peripheral timing, and fixing the speed "breaks" the
  machine.** BIOS ROM instruction fetches went uncharged for months because charging them hung boot;
  the hang was two CDROM defects (a deferred response re-armed at a 30 µs minimum, and a re-issued
  `Init` restarting work already owed) that only a slow BIOS could expose. The lesson is the order of
  suspicion: when adding a *correct* cost breaks something, the cost is usually right and the thing
  it slowed down is wrong. ROM *data* loads are still uncharged, and the old claim that charging them
  killed controller input did not reproduce (the pad kept polling 32 times a field) — what it does do
  is overshoot, see `bus_charge_cpu_load()`.
- **The measurement that settles emulated-timing arguments** is boot milestones in *emulated fields*,
  not wall clock: our TTY lines carry the log's `[f… t…]` stamp, and a DuckStation run gets the same
  axis by counting its `Now in v-blank` lines. Ace Combat 2 lands within ~2% end to end this way.
  The AppImage is useless for this — a release build compiles `DEBUG_LOG`/`TRACE_LOG` out, so its
  DMA/GPU/SPU channels are empty at any log level. Build the submodule with
  `-DCMAKE_BUILD_TYPE=Devel` (that is what defines `_DEVEL`) and it emits ~2.3M lines a minute.
- Anything on the load/store path is the hottest code in the emulator. A chain of region tests added
  to `interconnect_load32` cost ~10% of host frame time by itself; it is now one comparison.
- `include/timers.h` is CRLF, like `include/renderer.h`. Edit both by line, never by rewriting the
  whole file, or a two-line change becomes a whole-file diff.
- `include/renderer.h` is CRLF. Editing it with a script that rewrites the whole file converts it to
  LF and produces a 750-line diff.
