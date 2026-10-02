/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Entry point of a bare-metal hardware test. The emulator's --exe loader has
 * already set $sp from the PS-X EXE header and jumped here; there is no BIOS
 * kernel behind us (the tests run on a zero-filled ROM), so nothing may raise an
 * exception and nothing may call a BIOS function. Clear .bss, run main, park.
 */
    .set noreorder
    .section .text.start, "ax"
    .globl _start
_start:
    la      $t0, __bss_start
    la      $t1, __bss_end
1:  beq     $t0, $t1, 2f
    nop
    sw      $zero, 0($t0)
    b       1b
    addiu   $t0, $t0, 4
2:  jal     main
    nop
3:  b       3b
    nop
