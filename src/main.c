/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* setenv() is POSIX, and -std=c99 hides it behind __STRICT_ANSI__. Requesting
 * POSIX.1-2001 declares it without pulling in the wider GNU namespace. Must
 * precede every include so the first libc header sees it. */
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <SDL3/SDL.h>
#define GLEW_STATIC
#include <GL/glew.h>

#include "cpu.h"
#include "interconnect.h"
#include "bios.h"
#include "ram.h"
#include "renderer.h"
#include "cdrom.h"
#include "cdrom_audio.h"
#include "log.h"
#include "debug_ui.h"
#include "event_scheduler.h"
#include "controller.h"
#include "debugger.h"
#include "spu.h"
#include "lua_debug.h"
#include "system.h"
#include "frame_events.h"
#include "savestate.h"
#include "host_info.h"

/* From debug_ui.cpp — returns ImDrawData* after ImGui::Render() */
extern void* debug_ui_get_draw_data(void);
/* From debug_ui.cpp: empties the per-category log files' stdio buffers. They
 * are fully buffered, and this is the once-per-field point where they reach
 * the disk (the crash handlers below cover the rest). */
extern void debug_ui_flush_logs(void);

/* Emulated-frame length and host frame pacing both follow the GPU's video mode
 * (gpu_cycles_per_frame): 566203 cy / 59.82 Hz NTSC, 680823 cy / 49.75 Hz PAL.
 * Both were previously pinned to NTSC 60 Hz, which ran PAL discs ~20% fast.
 * PSX_SYSCLK_HZ comes from cdrom_disc.h. */

// --- Argument config ---
typedef struct {
    const char* bios_path;
    const char* game_path;
    const char* exe_path;
} EmuArgs;

static bool parse_args(int argc, char** argv, EmuArgs* out) {
    out->bios_path = NULL;
    out->game_path = NULL;
    out->exe_path  = NULL;

    for (int i = 1; i < argc; ++i) {
        if      (strncmp(argv[i], "--game=", 7) == 0) out->game_path = argv[i] + 7;
        else if (strncmp(argv[i], "--exe=",  6) == 0) out->exe_path  = argv[i] + 6;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--game=<path>] [--exe=<path>] <BIOS_PATH>\n", argv[0]);
            return false;
        } else if (!out->bios_path) {
            out->bios_path = argv[i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    /* Generic fallback only. No BIOS image ships with this repository and none
     * may: the path is expected on the command line, and this exists so an
     * argument-less run fails at bios_load() with a path to point at rather
     * than a null dereference. */
    if (!out->bios_path) out->bios_path = "roms/bios.bin";
    return true;
}

// --- SDL / OpenGL window init ---
typedef struct { SDL_Window* win; SDL_GLContext ctx; } SdlCtx;

/* ZS1_GPU picks which GPU the GL context lands on, on a hybrid-graphics machine.
 *
 * Neither value is ours: they are the GLVND/PRIME offload variables the driver
 * stack reads when it resolves the GLX vendor, so this only saves typing them in
 * front of the command. They have to be set before the GL context is created —
 * hence before SDL_Init — because that is when the dispatch happens.
 *
 *   ZS1_GPU=nvidia   __NV_PRIME_RENDER_OFFLOAD=1, __GLX_VENDOR_LIBRARY_NAME=nvidia
 *   ZS1_GPU=intel    DRI_PRIME=0, the Mesa side of the same switch
 *
 * Neither is set by default, so the machine's own default provider wins. The
 * point of having it at all is being able to put the same build on both GPUs in
 * one session: driver behaviour is a real source of rendering differences here,
 * and the renderer's GL_RGB scanout format, its float-to-uint texture coordinate
 * conversion and GL_DITHER all rendered differently on the two before they were
 * fixed. An override that already exists is worth more than one you have to
 * remember when a bug shows up.
 *
 * setenv() with overwrite=0, so an explicit variable on the command line still
 * beats this. */
static void apply_gpu_preference(void) {
    const char* pref = getenv("ZS1_GPU");
    if (!pref) return;

    if (strcmp(pref, "nvidia") == 0) {
        setenv("__NV_PRIME_RENDER_OFFLOAD", "1",      0);
        setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
        LOG_SYSTEM_INFO("[SYSTEM] ZS1_GPU=nvidia — requesting the discrete GPU via PRIME offload");
    } else if (strcmp(pref, "intel") == 0) {
        setenv("DRI_PRIME", "0", 0);
        LOG_SYSTEM_INFO("[SYSTEM] ZS1_GPU=intel — requesting the integrated GPU");
    } else {
        LOG_SYSTEM_WARN("[SYSTEM] ZS1_GPU=\"%s\" not recognised (want \"nvidia\" or \"intel\") — "
                        "using the system default", pref);
        return;
    }
    /* Whether the request was honoured is only knowable from the GL strings, which
     * are logged once the context is up. Compare them, do not assume. */
}

/* Which rendering backend to bring up.
 *
 * An environment variable rather than a setting, for now, because the picker in
 * the interface is Phase 5 work and the window flag has to be decided before
 * the window exists — SDL fixes SDL_WINDOW_OPENGL and SDL_WINDOW_VULKAN at
 * creation and neither can be added later. That is also why a hot switch has to
 * recreate the window rather than just the context. */
static GfxBackendType pick_backend(void) {
    const char* want = getenv("ZS1_GFX");
    if (!want) return GFX_BACKEND_GL33;
    if (!strcmp(want, "vulkan") || !strcmp(want, "vk")) {
        const char* why = gfx_backend_unavailable_reason(GFX_BACKEND_VULKAN);
        if (why) {
            LOG_SYSTEM_WARN("[SYSTEM] ZS1_GFX=vulkan requested but unavailable: %s", why);
            return GFX_BACKEND_GL33;
        }
        return GFX_BACKEND_VULKAN;
    }
    if (strcmp(want, "gl") && strcmp(want, "opengl"))
        LOG_SYSTEM_WARN("[SYSTEM] ZS1_GFX=\"%s\" not recognised (want \"gl\" or \"vulkan\")", want);
    return GFX_BACKEND_GL33;
}

/* Window plus context, and nothing that belongs to the process.
 *
 * Split out of init_sdl() because a backend switch has to run it again: the
 * SDL_WINDOW_OPENGL and SDL_WINDOW_VULKAN flags are fixed when the window is
 * created and cannot be added or removed afterwards, so going from one backend
 * to the other means a new window. SDL_Init() and SDL_Quit() are bracketed
 * around the whole session and deliberately stay out of here — re-initialising
 * the subsystems would take the audio device and the open gamepads with them. */
static bool create_gfx_window(SdlCtx* out, GfxBackendType backend) {
    out->win = NULL;
    out->ctx = NULL;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    /* Resizable so the window manager offers a maximise button; maximised
     * after the GL context is up (see below). Alt+Enter toggles fullscreen. */
    /* SDL3 dropped the position arguments from SDL_CreateWindow; a new window
     * lands wherever the backend puts it, so centre it explicitly to keep the
     * old placement. (Moot once it is maximised below, but only on a desktop
     * where maximising works.) */
    const SDL_WindowFlags gfx_flag = (backend == GFX_BACKEND_VULKAN)
                                   ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;
    out->win = SDL_CreateWindow("ZoniStation One", 1280, 720,
        gfx_flag | SDL_WINDOW_RESIZABLE);
    if (out->win)
        SDL_SetWindowPosition(out->win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!out->win) {
        LOG_SYSTEM_ERROR("[SYSTEM] SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    /* Vulkan's device and surface belong to the backend, and it creates them
     * from this window in renderer_init(). Everything below here is the GL
     * bring-up and is skipped. */
    if (backend == GFX_BACKEND_VULKAN) {
        out->ctx = NULL;
        LOG_SYSTEM_INFO("[SYSTEM] Vulkan window created; the device comes up with the renderer");
        return true;
    }

    out->ctx = SDL_GL_CreateContext(out->win);
    if (!out->ctx) {
        LOG_SYSTEM_ERROR("[SYSTEM] SDL_GL_CreateContext: %s", SDL_GetError());
        SDL_DestroyWindow(out->win);
        out->win = NULL;
        return false;
    }
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        LOG_SYSTEM_ERROR("[SYSTEM] GLEW init: %s", glewGetErrorString(err));
        SDL_GL_DestroyContext(out->ctx);
        SDL_DestroyWindow(out->win);
        out->ctx = NULL;
        out->win = NULL;
        return false;
    }
    /* Which GPU actually got the context matters on hybrid machines — the same
     * binary lands on the iGPU or the discrete card depending on the PRIME
     * environment, and "is this a driver bug" is unanswerable without it. */
    const char* gl_version  = (const char*)glGetString(GL_VERSION);
    const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
    const char* gl_vendor   = (const char*)glGetString(GL_VENDOR);
    LOG_SYSTEM_INFO("[SYSTEM] OpenGL %s | %s | %s", gl_version, gl_renderer, gl_vendor);

    /* Report which driver the context actually came from, and say plainly whether
     * a ZS1_GPU request was honoured. Asking for the discrete card and silently
     * getting the integrated one is a normal outcome — the offload can fail
     * because the kernel module is not loaded, or the compositor holds the
     * display — and it is exactly the case where an unexplained rendering
     * difference gets blamed on the emulator. So it is checked here rather than
     * left for a human to spot in the vendor string. */
    {
        bool on_nvidia = gl_vendor && strstr(gl_vendor, "NVIDIA") != NULL;
        bool on_intel  = gl_vendor && strstr(gl_vendor, "Intel")  != NULL;
        const char* driver = on_nvidia ? "NVIDIA proprietary"
                           : on_intel  ? "Mesa (Intel integrated)"
                           : "unknown";
        LOG_SYSTEM_INFO("[SYSTEM] GL driver in use: %s", driver);

        const char* want = getenv("ZS1_GPU");
        if (want && strcmp(want, "nvidia") == 0 && !on_nvidia)
            LOG_SYSTEM_WARN("[SYSTEM] ZS1_GPU=nvidia was requested but the context is on \"%s\" — "
                            "PRIME offload did not take. Check that the kernel module is loaded "
                            "(/proc/driver/nvidia/version) and that libGLX_nvidia is installed",
                            gl_vendor ? gl_vendor : "?");
        else if (want && strcmp(want, "intel") == 0 && !on_intel)
            LOG_SYSTEM_WARN("[SYSTEM] ZS1_GPU=intel was requested but the context is on \"%s\"",
                            gl_vendor ? gl_vendor : "?");
        else if (want)
            LOG_SYSTEM_INFO("[SYSTEM] ZS1_GPU=%s honoured", want);

        /* The Host HW panel reads this rather than carrying a typed-in GPU
         * name: on a hybrid machine the honest answer changes per run. */
        bool honoured = !want
                     || (strcmp(want, "nvidia") == 0 && on_nvidia)
                     || (strcmp(want, "intel")  == 0 && on_intel);
        host_info_set_gl(gl_vendor, gl_renderer, gl_version, driver, want, honoured);
    }

    check_gl_error("GLEW init");
    return true;
}

/* The window and its context, without ending the SDL session. */
static void destroy_gfx_window(SdlCtx* s) {
    if (s->ctx) { SDL_GL_DestroyContext(s->ctx); s->ctx = NULL; }
    if (s->win) { SDL_DestroyWindow(s->win);     s->win = NULL; }
}

static bool init_sdl(SdlCtx* out, GfxBackendType backend) {
    apply_gpu_preference();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        LOG_SYSTEM_ERROR("[SYSTEM] SDL_Init: %s", SDL_GetError());
        return false;
    }
    if (!create_gfx_window(out, backend)) {
        SDL_Quit();
        return false;
    }
    return true;
}

static void shutdown_sdl(SdlCtx* s) {
    destroy_gfx_window(s);
    SDL_Quit();
}

/* Swap the rendering backend, or the GPU under it, without stopping the machine.
 *
 * The hard constraint is SDL's: SDL_WINDOW_OPENGL and SDL_WINDOW_VULKAN are
 * fixed when the window is created and neither can be added later, so going
 * from one API to the other means a new window — not just a new context. That
 * is what makes this a sequence rather than a call.
 *
 * Three things have to survive it:
 *
 *   VRAM. It lives on the GPU, and gpu.vram.data is not a copy of it — the
 *   CPU-side store holds what the CPU wrote, never what the rasteriser drew.
 *   So it is read back into host memory before the device goes and pushed into
 *   the new one after. One megabyte, once per switch.
 *
 *   The drawing state. The ten renderer_set_* values are backend state; a fresh
 *   backend starts at its own defaults and the guest has no reason to re-send
 *   GP0(E2..E6). gpu_reapply_renderer_state() puts them back from Gpu, which is
 *   the authority for all of them.
 *
 *   The ImGui context — fonts, imgui.ini, the docking layout, the pinned
 *   watches. Only the two backend halves are recreated; the context is not
 *   touched, which is why the workspace comes back exactly as it was.
 *
 * The guest never notices: CPU, Interconnect, SPU and the drive are not
 * involved. No reset and no save state.
 *
 * Returns true when the requested backend is live. On failure the previous one
 * is rebuilt and false is returned with `sdl`, `backend_io` and the machine
 * left in a working state — a failed switch must not end the session. */
static bool switch_gfx_backend(SdlCtx* sdl, Interconnect* inter,
                               GfxBackendType* backend_io, int* device_io,
                               const GfxDeviceRequest* req) {
    /* One megabyte, static rather than on the stack: main()'s frame is already
     * carrying the machine. */
    static uint16_t vram_carry[VRAM_WIDTH * VRAM_HEIGHT];

    const GfxBackendType from = *backend_io;
    const GfxBackendType to   = req->type;

    LOG_SYSTEM_INFO("[SYSTEM] Renderer switch: %s -> %s, device %d",
                    from == GFX_BACKEND_VULKAN ? "Vulkan" : "OpenGL",
                    to   == GFX_BACKEND_VULKAN ? "Vulkan" : "OpenGL",
                    req->device_index);

    /* 1. The GPU thread must be idle before anything it owns is read or torn
     *    down, and the readback below is a synchronous round-trip through it —
     *    so it happens here, while the thread is still alive. */
    renderer_wait_frame_done(&inter->gpu.renderer);
    bool have_vram = renderer_read_vram_rect(&inter->gpu.renderer, vram_carry,
                                             0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    if (!have_vram)
        LOG_SYSTEM_WARN("[SYSTEM] VRAM readback refused before the switch — "
                        "the picture will be rebuilt by the guest instead of carried over");

    /* 2. Stop the thread, then take the GL context back onto this one: every
     *    teardown call below is a GL call on the GL path. */
    renderer_stop_gpu_thread(&inter->gpu.renderer);
    if (from != GFX_BACKEND_VULKAN) SDL_GL_MakeCurrent(sdl->win, sdl->ctx);

    /* 3. Both ImGui backend halves, but not the context. The Vulkan renderer
     *    half belongs to the Vulkan backend and goes down inside
     *    renderer_destroy() — hence this order, which is the same one the
     *    shutdown path uses and for the same reason. */
    debug_ui_backend_shutdown();
    renderer_destroy(&inter->gpu.renderer);
    destroy_gfx_window(sdl);

    /* 4. Bring the requested backend up. Anything from here on that fails
     *    falls through to the rollback below. */
    bool ok = create_gfx_window(sdl, to)
           && renderer_select_backend(&inter->gpu.renderer, to)
           && renderer_init_ex(&inter->gpu.renderer, sdl->win, req);

    if (!ok) {
        LOG_SYSTEM_ERROR("[SYSTEM] %s failed to come up — rebuilding %s",
                         to   == GFX_BACKEND_VULKAN ? "Vulkan" : "OpenGL",
                         from == GFX_BACKEND_VULKAN ? "Vulkan" : "OpenGL");
        renderer_destroy(&inter->gpu.renderer);
        destroy_gfx_window(sdl);

        GfxDeviceRequest back = { from, *device_io, 1 };
        if (!create_gfx_window(sdl, from)
         || !renderer_select_backend(&inter->gpu.renderer, from)
         || !renderer_init_ex(&inter->gpu.renderer, sdl->win, &back)) {
            /* Both backends are gone. There is nothing left to render with and
             * nothing to fall back to, so say so plainly rather than carrying
             * on against a dead device. */
            LOG_SYSTEM_ERROR("[SYSTEM] The previous renderer could not be rebuilt either");
            return false;
        }
    }

    const GfxBackendType live = ok ? to : from;

    /* 5. Put the machine's picture and its drawing state into the new backend,
     *    in that order: the state setters are cheap and the upload is what the
     *    next frame samples. */
    if (have_vram)
        renderer_upload_vram_rect(&inter->gpu.renderer, vram_carry,
                                  0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    gpu_reapply_renderer_state(&inter->gpu);

    /* 6. ImGui's two halves against the new window and device, then the thread.
     *    GL hands its context away again, exactly as at startup. */
    debug_ui_backend_init(sdl->win, sdl->ctx, (int)live);
    if (live != GFX_BACKEND_VULKAN) SDL_GL_MakeCurrent(sdl->win, NULL);
    renderer_start_gpu_thread(&inter->gpu.renderer, sdl->win, sdl->ctx);
    SDL_MaximizeWindow(sdl->win);

    *backend_io = live;
    *device_io  = ok ? req->device_index : *device_io;
    LOG_SYSTEM_INFO("[SYSTEM] Renderer switch %s — now on %s",
                    ok ? "done" : "rolled back",
                    live == GFX_BACKEND_VULKAN ? "Vulkan" : "OpenGL");
    return ok;
}

// --- SDL3 audio ---
static Spu* g_spu_for_audio = NULL;
static SDL_AudioStream* g_audio_stream = NULL;

// --- Exec trace on forced shutdown ---
static const Cpu* g_cpu_for_trace = NULL;

/* Ctrl+C, kill, abort(): dump the trace and leave.
 *
 * The per-category log files are fully buffered now (debug_ui.cpp), so the
 * lines since the last per-field flush are still in memory when a signal
 * arrives, and _exit(), unlike exit(), does not flush stdio. fflush(NULL)
 * writes every open stream out first. Neither fflush nor the fopen/fprintf in
 * cpu_dump_exec_trace is async-signal-safe; this handler already relied on
 * that working for the trace dump, and on the way out it is the right trade. */
static void sighandler_dump_trace(int sig) {
    (void)sig;
    if (g_cpu_for_trace) cpu_dump_exec_trace(g_cpu_for_trace, "logs/exec_trace.log");
    fflush(NULL);
    _exit(1);
}

/* A real crash: SIGSEGV, SIGBUS, SIGFPE, SIGILL.
 *
 * There was no handler for these, which was harmless while the log files were
 * line-buffered: every line was on disk the moment it was written. With full
 * buffering a segfault would lose the last field of log, which is precisely
 * the part that explains the segfault. So: dump the trace, flush, then put the
 * default action back and re-raise, so the process still dies of the signal it
 * got (exit status, core dump) rather than of a polite _exit(1).
 *
 * The default action goes back first thing, and the signal stays blocked while
 * the handler runs (no SA_NODEFER), so a second fault in here is fatal at once
 * instead of recursing; the raise() at the end is held until the handler
 * returns and then takes the default action. (SA_RESETHAND would say the same
 * thing but is XSI, hidden by this file's _POSIX_C_SOURCE.) */
static void sighandler_fatal(int sig) {
    signal(sig, SIG_DFL);
    if (g_cpu_for_trace) cpu_dump_exec_trace(g_cpu_for_trace, "logs/exec_trace.log");
    fflush(NULL);
    raise(sig);
}

/* SDL3 has no fill-this-buffer device callback: audio goes through an
 * SDL_AudioStream, and the callback is told how many bytes the device still
 * wants rather than handed the buffer. The source is unchanged — the SPU's own
 * ring, drained by spu_fill_audio — and so is the contract the main loop's
 * pacing depends on: the device pulls, the emulator produces, and
 * spu_ring_used() is still the clock. Only the delivery mechanism moved.
 *
 * The stream converts to whatever the device natively runs, which is what
 * SDL2's SDL_OpenAudioDevice(..., allowed_changes=0) was doing internally. */
static void SDLCALL audio_callback(void* userdata, SDL_AudioStream* stream,
                                   int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;

    enum { CHUNK_FRAMES = 1024 };
    const int frame_bytes = (int)(2 * sizeof(int16_t));   /* stereo int16 */
    int16_t buf[CHUNK_FRAMES * 2];

    while (additional_amount >= frame_bytes) {
        int frames = additional_amount / frame_bytes;
        if (frames > CHUNK_FRAMES) frames = CHUNK_FRAMES;

        if (g_spu_for_audio)
            spu_fill_audio(g_spu_for_audio, buf, frames);
        else
            memset(buf, 0, (size_t)frames * (size_t)frame_bytes);

        SDL_PutAudioStreamData(stream, buf, frames * frame_bytes);
        additional_amount -= frames * frame_bytes;
    }
}

static void audio_init(Spu* spu) {
    g_spu_for_audio = spu;

    /* SDL3 dropped the per-open buffer size; the device quantum comes from this
     * hint instead. 512 frames is what the SDL2 path asked for, and what
     * SPU_RING_TARGET_SAMPLES was tuned against — changing it moves the latency
     * the pacing loop settles at. */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");

    SDL_AudioSpec want = { .format = SDL_AUDIO_S16, .channels = 2, .freq = 44100 };
    g_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                               &want, audio_callback, NULL);
    if (!g_audio_stream) {
        LOG_SYSTEM_WARN("[SYSTEM] SDL audio open failed: %s — no sound", SDL_GetError());
        return;
    }

    /* Report the device's own format, not ours: the stream resamples silently,
     * and a device running at 48 kHz is worth seeing in the log when the audio
     * sounds wrong. */
    SDL_AudioSpec got = want;
    int dev_frames = 0;
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(g_audio_stream);
    if (dev) SDL_GetAudioDeviceFormat(dev, &got, &dev_frames);
    LOG_SYSTEM_INFO("[SYSTEM] Audio: %d Hz ch=%d fmt=0x%x buf=%d",
                    got.freq, got.channels, (unsigned)got.format, dev_frames);
    host_info_set_audio(SDL_GetCurrentAudioDriver(), got.freq, got.channels, dev_frames);

    SDL_ResumeAudioStreamDevice(g_audio_stream);
}

static void audio_shutdown(void) {
    if (g_audio_stream) {
        /* Destroying a stream opened with SDL_OpenAudioDeviceStream closes the
         * device it bound. */
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
    }
}

/* A savestate load rewrites structs that two other threads are reading: the GPU
 * thread replays the previous frame out of VRAM, and the SDL audio callback
 * reads the SPU's sample ring. Both are quiesced around the restore — the ring
 * indices in particular are restored as a pair, and a callback landing between
 * them would read a window that never existed. */
static bool load_state_guarded(const char* path, Cpu* cpu, Interconnect* inter) {
    renderer_wait_frame_done(&inter->gpu.renderer);
    if (g_audio_stream) SDL_LockAudioStream(g_audio_stream);
    bool ok = savestate_load(path, cpu, inter);
    if (g_audio_stream) SDL_UnlockAudioStream(g_audio_stream);
    return ok;
}

// --- PS-X EXE loader ---
static bool load_exe(const char* path, Cpu* cpu, Interconnect* inter) {
    // Warm-up: let BIOS init kernel before sideloading
    const uint32_t shell_pc = 0x80030000;
    uint64_t warmup = 0;
    while (cpu->pc != shell_pc && warmup < 20000000ULL) {
        cpu_run_next_instruction(cpu);
        warmup++;
    }
    if (cpu->pc == shell_pc)
        LOG_SYSTEM_INFO("[SYSTEM] BootEXE warmup done after %llu insns", (unsigned long long)warmup);
    else
        LOG_SYSTEM_WARN("[SYSTEM] BootEXE warmup timed out at PC=0x%08x", cpu->pc);

    FILE* f = fopen(path, "rb");
    if (!f) { LOG_SYSTEM_ERROR("[SYSTEM] Cannot open EXE: %s", path); return false; }

    PSEXEHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) < sizeof(hdr)) {
        LOG_SYSTEM_ERROR("[SYSTEM] EXE too small"); fclose(f); return false;
    }
    if (memcmp(hdr.id, "PS-X EXE", 8) != 0) {
        LOG_SYSTEM_ERROR("[SYSTEM] Invalid PS-X EXE magic"); fclose(f); return false;
    }

    if (hdr.memfill_size > 0) {
        uint32_t off = hdr.memfill_start & 0x1FFFFF;
        uint32_t len = hdr.memfill_size & ~3u;
        if (off + len <= 0x200000)
            memset((uint8_t*)inter->ram->data + off, 0, len);
    }

    if (hdr.file_size > 0) {
        if (fseek(f, 0x800, SEEK_SET) != 0 ||
            hdr.load_address < 0x80000000 ||
            hdr.load_address + hdr.file_size > 0x80200000) {
            LOG_SYSTEM_ERROR("[SYSTEM] Invalid EXE load range"); fclose(f); return false;
        }
        uint32_t off = hdr.load_address & 0x1FFFFF;
        size_t got = fread((uint8_t*)inter->ram->data + off, 1, hdr.file_size, f);
        if (got != hdr.file_size) {
            LOG_SYSTEM_ERROR("[SYSTEM] EXE read short: %zu/%u", got, hdr.file_size);
            fclose(f); return false;
        }
        LOG_SYSTEM_INFO("[SYSTEM] EXE loaded: %u bytes @ 0x%08x", hdr.file_size, hdr.load_address);
    }
    fclose(f);

    uint32_t sp = hdr.initial_sp_base + hdr.initial_sp_offset;
    cpu->regs[28]     = hdr.initial_gp;
    cpu->regs[29]     = sp;
    cpu->regs[30]     = sp;
    cpu->pc           = hdr.initial_pc;
    cpu->next_pc      = hdr.initial_pc + 4;
    cpu->current_pc   = hdr.initial_pc;
    LOG_SYSTEM_INFO("[SYSTEM] EXE entry: 0x%08x", hdr.initial_pc);
    return true;
}

// --- TTY keyboard injection ---
static void inject_tty_keys(Interconnect* inter) {
    const bool* keys = SDL_GetKeyboardState(NULL);
    static bool prev[SDL_SCANCODE_COUNT];

    static const struct { SDL_Scancode sc; char ch; } map[] = {
        { SDL_SCANCODE_W,      'w'  },
        { SDL_SCANCODE_S,      's'  },
        { SDL_SCANCODE_D,      'd'  },
        { SDL_SCANCODE_A,      'a'  },
        { SDL_SCANCODE_SPACE,  '\n' },
        { SDL_SCANCODE_RETURN, '\n' },
        { SDL_SCANCODE_E,      'e'  },
        { SDL_SCANCODE_C,      'c'  },
        { SDL_SCANCODE_X,      'x'  },
        { SDL_SCANCODE_Z,      'z'  },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
        if (keys[map[i].sc] && !prev[map[i].sc])
            interconnect_tty_input_add(inter, map[i].ch);
    memcpy(prev, keys, sizeof(prev));
}

// --- Main ---
int main(int argc, char* argv[]) {
    EmuArgs args;
    if (!parse_args(argc, argv, &args)) return 1;

    log_init();
    if (getenv("ZS1_LOG_STDERR")) log_set_stderr_quiet(false);
    if (getenv("ZS1_LOG_TRACE"))  log_set_level(LOG_LEVEL_TRACE);
    /* ZS1_LOG_LEVEL=silent|error|warn|info|debug|trace — the level is a real
     * performance knob (DEBUG formats thousands of lines per FMV frame), so it
     * is settable per run instead of only at compile time. */
    {
        const char* lvl = getenv("ZS1_LOG_LEVEL");
        if (lvl) {
            if      (!strcmp(lvl, "silent")) log_set_level(LOG_LEVEL_SILENT);
            else if (!strcmp(lvl, "error"))  log_set_level(LOG_LEVEL_ERROR);
            else if (!strcmp(lvl, "warn"))   log_set_level(LOG_LEVEL_WARN);
            else if (!strcmp(lvl, "info"))   log_set_level(LOG_LEVEL_INFO);
            else if (!strcmp(lvl, "debug"))  log_set_level(LOG_LEVEL_DEBUG);
            else if (!strcmp(lvl, "trace"))  log_set_level(LOG_LEVEL_TRACE);
        }
    }
    /* The runtime level can only reach what the build compiled in: lines above
     * ZS1_LOG_MAX_LEVEL are gone from the binary (include/log.h). Say so rather
     * than leave a trace run wondering why it is silent. */
    if ((int)log_get_current_level() > (int)ZS1_LOG_MAX_LEVEL)
        LOG_SYSTEM_WARN("[SYSTEM] Log level %d requested but this build compiles out every line "
                        "above level %d; rebuild with `make LOG_MAX_LEVEL=TRACE` to get TRACE lines",
                        (int)log_get_current_level(), (int)ZS1_LOG_MAX_LEVEL);
    LOG_SYSTEM_INFO("[SYSTEM] ZoniStation One starting — BIOS: %s", args.bios_path);

    /* Not const: the quick menu's Video panel can change it while the machine
     * runs, through switch_gfx_backend(). */
    GfxBackendType backend = pick_backend();
    int gfx_device = -1;   /* -1 = whichever the backend chose for itself */

    SdlCtx sdl;
    if (!init_sdl(&sdl, backend)) return 1;

    debug_ui_init(sdl.win, sdl.ctx, (int)backend);

    /* The crash handlers go in as soon as the log files exist (debug_ui_init
     * opens them), so a fault in renderer or device bring-up still leaves its
     * last lines on disk. The trace dump inside them waits for g_cpu_for_trace. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sighandler_fatal;
        sa.sa_flags   = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);
        sigaction(SIGFPE,  &sa, NULL);
        sigaction(SIGILL,  &sa, NULL);
    }

    // --- Component init ---
    // static: these structs are multi-MB (Bios 512KB, Ram 2MB, Interconnect ~3.4MB) —
    // too large for the 8MB default thread stack alongside the rest of main()'s frame
    // and its callees; a single process-lifetime instance of each, so static (BSS) is
    // the natural home, not the stack (and not heap, per this project's no-malloc convention).
    static Bios       bios;
    static Ram        ram;
    static Interconnect inter;
    static Cpu        cpu;
    Controller gamepad;

    ram_init(&ram);

    if (!bios_load(&bios, args.bios_path)) { shutdown_sdl(&sdl); return 1; }

    /* Bind the rendering backend before the machine is built. gpu_reset_state()
     * runs inside interconnect_init() and pushes the GP0/GP1 reset values
     * through four renderer setters (gpu.c:661-668) — that is before
     * renderer_init() below, and those values are not redundant, so the
     * renderer has to know which backend it is talking to by now. */
    if (!renderer_select_backend(&inter.gpu.renderer, backend)) {
        LOG_SYSTEM_ERROR("[SYSTEM] No rendering backend available");
        shutdown_sdl(&sdl);
        return 1;
    }

    /* The renderer comes up before the machine now, not after.
     *
     * Vulkan needs it: its device and its ImGui half are created here, and the
     * surface comes from the window. GL is happy either way. The old order also
     * meant the four setters gpu_reset_state() calls (gpu.c:661-668) landed on
     * a renderer that did not exist yet; running init first means they land on
     * the live backend instead of on its pre-init state. */
    if (!renderer_init(&inter.gpu.renderer, sdl.win)) {
        LOG_SYSTEM_ERROR("[SYSTEM] Renderer init failed");
        shutdown_sdl(&sdl);
        return 1;
    }

    interconnect_init(&inter, &bios, &ram);

    /* GL only: hand the context to the GPU thread. Vulkan has no context to
     * release — the backend owns its device outright. */
    if (backend != GFX_BACKEND_VULKAN) SDL_GL_MakeCurrent(sdl.win, NULL);
    renderer_start_gpu_thread(&inter.gpu.renderer, sdl.win, sdl.ctx);

    /* Fill the screen while keeping the titlebar's minimise/maximise/close
     * buttons. Window-manager call only — no GL. */
    SDL_MaximizeWindow(sdl.win);

    if (args.game_path) {
        if (!cdrom_load_disc(&inter.cdrom, args.game_path))
            LOG_SYSTEM_WARN("[SYSTEM] Disc load failed: %s — BIOS-only mode", args.game_path);
        else
            LOG_SYSTEM_INFO("[SYSTEM] Disc loaded: %s", args.game_path);
    } else {
        inter.cdrom.disc_present  = false;
        inter.cdrom.drive_state   = DRIVE_IDLE;
        LOG_SYSTEM_INFO("[SYSTEM] No disc — BIOS-only mode");
    }

    cpu_init(&cpu, &inter);
    interconnect_set_cpu(&inter, &cpu);
    frame_events_bind_clock(&inter.cpu_cycle_counter);
    lua_debug_init(&inter, &cpu);
    if (getenv("ZS1_LUA_SCRIPT")) lua_debug_run_file(getenv("ZS1_LUA_SCRIPT"));

    if (args.exe_path && !load_exe(args.exe_path, &cpu, &inter)) {
        shutdown_sdl(&sdl);
        return 1;
    }

    controller_init(&gamepad);
    sio_set_controller_connected(&inter.sio, true);

    audio_init(&inter.spu);

    system_init(&inter, &cpu);

    g_cpu_for_trace = &cpu;
    signal(SIGINT,  sighandler_dump_trace);
    signal(SIGTERM, sighandler_dump_trace);
    signal(SIGABRT, sighandler_dump_trace);

    LOG_SYSTEM_INFO("[SYSTEM] All components initialised — starting loop");

    /* Machine-bar identity for the debug UI: the two paths' basenames. */
    {
        const char* bn = args.bios_path;
        const char* dn = args.game_path ? args.game_path : "n/a";
        for (const char* p = args.bios_path; *p; p++)
            if (*p == '/' || *p == '\\') bn = p + 1;
        if (args.game_path)
            for (const char* p = args.game_path; *p; p++)
                if (*p == '/' || *p == '\\') dn = p + 1;
        debug_ui_set_machine_info(bn, dn);
    }

    // --- Main loop ---
    bool quit = false;
    SDL_Event ev;

    /* Live vitals for the machine bar. Cheap: two perf-counter reads and one
     * SPU sample delta per frame, no logging — the instrumentation that once
     * produced a bogus speed figure was the logging, not the counters. */
    uint64_t vit_prev         = SDL_GetPerformanceCounter();
    uint32_t vit_prev_samples = inter.spu.total_samples_generated;
    double   vit_drift_ema    = 0.0;

    /* Re-derived each frame: a game may switch video mode via GP1(08) at any
     * point (and the BIOS boots NTSC before a PAL disc's shell switches it). */
    uint32_t cycles_per_frame = gpu_cycles_per_frame(&inter.gpu);
    uint64_t frame_ticks = (uint64_t)((double)SDL_GetPerformanceFrequency() *
                                      (double)cycles_per_frame / (double)PSX_SYSCLK_HZ);
    uint64_t next_frame = SDL_GetPerformanceCounter() + frame_ticks;

    /* ZS1_FRAME_PROFILE=1: per-field wall-clock buckets, see the [PROF] line. */
    const int s_prof = getenv("ZS1_FRAME_PROFILE") ? 1 : 0;

    while (!quit) {
        while (SDL_PollEvent(&ev)) {
            controller_process_event(&gamepad, &ev);
            debug_ui_process_event(&ev);
            if (ev.type == SDL_EVENT_QUIT) {
                /* Say who ended the session: a window closed by the desktop and
                 * a guest that stopped drawing look identical from outside. */
                LOG_SYSTEM_INFO("[SYSTEM] Quit: SDL_EVENT_QUIT (window closed)");
                quit = true;
            }
            /* Escape belongs to the gameplay shell when that shell is up — it
             * opens and closes the quick menu there. It still quits from the
             * debug workspace, where there is no menu to open. */
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) {
                if (!debug_ui_escape_pressed()) {
                    LOG_SYSTEM_INFO("[SYSTEM] Quit: Escape in the debug workspace");
                    quit = true;
                }
            }
            /* Alt+Enter toggles borderless desktop fullscreen. SDL3 folded
             * FULLSCREEN_DESKTOP into a bool: a window with no display mode set
             * — which is ours — goes fullscreen-desktop. */
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_RETURN &&
                     (ev.key.mod & SDL_KMOD_ALT)) {
                bool fs = (SDL_GetWindowFlags(sdl.win) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(sdl.win, !fs);
            }
            /* F5 saves, F8 loads. Handled here rather than in the debug UI so
             * they work with every panel closed, which is when the emulator is
             * actually fast enough to reach the state worth capturing. */
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F5 && !ev.key.repeat) {
                bool ok = savestate_save(SAVESTATE_DEFAULT_PATH, &cpu, &inter);
                debug_ui_notify_state_result(true, ok, SAVESTATE_DEFAULT_PATH);
            }
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F8 && !ev.key.repeat) {
                load_state_guarded(SAVESTATE_DEFAULT_PATH, &cpu, &inter);
                debug_ui_notify_state_result(false, true, SAVESTATE_DEFAULT_PATH);
            }
            /* F12 is the Analog button for players without a touchpad pad.
             * F1..F9 pick the debug UI mode, F10/F11 pause and step; F12 is the
             * only function key left unclaimed. */
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F12 && !ev.key.repeat) {
                sio_cycle_pad_mode(&inter.sio);
            }
        }

        /* The gameplay shell's quick menu asks for a save or a load the same
         * way a script does: it parks the request and the machine's owner
         * carries it out here, outside the event dispatch. */
        {
            bool want_save = false;
            char state_path[192];
            if (debug_ui_take_state_request(&want_save, state_path, sizeof(state_path))) {
                bool ok;
                if (want_save) {
                    ok = savestate_save(state_path, &cpu, &inter);
                } else {
                    load_state_guarded(state_path, &cpu, &inter);
                    ok = true;
                }
                debug_ui_notify_state_result(want_save, ok, state_path);
            }
            if (debug_ui_take_quit_request()) {
                LOG_SYSTEM_INFO("[SYSTEM] Quit: requested from the quick menu");
                quit = true;
            }

            /* ZS1_GFX_SWITCH_TEST=<n>: flip the backend every n fields, for as
             * long as the run lasts. The switch is a sequence of a dozen steps
             * across three subsystems and a human at a menu cannot exercise it
             * often enough to see a leak — twenty switches with RSS watched is
             * the check that renderer_destroy() releases everything it should.
             * Off unless the variable is set. */
            {
                static int test_every = -1;
                static uint64_t field = 0;
                if (test_every < 0) {
                    const char* e = getenv("ZS1_GFX_SWITCH_TEST");
                    test_every = e ? atoi(e) : 0;
                    if (test_every > 0)
                        LOG_SYSTEM_INFO("[SYSTEM] Renderer switch stress: every %d fields", test_every);
                }
                if (test_every > 0 && ++field % (uint64_t)test_every == 0
                    && !gfx_backend_unavailable_reason(GFX_BACKEND_VULKAN)) {
                    GfxDeviceRequest r = { backend == GFX_BACKEND_VULKAN
                                           ? GFX_BACKEND_GL33 : GFX_BACKEND_VULKAN, -1, 1 };
                    bool ok = switch_gfx_backend(&sdl, &inter, &backend, &gfx_device, &r);
                    debug_ui_notify_gfx_result((int)backend, gfx_device, ok, NULL);
                    if (!ok && !inter.gpu.renderer.impl) quit = true;
                }
            }

            /* The Video panel asks for a backend or a GPU; the window and the
             * device belong here, so the switch happens here — between frames,
             * with no draw in flight and nothing reading VRAM. */
            GfxDeviceRequest gfx_req;
            if (debug_ui_take_gfx_request(&gfx_req)) {
                bool ok = switch_gfx_backend(&sdl, &inter, &backend, &gfx_device, &gfx_req);
                const HostInfo* hi = host_info_get();
                debug_ui_notify_gfx_result((int)backend, gfx_device, ok,
                                           hi->gl_renderer[0] ? hi->gl_renderer : NULL);
                if (!ok && !inter.gpu.renderer.impl) {
                    LOG_SYSTEM_ERROR("[SYSTEM] No renderer left after the switch — stopping");
                    quit = true;
                }
            }
        }

        /* A script's emu.load_state() parks its request rather than restoring
         * from inside the event dispatch; this is where it lands. */
        {
            char pending[512];
            if (lua_debug_take_pending_state_save(pending, sizeof(pending)))
                savestate_save(pending, &cpu, &inter);
            if (lua_debug_take_pending_state_load(pending, sizeof(pending)))
                load_state_guarded(pending, &cpu, &inter);
        }

        /* The pad's Analog button: DS4 touchpad click, or F12 on the keyboard.
         * Cycles digital -> analog -> stick. Taken before the poll so the mode it
         * selects is the one this frame's read is folded against. */
        if (controller_take_analog_toggle(&gamepad))
            sio_cycle_pad_mode(&inter.sio);
        SioPadMode pad_mode = sio_get_pad_mode(&inter.sio);
        gamepad.analog_active = (pad_mode != SIO_PAD_DIGITAL);

        /* Show the emulated pad's mode on the real pad's light bar. The colours
         * are the hardware's own (DOCS/controllersandmemorycards.md:369-372):
         * 5A41h digital LED=Off, 5A73h analog LED=Red, 5A53h stick LED=Green.
         * Which mode the game actually selected was previously only visible in
         * the log. */
        switch (pad_mode) {
            case SIO_PAD_ANALOG: controller_set_led(&gamepad, 0xFF, 0x00, 0x00); break;
            case SIO_PAD_STICK:  controller_set_led(&gamepad, 0x00, 0xFF, 0x00); break;
            default:             controller_set_led(&gamepad, 0x00, 0x00, 0x00); break;
        }

        sio_set_button_state(&inter.sio, controller_update(&gamepad));
        // Feed the sticks (raw -32768..32767) into the SIO analog bytes.
        sio_set_analog_state(&inter.sio, gamepad.left_x, gamepad.left_y,
                             gamepad.right_x, gamepad.right_y);
        // Rumble: M1/M2 levels captured from 42h reads → DS4 motors.
        uint8_t rumble_m1, rumble_m2;
        sio_get_rumble(&inter.sio, &rumble_m1, &rumble_m2);
        controller_update_rumble(&gamepad, rumble_m1, rumble_m2);
        inject_tty_keys(&inter);

        /* Host-side frame pacing budget follows the GPU's video mode. */
        cycles_per_frame = gpu_cycles_per_frame(&inter.gpu);
        frame_ticks = (uint64_t)((double)SDL_GetPerformanceFrequency() *
                                 (double)cycles_per_frame / (double)PSX_SYSCLK_HZ);

        uint64_t t0 = 0, t1 = 0, t_w0 = 0, t_w1 = 0, t_ui = 0, t2 = 0, t3 = 0, t4 = 0, t_p0 = 0;
        if (s_prof) t0 = SDL_GetPerformanceCounter();

        /* Run the machine for one video frame (or one debugger step).
         *
         * This used to come after renderer_wait_frame_done(), so every field
         * began by waiting for the GPU thread to finish the previous one,
         * buffer swap included; when the swap blocks on vblank (a 60 Hz panel
         * under 50 Hz content, a FIFO swapchain) that wait was added to the
         * field instead of overlapping it. Emulation does not need the GPU
         * thread to be idle:
         *   - it only records into the renderer's write slot; the GPU thread
         *     replays the other slot, and the slots trade places only in
         *     renderer_submit_frame(), under the mutex, after frames_pending
         *     has gone back to zero;
         *   - a synchronous VRAM readback (GP0(80h)/GP0(C0h)) waits for the
         *     previous frame by itself before replaying the write slot, on
         *     both backends;
         *   - savestate loads and backend switches run at the top of the loop
         *     and wait on their own (load_state_guarded, switch_gfx_backend).
         * What the wait protects is ImGui: the GPU thread is drawing the
         * previous frame's draw data, and ImGui::NewFrame() reuses it. So the
         * wait now sits after emulation and before debug_ui_render(). */
        system_run_frame(&inter, &cpu);
        if (s_prof) t1 = SDL_GetPerformanceCounter();

        /* Update machine-bar vitals from the counters we already hold. Wall
         * time is the full loop iteration (work + pacing), so Speed reads 100%
         * when the emulator keeps real time and drops below it when it cannot. */
        {
            uint64_t vit_now  = SDL_GetPerformanceCounter();
            double   vit_freq = (double)SDL_GetPerformanceFrequency();
            double   frame_ms = (double)(vit_now - vit_prev) * 1000.0 / vit_freq;
            double   budget_ms = (double)cycles_per_frame * 1000.0 / (double)PSX_SYSCLK_HZ;
            uint32_t cur_samples = inter.spu.total_samples_generated;
            uint32_t produced = cur_samples - vit_prev_samples;
            double   drift = 0.0;
            double   wall_s = frame_ms / 1000.0;
            if (wall_s > 0.0005) {
                double expected = 44100.0 * wall_s;   /* device consumption rate */
                if (expected > 1.0) drift = ((double)produced / expected - 1.0) * 100.0;
            }
            vit_drift_ema = vit_drift_ema * 0.9 + drift * 0.1;
            debug_ui_set_vitals(frame_ms, budget_ms, spu_ring_used(&inter.spu),
                                SPU_RING_TARGET_SAMPLES, vit_drift_ema);
            vit_prev = vit_now;
            vit_prev_samples = cur_samples;
        }

        /* The GPU thread must be done with the previous frame's ImGui draw data
         * (and the ImGui textures it updates) before NewFrame() touches them. */
        if (s_prof) t_w0 = SDL_GetPerformanceCounter();
        renderer_wait_frame_done(&inter.gpu.renderer);
        if (s_prof) t_w1 = SDL_GetPerformanceCounter();

        /* Build ImGui for this frame (SDL + widget code, no GL) */
        debug_ui_render(&cpu, &inter);
        if (s_prof) t_ui = SDL_GetPerformanceCounter();

        /* Whole-VRAM sync for the GL backend's R16UI sampling mirror. Only a
         * driver without a texture barrier samples that mirror; everywhere else
         * the backend returns without copying anything (renderer_gl.c), and
         * Vulkan has no mirror at all. */
        renderer_upload_vram(&inter.gpu.renderer, (const uint16_t*)inter.gpu.vram.data);
        if (s_prof) t2 = SDL_GetPerformanceCounter();

        /* The VRAM viewer is filled by a shader pass on the GPU thread now, from
         * the same texture the rasteriser draws into. It used to be converted
         * here from gpu.vram.data and uploaded: 2 MB of staging per frame for an
         * image that could not show anything the game drew, because that buffer
         * only ever receives uploads, fills and DMA. */
        if (s_prof) t3 = SDL_GetPerformanceCounter();

        /* Submit frame to GPU thread: swap buffers, wake renderer */
        renderer_submit_frame(&inter.gpu.renderer, debug_ui_get_draw_data());
        if (s_prof) t4 = SDL_GetPerformanceCounter();
        debug_ui_flush_logs();

        if (s_prof) t_p0 = SDL_GetPerformanceCounter();
        /* Pacing.
         *
         * With an audio device open the sound queue is the clock: the emulator
         * runs ahead only until the SPU ring holds SPU_RING_TARGET_SAMPLES, then
         * waits for the device to drain some. Production and consumption are
         * then matched by construction — the emulator cannot outrun the device
         * (dropped samples) or fall behind it (silence in the callback), and
         * frame-pacing jitter never reaches the audio.
         *
         * Without a device there is nothing to synchronise to, so fall back to
         * pacing frames against the emulated refresh rate. */
        if (g_audio_stream) {
            while (!quit && spu_ring_used(&inter.spu) > SPU_RING_TARGET_SAMPLES)
                SDL_Delay(1);
            next_frame = SDL_GetPerformanceCounter() + frame_ticks;
        } else {
            uint64_t now = SDL_GetPerformanceCounter();
            if (now < next_frame) {
                uint64_t ms = ((next_frame - now) * 1000ULL) / SDL_GetPerformanceFrequency();
                if (ms > 1) SDL_Delay((uint32_t)(ms - 1));
                while (SDL_GetPerformanceCounter() < next_frame) {}
                next_frame += frame_ticks;
            } else {
                next_frame = now + frame_ticks;
            }
        }

        /* ZS1_FRAME_PROFILE=1: where the frame's wall-clock time actually goes.
         * The emulator has to deliver 44100 audio samples per real second, so
         * any millisecond spent outside emulation is a millisecond of audio the
         * device will not get.
         *
         * The first fields keep their names and meanings so old logs still
         * compare: emu, spu, vram_upload (debug UI + VRAM upload), viewer,
         * submit, total (their sum) and CPI. Appended after them:
         *   wait    renderer_wait_frame_done(): the GPU thread finishing the
         *           previous frame, buffer swap included;
         *   ui      debug_ui_render() on its own;
         *   upload  renderer_upload_vram() on its own;
         *   pace    the audio-ring / refresh pacing at the end of the field;
         *   frame   the whole loop iteration, wall clock;
         * and the worst field of the window for each, as *_max: a stall that
         * hits one field in sixty disappears in a mean. */
        if (s_prof) {
            const uint64_t t5 = SDL_GetPerformanceCounter();
            const double   ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
            extern uint64_t g_spu_gen_ticks;
            static uint64_t prev_t5 = 0;
            static int frames = 0;
            static double a_emu, a_up, a_view, a_sub, a_spu, a_wait, a_ui, a_upl, a_pace, a_frame;
            static double m_emu, m_wait, m_ui, m_upl, m_sub, m_pace, m_frame;
            const double d_emu   = (double)(t1 - t0) * ms;
            const double d_wait  = (double)(t_w1 - t_w0) * ms;
            const double d_ui    = (double)(t_ui - t_w1) * ms;
            const double d_upl   = (double)(t2 - t_ui) * ms;
            const double d_view  = (double)(t3 - t2) * ms;
            const double d_sub   = (double)(t4 - t3) * ms;
            const double d_pace  = (double)(t5 - t_p0) * ms;
            const double d_frame = prev_t5 ? (double)(t5 - prev_t5) * ms : 0.0;
            prev_t5 = t5;
            a_spu += (double)g_spu_gen_ticks * ms; g_spu_gen_ticks = 0;
            a_emu += d_emu;  a_up += d_ui + d_upl;  a_view += d_view;  a_sub += d_sub;
            a_wait += d_wait; a_ui += d_ui; a_upl += d_upl; a_pace += d_pace; a_frame += d_frame;
            if (d_emu   > m_emu)   m_emu   = d_emu;
            if (d_wait  > m_wait)  m_wait  = d_wait;
            if (d_ui    > m_ui)    m_ui    = d_ui;
            if (d_upl   > m_upl)   m_upl   = d_upl;
            if (d_sub   > m_sub)   m_sub   = d_sub;
            if (d_pace  > m_pace)  m_pace  = d_pace;
            if (d_frame > m_frame) m_frame = d_frame;
            if (++frames == 60) {
                /* Cycles per instruction, over the same 60 frames. This is the
                 * independent check on the RAM data-access cost in bus.c: that
                 * constant was calibrated so the BIOS VSync loop spans a field,
                 * and a single loop can be made to say anything. CPI is a second,
                 * unrelated observation over all executed code: real R3000A code
                 * is mostly single-cycle ALU work punctuated by memory access, so
                 * a believable figure sits a little above 1. A CPI near 1 means
                 * the model is not being charged; a CPI far above it means the
                 * constant is buying the VSync fix by slowing everything down. */
                static uint32_t prev_cyc;
                static uint64_t prev_ins;
                uint32_t cyc = inter.cpu_cycle_counter;   /* 32-bit and wraps */
                uint64_t ins = inter.instructions_retired;
                uint32_t dcyc = cyc - prev_cyc;           /* unsigned: wrap-safe */
                uint64_t dins = ins - prev_ins;
                double cpi = dins ? (double)dcyc / (double)dins : 0.0;
                prev_cyc = cyc; prev_ins = ins;
                LOG_SYSTEM_INFO("[PROF] per frame: emu=%.2fms (spu %.2fms) vram_upload=%.2fms viewer=%.2fms submit=%.2fms total=%.2fms | CPI=%.3f"
                                " | wait=%.2fms ui=%.2fms upload=%.2fms pace=%.2fms frame=%.2fms"
                                " | emu_max=%.2fms wait_max=%.2fms ui_max=%.2fms upload_max=%.2fms"
                                " submit_max=%.2fms pace_max=%.2fms frame_max=%.2fms",
                                a_emu / 60, a_spu / 60, a_up / 60, a_view / 60, a_sub / 60,
                                (a_emu + a_up + a_view + a_sub) / 60, cpi,
                                a_wait / 60, a_ui / 60, a_upl / 60, a_pace / 60, a_frame / 60,
                                m_emu, m_wait, m_ui, m_upl, m_sub, m_pace, m_frame);
                frames = 0;
                a_emu = a_up = a_view = a_sub = a_spu = 0;
                a_wait = a_ui = a_upl = a_pace = a_frame = 0;
                m_emu = m_wait = m_ui = m_upl = m_sub = m_pace = m_frame = 0;
            }
        }
    }

    // --- Cleanup ---
    cpu_dump_exec_trace(&cpu, "logs/exec_trace.log");
    audio_shutdown();
    renderer_stop_gpu_thread(&inter.gpu.renderer);
    /* Re-acquire GL context for cleanup calls (destroy, ImGui shutdown) */
    if (backend != GFX_BACKEND_VULKAN) SDL_GL_MakeCurrent(sdl.win, sdl.ctx);
    /* The renderer goes first, and the order is load-bearing.
     *
     * ImGui is two halves: the context, owned here, and a renderer backend
     * owned by whichever graphics backend is live. Tearing the context down
     * first leaves the Vulkan half holding descriptor sets and pipelines
     * belonging to a context that no longer exists, and the process segfaults
     * on the way out. The GL path never showed it because its ImGui half is
     * shut down from inside debug_ui_shutdown(), before the context goes.
     * renderer_destroy() shuts down the backend's ImGui half and then the
     * device; debug_ui_shutdown() drops the context afterwards. */
    renderer_destroy(&inter.gpu.renderer);
    debug_ui_shutdown();
    lua_debug_shutdown();
    cdrom_eject_disc(&inter.cdrom);
    shutdown_sdl(&sdl);
    LOG_SYSTEM_INFO("[SYSTEM] Shutdown complete");
    return 0;
}
