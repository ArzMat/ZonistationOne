# Makefile for PS1 Emulator and log splitter

# Compiler and flags
CC = gcc
CXX = g++

# Common includes and libs.
#
# SDL3 comes from pkg-config on both sides, not a bare -lSDL3: the library is
# built from source into /usr/local (Ubuntu 24.04 and its derivatives ship no
# libsdl3-dev), so the linker needs the -L and the -rpath that sdl3.pc carries.
INCLUDES = -Iinclude -Ithird_party/imgui -Ithird_party/imgui/backends -Ithird_party/lua

SDL_CFLAGS = $(shell pkg-config --cflags sdl3)
SDL_LIBS   = $(shell pkg-config --libs sdl3)

LIBS = $(SDL_LIBS) -lGL -lGLEW -lm -lpthread

# Build mode. Default is an optimised build — the emulator is an interpreter on
# the hot path, so an unoptimised (-O0) build ran ~3-5x slower than the machine,
# left no headroom over the frame budget and drifted the moment any debug
# instrumentation was on. `make DEBUG=1` restores an -O0 build for stepping in gdb.
ifdef DEBUG
  OPT = -O0 -g
else
  OPT = -O3 -g -march=native -DNDEBUG
endif

# Link-time optimisation.
#
# The interpreter's hot path crosses translation units constantly: every
# instruction calls debugger_check_breakpoint (debugger.c) and every load and
# store calls debugger_check_read/write_watchpoint, all of which return
# immediately when nothing is set — a perf profile of a 30 s run put those three
# at 2.92% of all samples doing nothing at all. cpu_reg (cpu_registers.c) and
# mask_region (bus.c) are single-expression functions that cost another 1.90%
# purely in call overhead. Without LTO the compiler cannot see across the .o
# boundary to inline any of them.
#
# Measured on a 30 s Monsters & Co. run, median of 3, ZS1_FRAME_PROFILE only:
#   gcc-14, no LTO   emu 3.710 ms
#   gcc-13, no LTO   emu 3.495 ms
#   gcc-13, LTO      emu 3.225 ms   (-7.7% from LTO, -13.1% from the baseline)
# CPI stayed at 1.618 in all three, which is the check that the emulated machine
# did not change: CPI is a guest property and no host optimisation may move it.
#
# LTO objects from two different gcc majors cannot meet in one link. This
# machine has gcc 14.2 but g++ 13.3, and lto-wrapper refuses the mismatch:
#   "bytecode stream in file 'src/main.o' generated with LTO version 14.0
#    instead of the expected 13.1"
# That used to switch LTO off altogether, so a plain `make` on that machine
# built the slower binary.
#
# Force it off with `make LTO=`; a matching pair still works as before:
#   make clean && make CC=gcc-13 CXX=g++-13
#
# A mismatch no longer turns LTO off for the C side. Everything on the hot path
# (interpreter, bus, GPU command decoder, SPU) is C; only ImGui and debug_ui.cpp
# are C++, and they gain nothing from it. So with mismatched majors the C objects
# are still built with -flto, the C++ objects without, and the link goes through
# the C driver, whose lto-wrapper matches the C objects, with the C++ runtime
# named by its full path (the C driver cannot find the unversioned libstdc++.so
# of a different gcc on its own).
CC_MAJOR  := $(shell $(CC) -dumpversion 2>/dev/null | cut -d. -f1)
CXX_MAJOR := $(shell $(CXX) -dumpversion 2>/dev/null | cut -d. -f1)
ifeq ($(CC_MAJOR),$(CXX_MAJOR))
  LTO     ?= -flto=auto
  LTO_C   ?= $(LTO)
  LTO_CXX ?= $(LTO)
  LINK     = $(CXX)
  LINK_LTO = $(LTO)
  LINK_CXXRT =
else
  LTO     ?= -flto=auto
  LTO_C   ?= $(LTO)
  LTO_CXX ?=
  LINK     = $(CC)
  LINK_LTO = $(LTO_C)
  LINK_CXXRT = $(shell $(CXX) -print-file-name=libstdc++.so)
  $(info [build] $(CC) is $(CC_MAJOR) but $(CXX) is $(CXX_MAJOR): LTO on for C only, linking with $(CC).)
endif

# Profile-guided optimisation, two builds around one representative run:
#   make clean && make PGO_GEN=1     # instrumented binary
#   ./ZoniStation_One ...            # 60 s of a fixed scene; writes pgo/*.gcda
#   make clean && make PGO_USE=1     # optimised with that profile
# The profile directory survives `make clean` on purpose.
PGO_DIR = $(CURDIR)/pgo
ifdef PGO_GEN
  PGO_FLAGS = -fprofile-generate=$(PGO_DIR) -fprofile-update=atomic
endif
ifdef PGO_USE
  PGO_FLAGS = -fprofile-use=$(PGO_DIR) -fprofile-partial-training -Wno-missing-profile
endif

# Highest log level compiled in. TRACE lines sit on per-instruction and
# per-primitive paths, so a normal build drops them at compile time (include/log.h).
# `make LOG_MAX_LEVEL=TRACE` puts them back for a tracing session.
ifdef LOG_MAX_LEVEL
  LOG_DEFS = -DZS1_LOG_MAX_LEVEL=LOG_LEVEL_$(LOG_MAX_LEVEL)
endif

# Header dependency tracking. Without it, `make` after editing anything in
# include/ relinked stale objects: a struct whose layout changed in one
# translation unit and not another produces a binary that segfaults or
# misbehaves silently, and the only reliable answer was `make clean && make`
# every time. -MMD writes a .d file listing the headers each object really
# includes (project headers only — system ones do not change under us), and
# -MP emits a phony target for each so deleting a header does not wedge the
# build with "no rule to make target".
DEPFLAGS = -MMD -MP

# --- Vulkan backend, optional ---
#
# Two things are needed and neither is guaranteed on a machine that can still
# build and run this emulator: the Vulkan headers, and a GLSL-to-SPIR-V
# compiler. When either is missing the tree builds with the OpenGL backend
# alone and says so, the same way LTO above warns instead of failing — a
# default that does not build is not a default.
#
# The Vulkan *loader* is deliberately NOT a link-time dependency: vk_loader.c
# resolves every entry point through SDL_Vulkan_LoadLibrary() at runtime, so the
# binary still starts on a machine with no ICD, with the backend absent from the
# picker and the reason written out.
VK_HEADERS := $(wildcard /usr/include/vulkan/vulkan.h)
GLSLANG    := $(shell command -v glslangValidator 2>/dev/null)

ifeq ($(strip $(VK_HEADERS)),)
  $(info [build] Vulkan backend off: no <vulkan/vulkan.h> — install libvulkan-dev)
else ifeq ($(strip $(GLSLANG)),)
  $(info [build] Vulkan backend off: no glslangValidator — install glslang-tools)
else
  ENABLE_VULKAN := 1
endif

ifdef ENABLE_VULKAN
  VK_DEFS     = -DENABLE_VULKAN=1
  VK_INCLUDES = -Isrc/gpu/vk -Isrc/gpu/shaders
endif

# --- Shaders -> SPIR-V -> C arrays ---
#
# The GLSL stays readable in src/gpu/shaders/; the build turns each stage into a
# .spv and then into a .spv.h holding it as a byte array, so the binary carries
# its shaders and needs no compiler at runtime. The .spv/.spv.h are generated,
# gitignored, and rebuilt when the GLSL changes — ps1_common.glsl is a
# dependency of every stage that includes it, which is why it is listed
# explicitly rather than left to the pattern rule.
VK_SHADER_SRCS = \
    src/gpu/shaders/ps1.vert src/gpu/shaders/ps1.frag \
    src/gpu/shaders/fullscreen.vert src/gpu/shaders/scanout.frag \
    src/gpu/shaders/vram_view.frag
VK_SHADER_HDRS = $(addsuffix .spv.h,$(VK_SHADER_SRCS))

GLSLFLAGS = -V --target-env vulkan1.3 -Isrc/gpu/shaders

%.spv: % src/gpu/shaders/ps1_common.glsl
	@$(GLSLANG) $(GLSLFLAGS) -o $@ $< > /dev/null

# Symbol name from the file: src/gpu/shaders/ps1.frag -> zs1_shader_ps1_frag
# Written through a temporary file: a failed xxd (not installed) used to leave an
# empty header behind, which the next build took as up to date.
%.spv.h: %.spv
	@xxd -i -n zs1_shader_$(subst .,_,$(notdir $*)) $< > $@.tmp && mv $@.tmp $@

shaders: $(VK_SHADER_HDRS)
.PHONY: shaders

ifdef DEBUG
  LTO_C   :=
  LTO_CXX :=
  LINK_LTO :=
endif

CFLAGS = -std=c99 $(OPT) $(LTO_C) $(PGO_FLAGS) -Wall -Wextra $(DEPFLAGS) $(INCLUDES) $(VK_INCLUDES) $(SDL_CFLAGS) $(VK_DEFS) $(LOG_DEFS)
CXXFLAGS = -std=c++11 $(OPT) $(LTO_CXX) $(PGO_FLAGS) -Wall -Wextra $(DEPFLAGS) $(INCLUDES) $(VK_INCLUDES) $(SDL_CFLAGS) $(VK_DEFS) $(LOG_DEFS)
LDFLAGS_EMU = $(OPT) $(LINK_LTO) $(PGO_FLAGS)

# Build every translation unit at once by default. The tree is ~90 objects and
# they are independent; an explicit -j on the command line still wins, because
# make appends command-line flags after MAKEFLAGS.
NPROC := $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)

# --- Core / CPU ---
EMU_CPU_SRCS = \
    src/cpu/cpu_disasm.c src/cpu/cpu_init.c src/cpu/cpu_registers.c \
    src/cpu/cpu_bios.c src/cpu/cpu_exceptions.c src/cpu/cpu_icache.c \
    src/cpu/cpu_decode.c src/cpu/cpu_execution.c src/cpu/cpu_instructions.c

# --- System Core ---
EMU_CORE_SRCS = \
    src/core/bios.c src/core/interconnect.c src/core/bus.c src/core/bus_irq.c \
    src/core/ram.c src/core/dma.c src/core/timers.c src/core/sio.c \
    src/core/mdec.c src/core/controller.c src/core/event_scheduler.c src/core/pcdrv.c \
    src/core/debugger.c src/core/lua_debug.c src/core/system.c \
    src/core/frame_events.c src/core/savestate.c src/core/host_info.c

# --- Lua 5.4 (vendored source, see third_party/lua/) ---
# lua.c/luac.c both define main() (would collide with src/main.c); loadlib.c
# implements dynamic C-module loading (package/require/dlopen) which a debug
# script has no legitimate use for — excluding it drops the -ldl requirement
# too. See lua_debug.c's lua_debug_init for the luaL_requiref() calls that
# replace luaL_openlibs() (which needs loadlib.c's luaopen_package).
EMU_LUA_SRCS = $(filter-out third_party/lua/lua.c third_party/lua/luac.c \
                 third_party/lua/loadlib.c third_party/lua/linit.c, \
                 $(wildcard third_party/lua/*.c))

# --- GPU ---
EMU_GPU_SRCS = \
    src/gpu/gpu.c src/gpu/gpu_helpers.c src/gpu/gpu_commands.c \
    src/gpu/renderer.c src/gpu/renderer_gl.c src/gpu/vram.c

ifdef ENABLE_VULKAN
EMU_GPU_SRCS += src/gpu/vk/vk_loader.c src/gpu/vk/vk_device.c src/gpu/vk/renderer_vk.c
endif

# --- GTE ---
EMU_GTE_SRCS = \
    src/gte/gte.c src/gte/gte_ops.c

# --- CDROM ---
EMU_CDROM_SRCS = \
    src/cdrom/cdrom.c src/cdrom/cdrom_commands.c \
    src/cdrom/cdrom_disc.c src/cdrom/cdrom_audio.c \
    src/cdrom/cdrom_ecm.c src/cdrom/ecm_edc.c

# --- SPU ---
EMU_SPU_SRCS = \
    src/spu/spu.c src/spu/spu_voice.c src/spu/spu_adsr.c \
    src/spu/spu_mixing.c src/spu/spu_dma.c src/spu/spu_irq.c \
    src/spu/spu_stretch.c

# --- Utils ---
EMU_UTIL_SRCS = \
    src/utils/log.c src/utils/rxi_log.c

# --- Emulator C sources (aggregated) ---
EMU_C_SRCS = src/main.c \
    $(EMU_CPU_SRCS) $(EMU_CORE_SRCS) $(EMU_GPU_SRCS) \
    $(EMU_GTE_SRCS) $(EMU_CDROM_SRCS) $(EMU_SPU_SRCS) $(EMU_UTIL_SRCS) $(EMU_LUA_SRCS)

# --- Emulator C++ sources ---
EMU_CXX_SRCS = src/debug_ui.cpp \
               third_party/imgui/imgui.cpp \
               third_party/imgui/imgui_draw.cpp \
               third_party/imgui/imgui_widgets.cpp \
               third_party/imgui/imgui_tables.cpp \
               third_party/imgui/backends/imgui_impl_sdl3.cpp \
               third_party/imgui/backends/imgui_impl_opengl3.cpp

ifdef ENABLE_VULKAN
EMU_CXX_SRCS += third_party/imgui/backends/imgui_impl_vulkan.cpp src/gpu/vk/vk_imgui.cpp
# ImGui must not pull in Vulkan prototypes either: the process links no
# libvulkan, so its backend is handed our resolved pointers instead.
CXXFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES
endif

EMU_OBJS = $(EMU_C_SRCS:.c=.o) $(EMU_CXX_SRCS:.cpp=.o)
EMU_BIN = ZoniStation_One

# --- Unit tests (docs/TESTING_PLAN_2026-08-20.md, layer 1) ---
#
# Each tests/*_test.c is a self-contained program: it #includes the source file
# under test and stubs the few functions that file calls outside itself, so a
# test needs no SDL, no GL, no BIOS and no disc. `make test` builds and runs every
# one of them and stops at the first failure.
UNIT_TEST_SRCS := $(wildcard tests/*_test.c)
UNIT_TEST_BINS := $(patsubst tests/%.c,tests/bin/%,$(UNIT_TEST_SRCS))
TEST_CFLAGS = -std=c99 -O2 -g -Wall -Wextra -MMD -MP -Iinclude

tests/bin/%: tests/%.c
	@mkdir -p tests/bin
	$(CC) $(TEST_CFLAGS) -o $@ $< -lm

ALL_OBJS = $(EMU_OBJS)
DEPS = $(ALL_OBJS:.o=.d) $(wildcard tests/bin/*.d)

.PHONY: all test hwtest clean compile_commands

# `compile_commands` is defined before `all`, and make takes the FIRST real
# target as the default goal — so a bare `make` regenerated compile_commands.json
# and never built anything. The binary then stayed at whatever an earlier
# explicit `make all` had produced, which silently invalidates every run: a
# source edit appears to have "no effect" because it was never compiled in.
.DEFAULT_GOAL := all

# compile_commands.json — what a language server needs to parse this tree.
#
# Without it clangd guesses the include path, fails to find "interconnect.h" and
# the SDL3 headers, and then reports nonsense downstream: include/cpu.h lights up
# with "identifier uint32_t is undefined" on dozens of lines even though the
# header is self-contained and `gcc -fsyntax-only include/cpu.h` returns 0. The
# errors are the editor's, not the code's, and they bury the real ones.
#
# Generated from the same source lists the build uses, so it cannot drift from
# what actually gets compiled, and with no extra tooling to install.
compile_commands:
	@printf '[\n' > compile_commands.json
	@first=1; for f in $(EMU_C_SRCS); do \
	   [ $$first -eq 1 ] || printf ',\n' >> compile_commands.json; first=0; \
	   printf '  {"directory": "%s", "file": "%s", "command": "%s -c %s -o %s"}' \
	     "$(CURDIR)" "$$f" "$(CC) $(CFLAGS)" "$$f" "$${f%.c}.o" >> compile_commands.json; \
	 done; \
	 for f in $(EMU_CXX_SRCS); do \
	   printf ',\n  {"directory": "%s", "file": "%s", "command": "%s -c %s -o %s"}' \
	     "$(CURDIR)" "$$f" "$(CXX) $(CXXFLAGS)" "$$f" "$${f%.cpp}.o" >> compile_commands.json; \
	 done
	@printf '\n]\n' >> compile_commands.json
	@echo "compile_commands.json: $(words $(EMU_C_SRCS) $(EMU_CXX_SRCS)) entries"

ifdef ENABLE_VULKAN
all: $(VK_SHADER_HDRS)
# renderer_vk.c #includes the generated headers, which -MMD cannot know about
# before they exist: without this edge a parallel build could compile it first.
src/gpu/vk/renderer_vk.o: $(VK_SHADER_HDRS)
endif
all: $(EMU_BIN)

$(EMU_BIN): $(EMU_OBJS)
	$(LINK) -o $@ $^ $(LDFLAGS_EMU) $(LIBS) $(LINK_CXXRT)

%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

%.o: %.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS)

test: $(UNIT_TEST_BINS)
	@for t in $(UNIT_TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "all $(words $(UNIT_TEST_BINS)) unit tests passed"

# Bare-metal hardware tests (layer 2): small PS-X EXEs run inside the emulator
# on a zero-filled BIOS, on both renderers. Needs gcc-mipsel-linux-gnu and
# xvfb-run; see tests/hw/README.md.
hwtest: $(EMU_BIN)
	tests/hw/run.sh ./$(EMU_BIN)

split_log: split_log.c
	$(CC) -o split_log split_log.c

clean:
	rm -rf tests/bin tests/hw/build
	rm -f $(EMU_BIN) split_log \
	    src/gpu/shaders/*.spv src/gpu/shaders/*.spv.h \
	    src/*.[od] src/cpu/*.[od] src/core/*.[od] src/gpu/*.[od] src/gte/*.[od] \
	    src/cdrom/*.[od] src/spu/*.[od] src/utils/*.[od] tests/*.[od] \
	    src/gpu/vk/*.[od] \
    third_party/imgui/*.[od] third_party/imgui/backends/*.[od] third_party/lua/*.[od] \
	    *.txt logs/*.txt logs/*_old.txt

# Last: the .d files are generated, so a build that has never run simply has
# none and make carries on.
-include $(DEPS)
