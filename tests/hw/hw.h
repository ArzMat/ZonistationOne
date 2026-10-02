/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared helpers for the bare-metal hardware tests (docs/TESTING_PLAN_2026-08-20.md,
 * layer 2). Each test is a PS-X EXE that runs on a zero-filled BIOS through the
 * emulator's --exe loader: no kernel, interrupts off (SR after reset), so the
 * tests poll every status bit and never raise an exception.
 *
 * Results go out through the DUART TTY port the emulator already captures
 * (1F802023h, src/core/bus.c), one line per check:
 *   HWTEST <suite> <check> PASS
 *   HWTEST <suite> <check> FAIL got=XXXXXXXX want=XXXXXXXX
 *   HWTEST <suite> DONE pass=N fail=M
 * tests/hw/run.sh parses them.
 */
#ifndef ZS1_HW_TEST_H
#define ZS1_HW_TEST_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t*)(a))
#define REG16(a) (*(volatile uint16_t*)(a))
#define REG8(a)  (*(volatile uint8_t*)(a))

/* Uncached KSEG1 views of the I/O ports. */
#define TTY_DATA   REG8(0xBF802023)
#define I_STAT     REG32(0xBF801070)
#define DPCR       REG32(0xBF8010F0)
#define GP0        REG32(0xBF801810)
#define GPUREAD    REG32(0xBF801810)
#define GP1        REG32(0xBF801814)
#define GPUSTAT    REG32(0xBF801814)
#define MDEC_DATA  REG32(0xBF801820)
#define MDEC_CTRL  REG32(0xBF801824)

/* Polling bound: generous enough for any transfer these tests make, small
 * enough that a broken path ends the test with a FAIL line instead of a hang. */
#define POLL_LIMIT 2000000u

static const char* g_suite;
static int g_pass, g_fail;

static inline void tty_putc(char c) { TTY_DATA = (uint8_t)c; }

static inline void tty_puts(const char* s) {
    while (*s) tty_putc(*s++);
}

static inline void tty_hex(uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nib = (v >> shift) & 0xF;
        tty_putc((char)(nib < 10 ? '0' + nib : 'A' + nib - 10));
    }
}

static inline void tty_dec(uint32_t v) {
    char buf[11];
    int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) tty_putc(buf[--n]);
}

static inline void suite_begin(const char* name) {
    g_suite = name;
    g_pass = g_fail = 0;
    tty_puts("HWTEST "); tty_puts(name); tty_puts(" BEGIN\n");
}

static inline void check(const char* name, int ok, uint32_t got, uint32_t want) {
    tty_puts("HWTEST "); tty_puts(g_suite); tty_putc(' '); tty_puts(name);
    if (ok) {
        g_pass++;
        tty_puts(" PASS\n");
    } else {
        g_fail++;
        tty_puts(" FAIL got="); tty_hex(got); tty_puts(" want="); tty_hex(want); tty_putc('\n');
    }
}

static inline void suite_end(void) {
    tty_puts("HWTEST "); tty_puts(g_suite); tty_puts(" DONE pass=");
    tty_dec((uint32_t)g_pass); tty_puts(" fail="); tty_dec((uint32_t)g_fail); tty_putc('\n');
}

static inline void spin(uint32_t n) {
    while (n--) __asm__ volatile("nop");
}

/* Physical address of a RAM object, as the DMA registers want it. */
static inline uint32_t phys(const volatile void* p) {
    return (uint32_t)p & 0x1FFFFFu;
}

/* --- GPU ------------------------------------------------------------------ */

static inline void gp0(uint32_t v) {
    uint32_t n = POLL_LIMIT;
    while (!(GPUSTAT & (1u << 26)) && --n) {}      /* ready to receive a command word */
    GP0 = v;
}

/* GP0(C0h) VRAM->CPU, w*h halfwords into out (w*h even). Returns 0 on timeout. */
static inline int gpu_read_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t* out) {
    gp0(0x01000000u);                              /* clear texture cache */
    gp0(0xC0000000u);
    gp0((y << 16) | x);
    gp0((h << 16) | w);
    uint32_t n = POLL_LIMIT;
    while (!(GPUSTAT & (1u << 27)) && --n) {}      /* ready to send VRAM to CPU */
    if (!n) return 0;
    for (uint32_t i = 0; i < (w * h + 1) / 2; i++) {
        uint32_t word = GPUREAD;
        out[2 * i] = (uint16_t)word;
        out[2 * i + 1] = (uint16_t)(word >> 16);
    }
    return 1;
}

static inline void gpu_set_drawing_area(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2) {
    gp0(0xE3000000u | (y1 << 10) | x1);
    gp0(0xE4000000u | (y2 << 10) | x2);
    gp0(0xE5000000u);                              /* drawing offset 0,0 */
}

/* GP0(60h) monochrome rectangle, variable size; colour is 24-bit BGR. */
static inline void gpu_rect(uint32_t bgr, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    gp0(0x60000000u | (bgr & 0xFFFFFFu));
    gp0((y << 16) | x);
    gp0((h << 16) | w);
}

#endif
