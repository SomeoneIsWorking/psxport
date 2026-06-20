#!/usr/bin/env bash
# Fully automated build-and-run for the Tomba! 2 native PC port (macOS + Linux).
#
#   ./run.sh [/path/to/Tomba2.chd]
#
# Does everything end to end: builds the CHD tooling (libchdr + discdump) via CMake,
# extracts MAIN.EXE from your disc, recompiles the game core + native runtime, and launches
# it in a window. The disc image is yours to provide (never shipped) — pass it as an argument,
# or set PSXPORT_TOMBA2_DISC, or put it in a .env file, or drop a *.chd next to this script.
#
# Requirements (install once):
#   macOS:  brew install cmake sdl2 zstd zlib python3
#   Linux:  apt/dnf install cmake libsdl2-dev libzstd-dev zlib1g-dev python3 build-essential
#
# Env knobs: PSXPORT_NOAUDIO=1 (mute), PSXPORT_GPU_DUMP=dir (dump frames as PPM),
#            CC=clang/gcc (override compiler), PSXPORT_NOWINDOW=1 (headless run).
# no pipefail: several steps use `cmd | head -1`, where head closing early would SIGPIPE the
# producer and (under pipefail) abort the script; results are validated explicitly instead.
set -eu
cd "$(dirname "$0")"

say() { printf '\033[1;36m[run]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[run] error:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 0. toolchain -------------------------------------------------------------------
command -v cmake   >/dev/null || die "cmake not found (macOS: brew install cmake)"
command -v python3 >/dev/null || die "python3 not found"
command -v pkg-config >/dev/null || die "pkg-config not found (macOS: brew install pkg-config)"
pkg-config --exists sdl2 || die "SDL2 not found (macOS: brew install sdl2; Linux: libsdl2-dev)"
CC="${CC:-cc}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ---- 1. resolve the disc ------------------------------------------------------------
DISC="${1:-${PSXPORT_TOMBA2_DISC:-}}"
if [ -z "$DISC" ] && [ -f .env ]; then
  DISC="$(sed -n 's/^[[:space:]]*PSXPORT_TOMBA2_DISC[[:space:]]*=[[:space:]]*//p' .env | head -1)"
  [ -z "$DISC" ] && DISC="$(sed -n 's/^[[:space:]]*PSXPORT_DISC[[:space:]]*=[[:space:]]*//p' .env | head -1)"
fi
if [ -z "$DISC" ]; then
  DISC="$(ls ./*.chd 2>/dev/null | head -1 || true)"
fi
[ -n "$DISC" ] && [ -f "$DISC" ] || die "no disc image — pass it as ./run.sh <disc.chd>, set PSXPORT_TOMBA2_DISC, or drop a *.chd here"
say "disc: $DISC"

# ---- 2. build the CHD tooling (libchdr + discdump) via CMake ------------------------
if [ ! -x build/tools/discdump ] && [ ! -x build/tools/discdump.exe ]; then
  say "building libchdr + discdump (CMake)…"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j "$JOBS" --target discdump >/dev/null
fi
DISCDUMP=build/tools/discdump
[ -x "$DISCDUMP" ] || DISCDUMP=build/tools/discdump.exe
[ -x "$DISCDUMP" ] || die "discdump build failed"

# ---- 3. extract MAIN.EXE (the interpreter runs it from RAM) + the stage overlays ----
MAIN=scratch/bin/tomba2/MAIN.EXE
if [ ! -f "$MAIN" ]; then
  say "extracting MAIN.EXE from the disc…"
  mkdir -p scratch/bin/tomba2
  "$DISCDUMP" get MAIN.EXE "$DISC" scratch/bin/tomba2 >/dev/null
fi
[ -f "$MAIN" ] || die "could not extract MAIN.EXE"

# The disc's boot executable (SCUS_944.54): the real PSX entry point. It draws the SCEA screen,
# then LoadExec's MAIN.EXE — we run it as the authentic boot (runtime/recomp/native_stub.c).
STUB=scratch/bin/tomba2/SCUS_944.54
if [ ! -f "$STUB" ]; then
  say "extracting the boot stub SCUS_944.54 from the disc…"
  "$DISCDUMP" get SCUS_944.54 "$DISC" scratch/bin/tomba2 >/dev/null 2>&1 || true
fi
[ -f "$STUB" ] || die "could not extract the boot stub SCUS_944.54"

# Stage overlays (\BIN\{START,DEMO,GAME}.BIN): runtime-loaded game code the game pulls off the
# disc into RAM; the interpreter runs them in place. (Also fed to emit.py --overlays when the
# offline analysis recompiler is requested.) Game data — gitignored scratch.
OVL=scratch/bin/overlays
mkdir -p "$OVL"
for b in START DEMO GAME; do
  [ -f "$OVL/$b.BIN" ] || "$DISCDUMP" get "BIN/$b.BIN" "$DISC" "$OVL" >/dev/null 2>&1 || true
done

# ---- 4. (analysis only) recompile to C, then build the native runtime ---------------
# The interpreter-only runtime does NOT link the recompiled C — MAIN.EXE + the stub run from RAM via
# the interpreter. emit.py is kept as an OFFLINE analysis aid (readable pseudo-C for RE); regenerate
# generated/ only when asked (PSXPORT_RECOMP=1) so a normal run doesn't depend on the recompiler.
GEN=generated/tomba2_rec.c
RT=runtime/recomp       # common PSX->PC platform (future psxport submodule)
ENG=engine              # Tomba2Engine — game-specific native engine + RE
mkdir -p generated scratch/bin
if [ -n "${PSXPORT_RECOMP:-}" ] && { [ ! -f "$GEN" ] || [ "$MAIN" -nt "$GEN" ] || [ "$STUB" -nt "$GEN" ] || [ tools/recomp/emit.py -nt "$GEN" ]; }; then
  say "recompiling MAIN.EXE + boot stub -> C (analysis aid)…"
  python3 tools/recomp/emit.py "$MAIN" "$GEN" --overlays "$OVL" --stub "$STUB" >/dev/null
fi

# libchdr static libs (versions vary across platforms — locate them).
CHDR_DIR=build/vendor/beetle-psx/deps/libchdr
find_a() { find "$CHDR_DIR" -name "$1" 2>/dev/null | head -1; }
CHD_LIBS="$(find_a 'libchdr-static.a') $(find_a 'libchdr-lzma.a') $(find_a 'libminiz.a')"
ZSTD_A="$(find_a 'libzstd.a')"; [ -n "$ZSTD_A" ] && CHD_LIBS="$CHD_LIBS $ZSTD_A" || CHD_LIBS="$CHD_LIBS $(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)"
[ -n "$(find_a 'libchdr-static.a')" ] || die "libchdr not built (re-run; check the CMake step)"

MED=vendor/beetle-psx/mednafen
# libchdr headers live in the source tree; its compiled .a is under build/ (CHDR_DIR above).
# (No -Igenerated: the interpreter-only runtime links no recompiled shards, so nothing in the
# build includes generated/*. emit.py output is an offline analysis aid only — see later-101.)
INC="-I$RT -I$ENG -I$MED -I$MED/psx -Ivendor/beetle-psx/libretro-common/include -Ivendor/beetle-psx -Ivendor/beetle-psx/deps/libchdr/include"
# _XOPEN_SOURCE: makecontext/swapcontext (native threads) need it on macOS/glibc.
CFLAGS="-O2 -g -w -D_XOPEN_SOURCE=700 $INC $(pkg-config --cflags sdl2 vulkan 2>/dev/null) -DPSXPORT_SDL"
CXX="${CXX:-c++}"   # ImGui mod-overlay (imgui_overlay.cpp + vendored Dear ImGui) is C++
IMGUI=vendor/imgui
CXXFLAGS="-O2 -g -w -fpermissive -std=c++17 $INC -I$IMGUI -I$IMGUI/backends $(pkg-config --cflags sdl2 vulkan 2>/dev/null) -DPSXPORT_SDL"
tools/gen_vk_shaders.sh   # compile+embed the Vulkan present shaders (gpu_vk_shaders.h) before gpu_vk.c
# All TUs. Interpreter-only runtime: MAIN.EXE + the boot stub run from RAM via the interpreter
# (runtime/recomp/dispatch.c + interp.c); the recompiled generated/shard_*.c are NOT linked (the
# recompiler is kept only as an offline analysis aid). See docs/journal.md later-101.
# C++ TUs (.cpp) = the ImGui mod overlay; compiled with $CXX, linked via $CXX. Keep in sync with build_port.sh.
SRC="$RT/dispatch.cpp \
  $RT/cfg.c $RT/mem.cpp $RT/stubs.cpp $RT/hle.cpp $RT/threads.cpp $RT/interp.cpp $RT/gpu_native.cpp $RT/gpu_trace.cpp $RT/gpu_debug.cpp $RT/spu_audio.c $RT/pad_input.cpp $RT/memcard.cpp $RT/native_fmv.cpp \
  $MED/psx/gte.c $RT/gte_beetle.cpp $MED/psx/mdec.c $RT/mdec_beetle.c $MED/psx/spu.c $RT/spu_beetle.c \
  $RT/disc.c $RT/cd_override.cpp $RT/cdc_native.c $RT/xa_stream.c $RT/timing.cpp $RT/gpu_vk.cpp $RT/mods.c $ENG/game_tomba2.cpp $ENG/engine_init.cpp $ENG/engine_level.cpp $ENG/fps60.cpp $ENG/engine_tomba2.cpp $ENG/engine_submit.cpp $ENG/native_terrain.cpp $ENG/render_queue.cpp $ENG/native_path.cpp $ENG/native_path_a1.cpp $ENG/native_path_a2.cpp $ENG/native_path_a3.cpp $ENG/native_path_b1.cpp $ENG/native_path_b2.cpp $ENG/native_path_b3.cpp $ENG/native_path_b4.cpp $ENG/native_path_b5.cpp $ENG/margin_render.cpp $RT/sync_overrides.cpp $RT/native_boot.cpp $RT/dbg_server.cpp $RT/native_stub.cpp $RT/watchdog.c $RT/boot.cpp \
  $RT/imgui_overlay.cpp $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp $IMGUI/backends/imgui_impl_sdl2.cpp $IMGUI/backends/imgui_impl_vulkan.cpp"

say "building the native port in parallel (-j$JOBS)…"
OBJ=scratch/obj; mkdir -p "$OBJ"
compile_one() { o="$OBJ/$(echo "$1" | tr '/.' '__').o";
  case "$1" in
    *.cpp|*.cc) $CXX $CXXFLAGS -c "$1" -o "$o" ;;
    *)          $CC  $CFLAGS  -c "$1" -o "$o" ;;
  esac || { echo "FAILED: $1" >&2; exit 1; }; }
export -f compile_one; export CC CXX CFLAGS CXXFLAGS OBJ
# shellcheck disable=SC2086
printf '%s\n' $SRC | xargs -P"$JOBS" -I{} bash -c 'compile_one "$@"' _ {} || die "compile failed"
OBJS=""; for s in $SRC; do OBJS="$OBJS $OBJ/$(echo "$s" | tr '/.' '__').o"; done
# shellcheck disable=SC2086
# -rdynamic: export symbols so the watchdog's backtrace shows function names (watchdog.c). Link with $CXX (libstdc++).
$CXX -rdynamic $OBJS $CHD_LIBS $(pkg-config --libs sdl2 vulkan) -lpthread -lm -o scratch/bin/tomba2_port || die "link failed"

# ---- 5. run ------------------------------------------------------------------------
say "launching Tomba! 2 (native PC port)…"
WIN=1; [ -n "${PSXPORT_NOWINDOW:-}" ] && WIN=0
# Debug server ON by default so a windowed session can be inspected/driven live (tools/dbgclient.py);
# opt out with PSXPORT_DEBUG_SERVER=0. Window is windowed by default now (PSXPORT_FULLSCREEN=1 to override).
#
# The field terrain renderer 0x8002AB5C is native + ON by default (later-158). It renders PC-native
# (engine/native_terrain.cpp: float transform + real per-pixel depth, NO PSX GTE compose / packet), with
# the gameplay/scene-data prep (sway bytes + object matrix) shared with the recomp body. The later-157
# stopgap was for a now-fixed bug: the native terrain (1) read the WRONG geometry buffer (0x800A1AE8, a
# fabricated address) instead of the recomp's 0x8009FAE8, and (2) wrote the sway-angle scratch to
# scratchpad 0x1F8001C0 — a guest write the recomp never makes (it uses its own stack) — clobbering live
# engine state and breaking terrain collision (Tomba fell through). PSXPORT_TERRAIN_FAITHFUL=1 swaps in
# the GTE/packet transcription as an A/B oracle; PSXPORT_NO_TERRAIN=1 falls back to the recomp body.
PSXPORT_DEBUG_SERVER="${PSXPORT_DEBUG_SERVER:-1}" \
PSXPORT_NO_TERRAIN="${PSXPORT_NO_TERRAIN:-0}" \
PSXPORT_GPU_WINDOW=$WIN PSXPORT_TOMBA2_DISC="$DISC" exec ./scratch/bin/tomba2_port "$MAIN"
