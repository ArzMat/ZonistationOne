/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "cpu.h"
#include "cpu_mem.h"
#include "interconnect.h"
#include "bios.h"
#include "log.h"
#include "timers.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// =============================================================================
// BIOS Syscall Side-Channel Capture (LLE)
//
// Called from op_jr BEFORE the CPU jumps to 0xA0 / 0xB0.
// The BIOS still executes normally — we only read arguments to capture TTY
// output.  We do NOT set return values ($v0/$v1).
//
// All character output is funnelled through the same line buffer as the
// EXP2+0x23 DUART path (inter->tty_line_buf), so both sources produce
// consistent [BIOS TTY] output on stderr when a newline is received.
// =============================================================================

// Track the most recent BIOS syscall for context
static struct {
    int table;  // 0 = A0, 1 = B0
    uint32_t func;
    const char* name;
} bios_last_syscall = {0, 0, "none"};

static const char* get_bios_a_function_name(uint32_t func_num) {
    /* nocash PSX A-function table, 0x00-0xBF.
     * 0x73-0xBF: BIOS internal / _96_Cd functions — partially documented. */
    static const char* names[0xC0] = {
        /* 0x00-0x3F: standard C library */
        [0x00]="open",       [0x01]="lseek",      [0x02]="read",        [0x03]="write",
        [0x04]="close",      [0x05]="ioctl",       [0x06]="exit",        [0x07]="isatty",
        [0x08]="getc",       [0x09]="putc",        [0x0A]="todigit",     [0x0B]="atof",
        [0x0C]="strtoul",    [0x0D]="strtol",      [0x0E]="abs",         [0x0F]="labs",
        [0x10]="atoi",       [0x11]="atol",        [0x12]="atob",        [0x13]="setjmp",
        [0x14]="longjmp",    [0x15]="strcat",      [0x16]="strncat",     [0x17]="strcmp",
        [0x18]="strncmp",    [0x19]="strcpy",      [0x1A]="strncpy",     [0x1B]="strlen",
        [0x1C]="index",      [0x1D]="rindex",      [0x1E]="strchr",      [0x1F]="strrchr",
        [0x20]="strpbrk",    [0x21]="strspn",      [0x22]="strcspn",     [0x23]="strtok",
        [0x24]="strstr",     [0x25]="toupper",     [0x26]="tolower",     [0x27]="bcopy",
        [0x28]="bzero",      [0x29]="bcmp",        [0x2A]="memcpy",      [0x2B]="memset",
        [0x2C]="memmove",    [0x2D]="memcmp",      [0x2E]="memchr",      [0x2F]="rand",
        [0x30]="srand",      [0x31]="qsort",       [0x32]="strtod",      [0x33]="malloc",
        [0x34]="free",       [0x35]="lsearch",     [0x36]="bsearch",     [0x37]="calloc",
        [0x38]="realloc",    [0x39]="InitHeap",    [0x3A]="_exit",       [0x3B]="getchar",
        [0x3C]="putchar",    [0x3D]="gets",        [0x3E]="puts",        [0x3F]="printf",
        /* 0x40-0x72: BIOS internals (nocash PSX) */
        [0x40]="SystemErrorUnresolvedException",
        [0x41]="LoadExeHeader",     [0x42]="LoadExeFile",       [0x43]="DoExecute",
        [0x44]="FlushCache",        [0x45]="init_a0_b0_c0_vectors",
        [0x46]="GPU_dw",            [0x47]="gpu_send_dma",
        [0x48]="SendGP1Command",    [0x49]="GPU_cw",
        [0x4A]="GPU_cwp",           [0x4B]="send_gpu_linked_list",
        [0x4C]="gpu_abort_dma",     [0x4D]="GetGPUStatus",      [0x4E]="gpu_sync",
        [0x4F]="SystemError",       [0x50]="SystemError",
        [0x51]="LoadAndExecute",    [0x52]="SystemError",        [0x53]="SystemError",
        [0xA1]="SystemError",
        [0x54]="CdInit",            [0x55]="_bu_init",           [0x56]="CdRemove",
        [0x57]="unused",            [0x58]="unused",
        [0x59]="unused",            [0x5A]="unused",
        [0x5B]="dev_tty_init",      [0x5C]="dev_tty_open",
        [0x5D]="dev_tty_inout",     [0x5E]="dev_tty_ioctl",
        [0x5F]="dev_cd_open",       [0x60]="dev_cd_read",
        [0x61]="dev_cd_close",      [0x62]="dev_cd_firstfile",
        [0x63]="dev_cd_nextfile",   [0x64]="dev_cd_chdir",
        [0x65]="dev_card_open",     [0x66]="dev_card_read",
        [0x67]="dev_card_write",    [0x68]="dev_card_close",
        [0x69]="dev_card_firstfile",[0x6A]="dev_card_nextfile",
        [0x6B]="dev_card_erase",    [0x6C]="dev_card_undelete",
        [0x6D]="dev_card_format",   [0x6E]="dev_card_rename",
        [0x6F]="dev_card_6f",
        [0x70]="_bu_init",          [0x71]="_96_init",           [0x72]="_96_remove",
        /* 0x73-0x77: undocumented */
        [0x78]="_96_CdSeekL",
        /* 0x79-0x7B: undocumented */
        [0x7C]="_96_CdGetStatus",
        [0x7E]="_96_CdRead",
        [0x7F]="_96_CdReadSector",
        [0x80]="_96_CdSeekP",
        /* 0x81-0xA7: undocumented BIOS internals */
        [0xA8]="BIOS_int_A8",
        /* 0xA9-0xBB: undocumented */
        [0xBC]="BIOS_int_BC",   /* called ~70x/frame from shell rendering loop */
        /* 0xBD-0xBF: undocumented */
    };
    if (func_num < 0xC0 && names[func_num]) return names[func_num];
    return "unknown";
}

static const char* get_bios_b_function_name(uint32_t func_num) {
    static const char* names[] = {
        "alloc_kernel_memory", "free_kernel_memory", "init_timer", "get_timer",
        "enable_timer_irq", "disable_timer_irq", "restart_timer", "DeliverEvent",
        "OpenEvent", "CloseEvent", "WaitEvent", "TestEvent", "EnableEvent", "DisableEvent",
        "OpenTh", "CloseTh", "ChangeTh", "psx_stub (jump_to_00)", "InitPAD2", "StartPAD2",
        "StopPAD2", "PAD_init2", "PAD_dr", "ReturnFromException", "ResetEntryInt",
        "HookEntryInt", "SystemError", "SystemError", "SystemError", "SystemError", 
        "SystemError", "SystemError", "UnDeliverEvent", "SystemError", "SystemError", 
        "SystemError", "psx_stub", "psx_stub", "psx_stub", "psx_stub", "psx_stub", 
        "psx_stub", "SystemError", "SystemError", "psx_stub", "psx_stub", "psx_stub", 
        "psx_stub", "psx_stub", "psx_stub", "open", "lseek", "read", "write", "close", 
        "ioctl", "exit", "isatty", "getc", "putc", "getchar", "putchar", "gets", "puts", 
        "cd", "format", "firstfile2", "nextfile", "rename", "erase", "undelete", 
        "AddDrv", "DelDrv", "PrintInstalledDevices", "InitCARD2", "StartCARD2", "StopCARD2", 
        "_card_info_subfunc", "_card_write", "_card_read", "_new_card", "Krom2RawAdd", 
        "SystemError", "Krom2Offset", "_get_errno", "_get_error", "GetC0Table", "GetB0Table", 
        "_card_chan", "testdevice", "SystemError", "ChangeClearPAD", "_card_status", "_card_wait"
    };
    if (func_num <= 0x5D) return names[func_num];
    return "unknown";
}

static const char* get_bios_c_function_name(uint32_t func_num) {
    /* nocash PSX C-function table (0x00-0x1F) */
    static const char* names[0x1A] = {
        [0x00]="EnqueueTimerAndVblankIrqs",
        [0x01]="EnqueueSyscallHandler",
        [0x02]="SysEnqIntRP",
        [0x03]="SysDeqIntRP",
        [0x04]="get_free_EvCB_slot",
        [0x05]="get_free_TCB_slot",
        [0x06]="ExceptionHandler",
        [0x07]="InstallExceptionHandlers",
        [0x08]="SysInitMemory",
        [0x09]="SysInitKernelVariables",
        [0x0A]="ChangeClearRCnt",
        [0x0B]="SystemError",
        [0x0C]="InitDefInt",
        [0x0D]="SystemError",  [0x0E]="SystemError",  [0x0F]="SystemError",
        [0x10]="SystemError",  [0x11]="SystemError",  [0x12]="SystemError",
        [0x13]="FlushStfIoBuffer",
        [0x14]="unused",  [0x15]="unused",  [0x16]="unused",  [0x17]="unused",
        [0x18]="KernelRedirect",
        [0x19]="PatchAOTable",
    };
    if (func_num < 0x1A && names[func_num]) return names[func_num];
    return "unknown";
}

/* Guest memory as the side channel reads it: a peek, not a bus access.
 *
 * Everything in this file is a host-side observer. It used to read the guest's
 * strings and printf arguments with interconnect_load8/32(), which is the CPU's
 * own load path: each byte charged a RAM load stall to the instruction that
 * happened to be executing (so a BIOS printf made the emulated machine slower
 * by three cycles per character of its format string), went through the read
 * watchpoints (so a watch on a string buffer fired on the logger's read), and
 * would have read an I/O register for real had a pointer ever landed there.
 *
 * This reads only memory that has no side effects (main RAM with its mirrors,
 * the scratchpad, the BIOS ROM), decoded the way interconnect_load8() decodes
 * them, and returns 0 for anything else, which ends a string. */
static uint8_t bios_peek8(const Interconnect* inter, uint32_t vaddr) {
    const uint32_t phys = bus_mask_region(vaddr);
    if (phys < CPU_MEM_RAM_WINDOW_END)
        return inter->ram->data[phys & (RAM_SIZE - 1)];
    if (phys >= 0x1F800000u && phys < 0x1F800000u + SCRATCHPAD_SIZE && vaddr < 0xA0000000u)
        return inter->scratchpad[phys - 0x1F800000u];
    if (phys >= 0x1FC00000u && phys < 0x20000000u)
        return inter->bios->data[(phys - 0x1FC00000u) & (BIOS_SIZE - 1)];
    return 0;
}

static uint32_t bios_peek32(const Interconnect* inter, uint32_t vaddr) {
    return (uint32_t)bios_peek8(inter, vaddr)
         | ((uint32_t)bios_peek8(inter, vaddr + 1) << 8)
         | ((uint32_t)bios_peek8(inter, vaddr + 2) << 16)
         | ((uint32_t)bios_peek8(inter, vaddr + 3) << 24);
}

// Adds one character to the interconnect's TTY line buffer.
// Flushes to stderr as a plain line on newline.
// This is the syscall side-channel, not the DUART; interconnect_tty_char drops
// it once the DUART is live so the same line is not logged from both.
static void tty_add_char(Interconnect* inter, char ch) {
    interconnect_tty_char(inter, ch, false);
}

/* ZS1_TTY_TRACE=1 names the hook behind every captured line.
 *
 * Some lines reach the log twice and some do not, and two guesses at which pair
 * of hooks overlaps were both wrong. Rather than guess a third time, each hook
 * says what it saw, so the duplicate pair is read off instead of inferred. */
static void tty_trace(const char* hook, const char* text, uint32_t len) {
    static int on = -1;
    if (on < 0) on = getenv("ZS1_TTY_TRACE") ? 1 : 0;
    if (!on) return;
    char buf[160];
    uint32_t n = len < sizeof(buf) - 1 ? len : (uint32_t)sizeof(buf) - 1;
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = text[i];
        buf[o++] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    buf[o] = '\0';
    LOG_BIOS_DEBUG("[TTYSRC] %-6s len=%u \"%s\"", hook, (unsigned)len, buf);
}

// Capture write(fd, buf, len) — stdout (fd=1) and stderr (fd=2) only
static void capture_bios_write(Cpu* cpu) {
    uint32_t fd  = cpu->regs[4];  // $a0
    uint32_t buf = cpu->regs[5];  // $a1
    uint32_t len = cpu->regs[6];  // $a2

    if ((fd == 1 || fd == 2) && cpu->inter && len > 0 && len < 0x10000) {
        for (uint32_t i = 0; i < len; i++) {
            char ch = (char)bios_peek8(cpu->inter, buf + i);
            tty_trace("write", &ch, 1);
            tty_add_char(cpu->inter, ch);
        }
    }
}

// MIPS o32 varargs cursor for printf(fmt, ...): first 3 varargs come from
// $a1-$a3, the rest from the caller's stack at $sp+16, $sp+20, ... (the o32
// ABI always reserves 4 argument-shadow words on the stack, so args beyond
// $a3 start right after that reserved block).
typedef struct {
    Cpu*     cpu;
    uint32_t a1, a2, a3;
    int      next_reg;    // 0..3: how many of {a1,a2,a3} consumed so far
    uint32_t sp;
    int      stack_slot;  // next stack word index beyond $sp+16
} PrintfArgs;

static uint32_t printf_next_u32(PrintfArgs* pa) {
    switch (pa->next_reg) {
        case 0: pa->next_reg++; return pa->a1;
        case 1: pa->next_reg++; return pa->a2;
        case 2: pa->next_reg++; return pa->a3;
        default: {
            uint32_t addr = pa->sp + 16u + (uint32_t)pa->stack_slot * 4u;
            pa->stack_slot++;
            return bios_peek32(pa->cpu->inter, addr);
        }
    }
}

// Capture printf(fmt, ...) — actually substitute %s/%d/%x/etc. from the
// caller's registers/stack per the MIPS o32 varargs convention, instead of
// dumping the raw format string. Each %-directive is re-extracted verbatim
// (flags/width/precision/length + conversion char) and handed to the host's
// real snprintf with a correctly-typed argument, so width/zero-pad specs
// like "%08x" format exactly as real hardware would show them.
static void capture_bios_printf(Cpu* cpu) {
    uint32_t fmt = cpu->regs[4];  // $a0 = format string pointer
    if (!fmt || !cpu->inter) return;

    char fmtbuf[512];
    uint32_t flen = 0;
    for (; flen < sizeof(fmtbuf) - 1; flen++) {
        uint8_t b = bios_peek8(cpu->inter, fmt + flen);
        if (b == 0) break;
        fmtbuf[flen] = (char)b;
    }
    fmtbuf[flen] = '\0';

    PrintfArgs pa = {
        .cpu = cpu,
        .a1 = cpu->regs[5], .a2 = cpu->regs[6], .a3 = cpu->regs[7],
        .next_reg = 0,
        .sp = cpu->regs[29],
        .stack_slot = 0,
    };

    char out[1024];
    uint32_t oi = 0;
    for (uint32_t i = 0; i < flen && oi < sizeof(out) - 1; ) {
        if (fmtbuf[i] != '%') { out[oi++] = fmtbuf[i++]; continue; }

        uint32_t start = i;
        i++; // skip '%'
        while (i < flen && strchr("-+ #0", fmtbuf[i])) i++;
        while (i < flen && fmtbuf[i] >= '0' && fmtbuf[i] <= '9') i++;
        if (i < flen && fmtbuf[i] == '.') {
            i++;
            while (i < flen && fmtbuf[i] >= '0' && fmtbuf[i] <= '9') i++;
        }
        while (i < flen && strchr("hlLqjzt", fmtbuf[i])) i++;
        if (i >= flen) {
            for (uint32_t k = start; k < flen && oi < sizeof(out) - 1; k++) out[oi++] = fmtbuf[k];
            break;
        }
        char conv = fmtbuf[i++];

        char directive[32];
        uint32_t dlen = i - start;
        if (dlen >= sizeof(directive)) dlen = sizeof(directive) - 1;
        memcpy(directive, fmtbuf + start, dlen);
        directive[dlen] = '\0';

        int written;
        switch (conv) {
            case '%':
                if (oi < sizeof(out) - 1) out[oi++] = '%';
                continue; // literal '%', no arg consumed

            case 's': {
                uint32_t ptr = printf_next_u32(&pa);
                char strbuf[256];
                uint32_t si = 0;
                if (ptr) {
                    for (; si < sizeof(strbuf) - 1; si++) {
                        uint8_t b = bios_peek8(cpu->inter, ptr + si);
                        if (b == 0) break;
                        strbuf[si] = (char)b;
                    }
                }
                strbuf[si] = '\0';
                written = snprintf(out + oi, sizeof(out) - oi, directive, ptr ? strbuf : "(null)");
                break;
            }
            case 'c': {
                int v = (int)printf_next_u32(&pa);
                written = snprintf(out + oi, sizeof(out) - oi, directive, v);
                break;
            }
            case 'd': case 'i': {
                int32_t v = (int32_t)printf_next_u32(&pa);
                written = snprintf(out + oi, sizeof(out) - oi, directive, v);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                uint32_t v = printf_next_u32(&pa);
                written = snprintf(out + oi, sizeof(out) - oi, directive, v);
                break;
            }
            case 'p': {
                // %p expects a host pointer-sized arg; PS1 addresses are 32-bit,
                // so widen explicitly rather than passing a bare uint32_t (which
                // would under-supply the vararg slot on a 64-bit host).
                uint32_t v = printf_next_u32(&pa);
                written = snprintf(out + oi, sizeof(out) - oi, directive, (void*)(uintptr_t)v);
                break;
            }
            default:
                // Unknown conversion: copy the directive text as-is, consume no arg.
                for (uint32_t k = start; k < i && oi < sizeof(out) - 1; k++) out[oi++] = fmtbuf[k];
                continue;
        }
        if (written > 0) oi += (uint32_t)written;
    }
    if (oi >= sizeof(out)) oi = sizeof(out) - 1;
    out[oi] = '\0';

    // Remember it so the write() the BIOS makes with these same bytes is not
    // logged again — see s_printf_echo.
    /* Traced, deliberately not emitted.
     *
     * The BIOS's own printf writes the formatted text out one character at a
     * time through putchar, which is hooked too, so emitting here as well put
     * every formatted line in the log twice. The character path is the one that
     * cannot miss anything — it is what the BIOS actually does — so this hook
     * keeps only the decoded form, for ZS1_TTY_TRACE. */
    tty_trace("printf", out, oi);
}

// Capture putc(c, fd) / putchar(c)
static void capture_bios_putc(Cpu* cpu) {
    uint32_t c  = cpu->regs[4] & 0xFF;  // $a0 = character
    uint32_t fd = cpu->regs[5];          // $a1 = file descriptor (putchar: irrelevant)
    uint32_t fn = cpu->regs[9];          // $t1 = A0/B0 function number

    // putchar (A0:0x3C, B0:0x3D) has no fd; putc (A0:0x09, B0:0x3B) uses fd
    int is_putchar = (fn == 0x3C || fn == 0x3D);
    { char cc = (char)c; tty_trace("putc", &cc, 1); }
    if (is_putchar || fd == 1 || fd == 2)
        tty_add_char(cpu->inter, (char)c);
}

// Capture puts(str)
static void capture_bios_puts(Cpu* cpu) {
    uint32_t str = cpu->regs[4];  // $a0 = string pointer

    if (str && cpu->inter) {
        { char t[128]; uint32_t k=0;
          for (; k < sizeof(t)-1; k++) { uint8_t b=bios_peek8(cpu->inter, str+k); if(!b) break; t[k]=(char)b; }
          tty_trace("puts", t, k); }
        for (uint32_t i = 0; i < 512; i++) {
            uint8_t b = bios_peek8(cpu->inter, str + i);
            if (b == 0) break;
            tty_add_char(cpu->inter, (char)b);
        }
        tty_add_char(cpu->inter, '\n');  // puts() appends newline
    }
}

// Called from op_jr when target == 0xA0.
// Returns true if the call was HLE'd; op_jr must then skip the native jump (next_pc already set).
bool handle_a0_syscall(Cpu* cpu) {
    uint32_t call = cpu->regs[9]; // $t1
    /* Filter out garbage JR targets misidentified as syscalls (e.g. 0x1E988, 0x1F801800).
     * Exception: the A0 dispatch table does SLL $t1,$t1,2 before jumping to the function;
     * a re-entrant A0 call from inside the dispatch stub therefore arrives with $t1 already
     * shifted left by 2 (e.g. 0xA1 → 0x284).  Recover the original index. */
    if (call > 0xFF) {
        if ((call & 3) == 0 && (call >> 2) <= 0xFF)
            call >>= 2;   /* undo the SLL×2 from the dispatch table */
        else
            return false;
    }

    bios_last_syscall.table = 0;
    bios_last_syscall.func = call;
    bios_last_syscall.name = get_bios_a_function_name(call);

    /* Suppress high-frequency calls */
    if (call == 0xBC) return false;
    if (call == 0x40) {
        /* Only log A0(0x40) once — it becomes an infinite loop after crash */
        static bool a40_logged = false;
        if (a40_logged) return false;
        a40_logged = true;
    }
    LOG_CPU_DEBUG("[CPU] A0(%s / 0x%02X) called from $ra=0x%08x", bios_last_syscall.name, call, cpu->regs[31]);
    if (call == 0x00 && cpu->inter) {
        uint32_t name_ptr = cpu->regs[4];
        char name[64]; uint32_t i;
        for (i = 0; i < sizeof(name) - 1 && name_ptr; i++) {
            uint8_t b = bios_peek8(cpu->inter, name_ptr + i);
            if (b == 0) break;
            name[i] = (char)b;
        }
        name[i] = 0;
        LOG_CPU_DEBUG("[CPU] open(\"%s\", 0x%x) called from $ra=0x%08x $a0=0x%08x",
                      name, cpu->regs[5], cpu->regs[31], name_ptr);
    }
    switch (call) {
        case 0x03: capture_bios_write(cpu);  break;  // write()
        case 0x09:                                    // putc()
        case 0x3C: capture_bios_putc(cpu);   break;  // putchar()
        case 0x3E: capture_bios_puts(cpu);   break;  // puts()
        case 0x3F: capture_bios_printf(cpu); break;  // printf() — substituted, see capture_bios_printf

        case 0x40: {
            // SystemErrorUnresolvedException — one-shot full CPU state dump before loop
            static bool dumped_0x40 = false;
            if (!dumped_0x40) {
                dumped_0x40 = true;
                uint32_t epc = cpu->in_delay_slot ? (cpu->current_pc - 4) : cpu->current_pc;
                LOG_CPU_ERROR("[CPU] === A0(0x40) SystemErrorUnresolvedException ===");
                LOG_CPU_ERROR("[CPU]  PC=0x%08x  nPC=0x%08x  curPC=0x%08x",
                              cpu->pc, cpu->next_pc, cpu->current_pc);
                LOG_CPU_ERROR("[CPU]  EPC=0x%08x  Cause=0x%08x  SR=0x%08x  BD=%d",
                              epc, cpu->cause, cpu->sr, cpu->in_delay_slot);
                for (int i = 0; i < 32; i += 4) {
                    LOG_CPU_ERROR("[CPU]  $%02d=0x%08x  $%02d=0x%08x  $%02d=0x%08x  $%02d=0x%08x",
                                  i,   cpu->regs[i],
                                  i+1, cpu->regs[i+1],
                                  i+2, cpu->regs[i+2],
                                  i+3, cpu->regs[i+3]);
                }
                LOG_CPU_ERROR("[CPU]  $ra(saved)=0x%08x", cpu->regs[31]);
                LOG_CPU_ERROR("[CPU] === END A0(0x40) STATE ===");
            }
            break;
        }

        case 0xA1: {
            // SystemError(type, errorcode) — type: 'B'=Boot, 'D'=Disk (DOCS/kernelbios.md).
            // Per docs this function loops on itself by design once reached, so this is
            // always the terminal/fatal condition — one-shot dump with full context.
            static bool dumped_0xA1 = false;
            if (!dumped_0xA1) {
                dumped_0xA1 = true;
                uint32_t type_arg = cpu->regs[4];  // $a0
                uint32_t errorcode = cpu->regs[5]; // $a1
                char type_ch = (type_arg >= 0x20 && type_arg < 0x7F) ? (char)type_arg : '?';
                uint32_t epc = cpu->in_delay_slot ? (cpu->current_pc - 4) : cpu->current_pc;
                LOG_CPU_ERROR("[CPU] === A0(0xA1) SystemError(type='%c' (0x%08x), errorcode=0x%08x) ===",
                              type_ch, type_arg, errorcode);
                LOG_CPU_ERROR("[CPU]  Called from $ra=0x%08x  PC=0x%08x  nPC=0x%08x  curPC=0x%08x",
                              cpu->regs[31], cpu->pc, cpu->next_pc, cpu->current_pc);
                LOG_CPU_ERROR("[CPU]  EPC=0x%08x  Cause=0x%08x  SR=0x%08x  BD=%d",
                              epc, cpu->cause, cpu->sr, cpu->in_delay_slot);
                for (int i = 0; i < 32; i += 4) {
                    LOG_CPU_ERROR("[CPU]  $%02d=0x%08x  $%02d=0x%08x  $%02d=0x%08x  $%02d=0x%08x",
                                  i,   cpu->regs[i],
                                  i+1, cpu->regs[i+1],
                                  i+2, cpu->regs[i+2],
                                  i+3, cpu->regs[i+3]);
                }
                LOG_CPU_ERROR("[CPU] === END A0(0xA1) STATE ===");
                cpu_dump_exec_trace(cpu, "logs/exec_trace_systemerror_a1.log");
                LOG_CPU_ERROR("[CPU] Dumped exec trace to logs/exec_trace_systemerror_a1.log");
            }
            break;
        }

        default: break;
    }
    // LLE: Do NOT fake return values — BIOS kernel executes its real implementations.
    return false;
}

// Called from op_jr when target == 0xB0
void handle_b0_syscall(Cpu* cpu) {
    uint32_t call = cpu->regs[9]; // $t1
    
    // Massive hex values mean this is an internal jump/hook, not a syscall
    if (call > 0x5D) return;

    bios_last_syscall.table = 1;
    bios_last_syscall.func = call;
    bios_last_syscall.name = get_bios_b_function_name(call);
    
    // Suppress noise for high-frequency polling calls (B0(0x32)=open, B0(0x2C), etc.)
    // NOTE: 0x32 is "open", not "GetC" as the old comment claimed — verified against
    // DOCS/kernelbios.md and by static disassembly of the B0(0x32) trampoline.
    if (call != 0x32 && call != 0x2C) {
        LOG_BIOS_DEBUG("[BIOS] B0(%s / 0x%02X)", bios_last_syscall.name, call);
    }
    if (call == 0x32 && cpu->inter) {
        // open(filename, accessmode) — filename capture, bypassing the log
        // suppression above just for this, to root-cause a failing open().
        uint32_t name_ptr = cpu->regs[4];
        char name[64]; uint32_t i;
        for (i = 0; i < sizeof(name) - 1 && name_ptr; i++) {
            uint8_t b = bios_peek8(cpu->inter, name_ptr + i);
            if (b == 0) break;
            name[i] = (char)b;
        }
        name[i] = 0;
        LOG_BIOS_DEBUG("[BIOS] B0(open) open(\"%s\", 0x%x) called from $ra=0x%08x $a0=0x%08x",
                       name, cpu->regs[5], cpu->regs[31], name_ptr);
    }
    if (call == 0x00) {
        LOG_BIOS_DEBUG("[BIOS] AllocKernelMemory(size=0x%x) ra=0x%08x", cpu->regs[4], cpu->regs[31]);
        if (cpu->regs[4] == 0xf1000001) {
            static bool dumped_bad_alloc = false;
            if (!dumped_bad_alloc) {
                dumped_bad_alloc = true;
                cpu_dump_exec_trace(cpu, "logs/exec_trace_bad_alloc.log");
                LOG_BIOS_ERROR("[BIOS] Dumped exec trace for bad AllocKernelMemory size to logs/exec_trace_bad_alloc.log");
            }
        }
    }

    switch (call) {
        case 0x35: capture_bios_write(cpu); break;  // write()
        case 0x3B:                                   // putc()
        case 0x3D: capture_bios_putc(cpu);  break;  // putchar()
        case 0x3F: capture_bios_puts(cpu);  break;  // puts()
        default:   break;
    }
    // Do NOT set cpu->regs[2] — BIOS executes its real function after this hook
}

// Called from op_jr when target == 0xC0
void handle_c0_syscall(Cpu* cpu) {
    uint32_t call = cpu->regs[9]; // $t1
    if (call > 0x1F) return;
    LOG_BIOS_DEBUG("[BIOS] C0(%s / 0x%02X)", get_bios_c_function_name(call), call);
}
