/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

// PCSX ReARMed-style logging system with component categories and rate limiting

// --- Log Levels ---
typedef enum {
    LOG_LEVEL_SILENT = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_TRACE = 5
} LogLevel;

// --- Log Categories (PCSX ReARMed style) ---
typedef enum {
    LOG_CAT_SYSTEM = 0,     // General system messages
    LOG_CAT_CPU = 1,        // CPU execution, instructions
    LOG_CAT_IRQ = 2,        // Interrupt handling
    LOG_CAT_DMA = 3,        // DMA transfers
    LOG_CAT_GPU = 4,        // Graphics operations
    LOG_CAT_CDROM = 5,      // CD-ROM operations
    LOG_CAT_TIMER = 6,      // Timer events
    LOG_CAT_BIOS = 7,       // BIOS calls and operations
    LOG_CAT_INTERCONNECT = 8, // Memory/IO interconnect
    LOG_CAT_RENDERER = 9,   // OpenGL rendering
    LOG_CAT_EVENT = 10,     // Event scheduler
    LOG_CAT_GTE = 11,       // Geometry Transform Engine
    LOG_CAT_VRAM = 12,      // VRAM operations
    LOG_CAT_RAM = 13,       // RAM operations
    LOG_CAT_DEBUG = 14,     // Debugger operations
    LOG_CAT_MDEC = 15,      // MDEC (Macroblock Decoder)
    LOG_CAT_SPU = 16,       // Sound Processing Unit
    LOG_CAT_COUNT = 17      // Total number of categories
} LogCategory;

// --- Rate Limiting Structure ---
typedef struct {
    uint32_t count;         // How many times this has been logged
    uint32_t limit_first;   // Log first N messages
    uint32_t limit_every;   // Then log every Nth message
    bool enabled;           // Is this category enabled?
} LogCategoryState;

// --- Core Logging Functions ---
#ifdef __cplusplus
extern "C" {
#endif

void log_init(void);
void log_set_level(LogLevel level);
void log_set_category_enabled(LogCategory category, bool enabled);
void log_set_rate_limit(LogCategory category, uint32_t first_n, uint32_t every_n);
void log_set_output_file(const char* filename);
bool log_should_print(LogCategory category, LogLevel level);

// Internal logging function
void log_print(LogCategory category, LogLevel level, const char* format, ...);

/* The runtime level, read by the macros below before they call log_print().
 * Defined in log.c; log_set_level() is still the only writer. */
extern LogLevel current_log_level;

// BIOS/game TTY output (EXP2 DUART / printf-putc-puts capture). Always
// emitted regardless of the current log level or per-category enable state
// — this is the guest program's actual output, not a debug trace, so
// lowering the log level to cut noise must never hide it. Message should be
// the plain program text with no added tag (the log window/category already
// identifies the source).
void log_print_tty(const char* msg);

/* --- Emulated-clock stamp ---
 * Every dispatched line carries the machine's own clock, not the host wall
 * clock. Wall seconds cannot line this trace up against another emulator's
 * run: a whole boot phase (EXE load, game init) lands inside the same second,
 * which makes "who is faster, and where" unanswerable. The source is owned by
 * the machine and read once per emitted line, so it costs nothing on lines
 * the level gate already dropped. */
typedef struct {
    uint64_t cycle;   /* monotonic emulated CPU cycles since reset */
    uint32_t field;   /* CRTC fields (VBlanks) since reset */
} LogClock;

typedef void (*LogClockFn)(void* udata, LogClock* out);
void log_set_clock_source(LogClockFn fn, void* udata);


/* --- Level gate, decided at the call site ---
 *
 * Every LOG_* macro used to be an unconditional call to log_print(), a
 * variadic function, with the level test inside it. A line that the level
 * drops still paid for evaluating and spilling its arguments (16 of them for a
 * textured quad's DEBUG line) and for the call itself, on per-primitive,
 * per-GTE-op and per-macroblock paths. The test is cheap, so it moves to the
 * call site and the call only happens when the line can actually be printed.
 *
 * Two gates, in this order:
 *
 *   ZS1_LOG_MAX_LEVEL   compile time. Anything above it is a constant-false
 *                       branch the compiler deletes. Default DEBUG, so TRACE
 *                       (per-instruction, per-primitive) is not in a normal
 *                       build at all; `make LOG_MAX_LEVEL=TRACE` puts it back.
 *   current_log_level   run time, exactly the comparison log_print() makes
 *                       first. log_print() still repeats it and then applies the
 *                       per-category enable and the rate limit, so the lines
 *                       that get through are the same lines as before: a line
 *                       this gate drops would have been dropped by log_print()
 *                       before it touched any state (the rate-limit counter is
 *                       only incremented after the level test).
 *
 * The arguments stay inside the dead branch rather than disappearing from it.
 * A variable that only exists to be logged is then still "used" as far as
 * -Wunused-variable is concerned, which is what keeps the zero-warnings rule
 * when a level is compiled out. */
#ifndef ZS1_LOG_MAX_LEVEL
#define ZS1_LOG_MAX_LEVEL LOG_LEVEL_DEBUG
#endif

#define ZS1_LOG_AT(cat, lvl, ...)                                              \
    do {                                                                       \
        if ((int)(lvl) <= (int)(ZS1_LOG_MAX_LEVEL) &&                          \
            __builtin_expect((int)(lvl) <= (int)current_log_level, 0))         \
            log_print((cat), (lvl), __VA_ARGS__);                              \
    } while (0)

// --- Direct Component-Specific Macros (PCSX ReARMed style) ---
// Each one is ZS1_LOG_AT() with its category and level filled in
#define LOG_SYSTEM_ERROR(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_SYSTEM_WARN(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_SYSTEM_INFO(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_SYSTEM_DEBUG(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_SYSTEM_TRACE(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_CPU_ERROR(...) ZS1_LOG_AT(LOG_CAT_CPU, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_CPU_WARN(...) ZS1_LOG_AT(LOG_CAT_CPU, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_CPU_INFO(...) ZS1_LOG_AT(LOG_CAT_CPU, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_CPU_DEBUG(...) ZS1_LOG_AT(LOG_CAT_CPU, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_CPU_TRACE(...) ZS1_LOG_AT(LOG_CAT_CPU, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_IRQ_ERROR(...) ZS1_LOG_AT(LOG_CAT_IRQ, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_IRQ_WARN(...) ZS1_LOG_AT(LOG_CAT_IRQ, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_IRQ_INFO(...) ZS1_LOG_AT(LOG_CAT_IRQ, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_IRQ_DEBUG(...) ZS1_LOG_AT(LOG_CAT_IRQ, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_IRQ_TRACE(...) ZS1_LOG_AT(LOG_CAT_IRQ, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_DMA_ERROR(...) ZS1_LOG_AT(LOG_CAT_DMA, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_DMA_WARN(...) ZS1_LOG_AT(LOG_CAT_DMA, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_DMA_INFO(...) ZS1_LOG_AT(LOG_CAT_DMA, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DMA_DEBUG(...) ZS1_LOG_AT(LOG_CAT_DMA, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_DMA_TRACE(...) ZS1_LOG_AT(LOG_CAT_DMA, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_GPU_ERROR(...) ZS1_LOG_AT(LOG_CAT_GPU, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_GPU_WARN(...) ZS1_LOG_AT(LOG_CAT_GPU, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_GPU_INFO(...) ZS1_LOG_AT(LOG_CAT_GPU, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_GPU_DEBUG(...) ZS1_LOG_AT(LOG_CAT_GPU, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_GPU_TRACE(...) ZS1_LOG_AT(LOG_CAT_GPU, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_CDROM_ERROR(...) ZS1_LOG_AT(LOG_CAT_CDROM, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_CDROM_WARN(...) ZS1_LOG_AT(LOG_CAT_CDROM, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_CDROM_INFO(...) ZS1_LOG_AT(LOG_CAT_CDROM, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_CDROM_DEBUG(...) ZS1_LOG_AT(LOG_CAT_CDROM, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_CDROM_TRACE(...) ZS1_LOG_AT(LOG_CAT_CDROM, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_TIMER_ERROR(...) ZS1_LOG_AT(LOG_CAT_TIMER, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_TIMER_WARN(...) ZS1_LOG_AT(LOG_CAT_TIMER, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_TIMER_INFO(...) ZS1_LOG_AT(LOG_CAT_TIMER, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_TIMER_DEBUG(...) ZS1_LOG_AT(LOG_CAT_TIMER, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TIMER_TRACE(...) ZS1_LOG_AT(LOG_CAT_TIMER, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_SIO_ERROR(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_SIO_WARN(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_SIO_INFO(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_SIO_DEBUG(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_SIO_TRACE(...) ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_BIOS_ERROR(...) ZS1_LOG_AT(LOG_CAT_BIOS, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_BIOS_WARN(...) ZS1_LOG_AT(LOG_CAT_BIOS, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_BIOS_INFO(...) ZS1_LOG_AT(LOG_CAT_BIOS, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_BIOS_DEBUG(...) ZS1_LOG_AT(LOG_CAT_BIOS, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_BIOS_TRACE(...) ZS1_LOG_AT(LOG_CAT_BIOS, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_INTERCONNECT_ERROR(...) ZS1_LOG_AT(LOG_CAT_INTERCONNECT, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_INTERCONNECT_WARN(...) ZS1_LOG_AT(LOG_CAT_INTERCONNECT, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INTERCONNECT_INFO(...) ZS1_LOG_AT(LOG_CAT_INTERCONNECT, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_INTERCONNECT_DEBUG(...) ZS1_LOG_AT(LOG_CAT_INTERCONNECT, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INTERCONNECT_TRACE(...) ZS1_LOG_AT(LOG_CAT_INTERCONNECT, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_RENDERER_ERROR(...) ZS1_LOG_AT(LOG_CAT_RENDERER, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_RENDERER_WARN(...) ZS1_LOG_AT(LOG_CAT_RENDERER, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_RENDERER_INFO(...) ZS1_LOG_AT(LOG_CAT_RENDERER, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_RENDERER_DEBUG(...) ZS1_LOG_AT(LOG_CAT_RENDERER, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_RENDERER_TRACE(...) ZS1_LOG_AT(LOG_CAT_RENDERER, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_EVENT_ERROR(...) ZS1_LOG_AT(LOG_CAT_EVENT, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_EVENT_WARN(...) ZS1_LOG_AT(LOG_CAT_EVENT, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_EVENT_INFO(...) ZS1_LOG_AT(LOG_CAT_EVENT, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_EVENT_DEBUG(...) ZS1_LOG_AT(LOG_CAT_EVENT, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_EVENT_TRACE(...) ZS1_LOG_AT(LOG_CAT_EVENT, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_GTE_ERROR(...) ZS1_LOG_AT(LOG_CAT_GTE, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_GTE_WARN(...) ZS1_LOG_AT(LOG_CAT_GTE, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_GTE_INFO(...) ZS1_LOG_AT(LOG_CAT_GTE, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_GTE_DEBUG(...) ZS1_LOG_AT(LOG_CAT_GTE, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_GTE_TRACE(...) ZS1_LOG_AT(LOG_CAT_GTE, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_VRAM_ERROR(...) ZS1_LOG_AT(LOG_CAT_VRAM, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_VRAM_WARN(...) ZS1_LOG_AT(LOG_CAT_VRAM, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_VRAM_INFO(...) ZS1_LOG_AT(LOG_CAT_VRAM, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_VRAM_DEBUG(...) ZS1_LOG_AT(LOG_CAT_VRAM, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_VRAM_TRACE(...) ZS1_LOG_AT(LOG_CAT_VRAM, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_RAM_ERROR(...) ZS1_LOG_AT(LOG_CAT_RAM, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_RAM_WARN(...) ZS1_LOG_AT(LOG_CAT_RAM, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_RAM_INFO(...) ZS1_LOG_AT(LOG_CAT_RAM, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_RAM_DEBUG(...) ZS1_LOG_AT(LOG_CAT_RAM, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_RAM_TRACE(...) ZS1_LOG_AT(LOG_CAT_RAM, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_DEBUGGER_ERROR(...) ZS1_LOG_AT(LOG_CAT_DEBUG, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_DEBUGGER_WARN(...) ZS1_LOG_AT(LOG_CAT_DEBUG, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_DEBUGGER_INFO(...) ZS1_LOG_AT(LOG_CAT_DEBUG, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUGGER_DEBUG(...) ZS1_LOG_AT(LOG_CAT_DEBUG, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_DEBUGGER_TRACE(...) ZS1_LOG_AT(LOG_CAT_DEBUG, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_MDEC_ERROR(...) ZS1_LOG_AT(LOG_CAT_MDEC, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_MDEC_WARN(...) ZS1_LOG_AT(LOG_CAT_MDEC, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_MDEC_INFO(...) ZS1_LOG_AT(LOG_CAT_MDEC, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_MDEC_DEBUG(...) ZS1_LOG_AT(LOG_CAT_MDEC, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_MDEC_TRACE(...) ZS1_LOG_AT(LOG_CAT_MDEC, LOG_LEVEL_TRACE, __VA_ARGS__)

#define LOG_SPU_ERROR(...) ZS1_LOG_AT(LOG_CAT_SPU, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_SPU_WARN(...) ZS1_LOG_AT(LOG_CAT_SPU, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_SPU_INFO(...) ZS1_LOG_AT(LOG_CAT_SPU, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_SPU_DEBUG(...) ZS1_LOG_AT(LOG_CAT_SPU, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_SPU_TRACE(...) ZS1_LOG_AT(LOG_CAT_SPU, LOG_LEVEL_TRACE, __VA_ARGS__)

// Legacy compatibility for custom macros
#define LOG_TIMERS_ERROR(...) LOG_TIMER_ERROR(__VA_ARGS__)
#define LOG_TIMERS_WARN(...) LOG_TIMER_WARN(__VA_ARGS__)
#define LOG_TIMERS_INFO(...) LOG_TIMER_INFO(__VA_ARGS__)
#define LOG_TIMERS_DEBUG(...) LOG_TIMER_DEBUG(__VA_ARGS__)




// --- Simple fallback macros for old code (no categories) ---
#define LOG_INFO(...)    ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_ERROR(...)   ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)    ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_DEBUG(...)   ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...)   ZS1_LOG_AT(LOG_CAT_SYSTEM, LOG_LEVEL_TRACE, __VA_ARGS__)

// Logging macro usage:
// Use LOG_<CATEGORY>_<LEVEL>(...) for all logging calls, e.g. LOG_CPU_INFO(...), LOG_CDROM_WARN(...)
// Only use fallback LOG_INFO/LOG_ERROR/etc. for generic system messages.
// Avoid legacy macros like LOG_CPU_IMPORTANT, LOG_CDROM_IMPORTANT.

// Old function compatibility
LogLevel log_get_current_level(void);
#define log_get_level() log_get_current_level()
const char* log_category_name(LogCategory category);

// --- Multi-sink API (ImGui, custom callbacks) ---
// Registered sinks receive raw (category, level, message) before rxi formatting.
typedef void (*LogSinkFn)(int category, int level, const char* msg, void* udata);
void log_add_sink(LogSinkFn fn, void* udata);
void log_set_stderr_quiet(bool quiet);  // suppress colored stderr (e.g. when only logging to file)

#ifdef __cplusplus
}
#endif

#endif // LOG_H