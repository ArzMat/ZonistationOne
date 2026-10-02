/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* The call-site level gate in include/log.h.
 *
 * Three properties, each of which a regression would break silently:
 *   - a level above ZS1_LOG_MAX_LEVEL (TRACE, by default) never reaches
 *     log_print, whatever the runtime level, and its arguments are not even
 *     evaluated;
 *   - every other level reaches log_print exactly when log_print's own first
 *     test would have let it through (level <= current_log_level), so the set
 *     of emitted lines is unchanged;
 *   - a variable used only as a log argument still counts as used when its
 *     level is compiled out (the zero-warnings rule). The pragma below turns
 *     that warning into an error, so this file would not build if the macro
 *     ever dropped its arguments instead of keeping them in a dead branch.
 */
#pragma GCC diagnostic error "-Wunused-variable"
#pragma GCC diagnostic error "-Wunused-but-set-variable"

#include "log.h"

#include <stdio.h>

LogLevel current_log_level = LOG_LEVEL_INFO;

static int         g_calls;
static LogCategory g_last_cat;
static LogLevel    g_last_level;

void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)format;
    g_calls++;
    g_last_cat = category;
    g_last_level = level;
}

static int g_evaluated;
static int side_effect(void) { g_evaluated++; return 42; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* One call per level through the GPU category, returning how many reached
 * log_print. */
static int emit_level(LogLevel lvl) {
    int before = g_calls;
    switch (lvl) {
        case LOG_LEVEL_ERROR: LOG_GPU_ERROR("e %d", 1); break;
        case LOG_LEVEL_WARN:  LOG_GPU_WARN("w %d", 2);  break;
        case LOG_LEVEL_INFO:  LOG_GPU_INFO("i %d", 3);  break;
        case LOG_LEVEL_DEBUG: LOG_GPU_DEBUG("d %d", 4); break;
        case LOG_LEVEL_TRACE: LOG_GPU_TRACE("t %d", 5); break;
        default: break;
    }
    return g_calls - before;
}

/* Only ever logged: must not warn when TRACE is compiled out. */
static void only_logged(void) {
    int dbg_page_x = side_effect();
    LOG_GTE_TRACE("[GTE] page %d", dbg_page_x);
}

int main(void) {
    int checks = 0;

    /* The matrix: every runtime level against every macro level. */
    for (int rt = LOG_LEVEL_SILENT; rt <= LOG_LEVEL_TRACE; rt++) {
        current_log_level = (LogLevel)rt;
        for (int lv = LOG_LEVEL_ERROR; lv <= LOG_LEVEL_TRACE; lv++) {
            int want = (lv <= rt && lv <= (int)ZS1_LOG_MAX_LEVEL) ? 1 : 0;
            int got  = emit_level((LogLevel)lv);
            CHECK(got == want, "runtime %d, level %d: %d call(s), expected %d", rt, lv, got, want);
            if (got) {
                CHECK(g_last_cat == LOG_CAT_GPU, "category %d", (int)g_last_cat);
                CHECK((int)g_last_level == lv, "level %d", (int)g_last_level);
            }
            checks++;
        }
    }

    /* TRACE is compiled out by default: no call and no argument evaluation,
     * even with the runtime level at TRACE. */
    current_log_level = LOG_LEVEL_TRACE;
    g_evaluated = 0;
    int before = g_calls;
    LOG_CPU_TRACE("[CPU] %d", side_effect());
    CHECK(ZS1_LOG_MAX_LEVEL != LOG_LEVEL_DEBUG || (g_calls == before && g_evaluated == 0),
          "TRACE reached log_print (calls %d, evaluated %d)", g_calls - before, g_evaluated);
    checks++;

    /* A dropped DEBUG line does not evaluate its arguments either. */
    current_log_level = LOG_LEVEL_INFO;
    g_evaluated = 0;
    LOG_GPU_DEBUG("[GPU] %d", side_effect());
    CHECK(g_evaluated == 0, "dropped DEBUG line evaluated its argument");
    checks++;

    /* ...and a passing one evaluates it exactly once. */
    current_log_level = LOG_LEVEL_DEBUG;
    g_evaluated = 0;
    LOG_GPU_DEBUG("[GPU] %d", side_effect());
    CHECK(g_evaluated == 1, "passing DEBUG line evaluated its argument %d times", g_evaluated);
    checks++;

    /* The macro is one statement: an unbraced if/else around it must pair up. */
    current_log_level = LOG_LEVEL_INFO;
    before = g_calls;
    if (checks < 0) LOG_SYSTEM_INFO("never"); else LOG_SYSTEM_INFO("always");
    CHECK(g_calls == before + 1, "if/else around a LOG_ macro mis-paired");
    checks++;

    /* The fallback names route through the same gate. */
    current_log_level = LOG_LEVEL_WARN;
    before = g_calls;
    LOG_INFO("dropped");
    LOG_WARN("kept");
    CHECK(g_calls == before + 1 && g_last_cat == LOG_CAT_SYSTEM, "fallback macros");
    checks++;

    current_log_level = LOG_LEVEL_TRACE;
    g_evaluated = 0;
    only_logged();
    CHECK(g_evaluated == 1, "a local computed for a compiled-out line is still computed");
    checks++;

    printf("log_gate_test: %d checks, %d failures (ZS1_LOG_MAX_LEVEL=%d)\n",
           checks, failures, (int)ZS1_LOG_MAX_LEVEL);
    return failures ? 1 : 0;
}
