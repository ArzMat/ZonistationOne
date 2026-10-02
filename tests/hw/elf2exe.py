#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wrap a flat binary linked at 0x80010000 into a PS-X EXE the emulator's --exe loader accepts."""
import struct
import sys

LOAD_ADDRESS = 0x80010000
STACK_TOP = 0x801FFF00
HEADER_SIZE = 0x800
SECTOR_SIZE = 0x800
MARKER = b"Sony Computer Entertainment Inc. for North America area"
MARKER_OFFSET = 0x4C


def build_exe(binary: bytes, entry: int, bss_start: int, bss_size: int) -> bytes:
    """
    Build a PS-X EXE image (header + payload padded to whole sectors).

    Args:
        binary: Contents of the text/data sections, starting at LOAD_ADDRESS.
        entry: Initial PC.
        bss_start: Address of .bss, zeroed by the loader (memfill).
        bss_size: Size of .bss in bytes.

    Returns:
        The complete file contents.
    """
    padded = binary + b"\0" * (-len(binary) % SECTOR_SIZE)
    header = bytearray(HEADER_SIZE)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<IIIIIIIIII", header, 0x10,
                     entry, 0, LOAD_ADDRESS, len(padded), 0, 0,
                     bss_start, bss_size, STACK_TOP, 0)
    header[MARKER_OFFSET:MARKER_OFFSET + len(MARKER)] = MARKER
    return bytes(header) + padded


def main() -> int:
    """Command line: elf2exe.py <flat.bin> <entry> <bss_start> <bss_end> <out.exe> (addresses in hex)."""
    if len(sys.argv) != 6:
        sys.stderr.write(main.__doc__ + "\n")
        return 2
    flat_path, entry, bss_start, bss_end, out_path = sys.argv[1:]
    with open(flat_path, "rb") as flat:
        binary = flat.read()
    start = int(bss_start, 16)
    image = build_exe(binary, int(entry, 16), start, int(bss_end, 16) - start)
    with open(out_path, "wb") as out:
        out.write(image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
