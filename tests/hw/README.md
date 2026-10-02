# Bare-metal hardware tests

Layer 2 of `docs/TESTING_PLAN_2026-08-20.md`, without the copyrighted BIOS: each test is a small
PS-X EXE that runs inside the emulator on a zero-filled ROM through the `--exe` loader. There is no
kernel, interrupts stay off as after reset, so the tests poll status bits and never raise an
exception. They write their results to the DUART TTY port (1F802023h), which the emulator already
logs:

```
HWTEST <suite> <check> PASS
HWTEST <suite> <check> FAIL got=XXXXXXXX want=XXXXXXXX
HWTEST <suite> DONE pass=N fail=M
```

```sh
sudo apt install gcc-mipsel-linux-gnu xvfb mesa-vulkan-drivers   # once
make hwtest                         # every suite, OpenGL and Vulkan
tests/hw/run.sh ./ZoniStation_One gpu spu
HWTEST_BACKENDS=gl tests/hw/run.sh  # one backend only
```

Each check rests on a psx-spx line cited next to it. Every check that covers a defect was run
against the code before its fix and failed there, on at least one renderer; the others
(`decode_complete`, `transfer_completes`, `data_reached_vram`) are sanity checks. A PASS therefore
means the documented behaviour, not just "no crash".

| Suite | Checks |
|---|---|
| `gpu` | GP0(02h) fill outside the drawing area and with GP0(E6h).0 set; GP0(80h) copy of pixels rasterised in the same field; GP0(A0h) upload wrapping at the right edge of VRAM. Every area is dirtied by a primitive first, so the readback comes from the renderer, not from the CPU copy of VRAM. |
| `mdec` | Decode completes; status bits 15-0 read FFFFh after a command; the reset bit keeps the quant and scale tables; FE00h padding after a finished MDEC(1) is not taken as a parameter count. |
| `dma` | A SyncMode 1 transfer leaves MADR at the end address and BA at zero; the data reaches VRAM. |
| `spu` | The voice IRQ fires every time a looping voice reads the block at IRQA; a Loop End jumps to a repeat address written before Key On; the documented manual-write sequence (Stop, address, FIFO, Manual) lands in SPU RAM; the main volume sweeps. |

The software renderers Mesa provides under Xvfb (llvmpipe for OpenGL, lavapipe for Vulkan) are
enough; no GPU is needed.
