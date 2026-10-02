/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hardware tests load at the usual PS-X EXE address, in cached KSEG0 RAM. */
ENTRY(_start)
SECTIONS
{
    . = 0x80010000;
    .text   : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) *(.sdata*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss (NOLOAD) : { *(.sbss*) *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.note*) *(.pdr) *(.gnu.attributes) }
}
