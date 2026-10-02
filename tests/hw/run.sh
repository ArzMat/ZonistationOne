#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the bare-metal hardware tests and run each one inside the emulator, on
# the OpenGL and the Vulkan backend, with a zero-filled BIOS (no kernel needed).
# A test reports through the TTY port; see tests/hw/hw.h for the line format.
#
#   tests/hw/run.sh [path/to/ZoniStation_One] [test ...]
#
# Needs a MIPS cross compiler (mipsel-linux-gnu-gcc, or MIPS_CC=...), python3
# and xvfb-run. Exit status is non-zero if any check fails or any test does not
# finish within HWTEST_TIMEOUT seconds.
set -u
here=$(cd "$(dirname "$0")" && pwd)
emu=$(cd "$(dirname "${1:-$here/../../ZoniStation_One}")" && pwd)/$(basename "${1:-ZoniStation_One}")
[ $# -gt 0 ] && shift
tests=${*:-gpu mdec dma spu}
cc=${MIPS_CC:-mipsel-linux-gnu-gcc}
prefix=${cc%gcc}
backends=${HWTEST_BACKENDS:-gl vulkan}
timeout_s=${HWTEST_TIMEOUT:-60}
out=${HWTEST_OUT:-$here/build}
cflags="-march=r3000 -mabi=32 -msoft-float -mno-abicalls -fno-pic -G0 -O2 \
-ffreestanding -fno-builtin -nostdlib -Wall -Wextra -Wl,--build-id=none"

command -v "$cc" >/dev/null || { echo "hwtest: $cc not found (apt install gcc-mipsel-linux-gnu)"; exit 2; }
command -v xvfb-run >/dev/null || { echo "hwtest: xvfb-run not found"; exit 2; }
[ -x "$emu" ] || { echo "hwtest: emulator binary $emu not found (run make first)"; exit 2; }
mkdir -p "$out"
head -c 524288 /dev/zero > "$out/zero_bios.bin"

status=0
for t in $tests; do
    "$cc" $cflags -T "$here/ps1.ld" -o "$out/$t.elf" "$here/crt0.S" "$here/${t}_test.c" -lgcc || exit 2
    "${prefix}objcopy" -O binary -j .text -j .rodata -j .data "$out/$t.elf" "$out/$t.bin"
    sym() { "${prefix}nm" "$out/$t.elf" | awk -v s="$1" '$3 == s { print $1 }'; }
    python3 "$here/elf2exe.py" "$out/$t.bin" "$(sym _start)" "$(sym __bss_start)" "$(sym __bss_end)" "$out/$t.exe" || exit 2

    for gfx in $backends; do
        log="$out/$t.$gfx.log"
        : > "$log"
        (cd "$out" && SDL_AUDIODRIVER=dummy ZS1_LOG_STDERR=1 ZS1_GFX=$gfx \
            exec xvfb-run -a -s "-screen 0 1280x720x24" "$emu" zero_bios.bin --exe="$t.exe") > "$log" 2>&1 &
        pid=$!
        waited=0
        while [ $waited -lt "$timeout_s" ] && ! grep -q "HWTEST $t DONE" "$log"; do
            sleep 1; waited=$((waited + 1))
        done
        pkill -P $pid 2>/dev/null; kill $pid 2>/dev/null; wait $pid 2>/dev/null
        pkill -f "$emu zero_bios.bin --exe=$t.exe" 2>/dev/null
        backend=$(grep -o "Backend selected: [A-Za-z0-9 .]*" "$log" | tail -1)
        echo "== $t on $gfx (${backend:-backend unknown})"
        grep -o "HWTEST .*" "$log" | sed 's/\x1b\[[0-9;]*m//g' | grep -v " BEGIN$"
        if ! grep -q "HWTEST $t DONE pass=[0-9]* fail=0" "$log"; then
            grep -q "HWTEST $t DONE" "$log" || echo "HWTEST $t TIMEOUT after ${timeout_s}s (log: $log)"
            status=1
        fi
    done
done
exit $status
