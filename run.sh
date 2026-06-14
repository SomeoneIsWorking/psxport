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

# ---- 3. extract MAIN.EXE (the recompiler input) ------------------------------------
MAIN=scratch/bin/tomba2/MAIN.EXE
if [ ! -f "$MAIN" ]; then
  say "extracting MAIN.EXE from the disc…"
  mkdir -p scratch/bin/tomba2
  "$DISCDUMP" get MAIN.EXE "$DISC" scratch/bin/tomba2 >/dev/null
fi
[ -f "$MAIN" ] || die "could not extract MAIN.EXE"

# ---- 4. recompile the game core + build the native runtime -------------------------
GEN=generated/tomba2_rec.c
RT=runtime/recomp
mkdir -p generated scratch/bin
if [ ! -f "$GEN" ] || [ "$MAIN" -nt "$GEN" ] || [ tools/recomp/emit.py -nt "$GEN" ]; then
  say "recompiling MAIN.EXE -> C…"
  python3 tools/recomp/emit.py "$MAIN" "$GEN" >/dev/null
fi

# libchdr static libs (versions vary across platforms — locate them).
CHDR_DIR=build/vendor/beetle-psx/deps/libchdr
find_a() { find "$CHDR_DIR" -name "$1" 2>/dev/null | head -1; }
CHD_LIBS="$(find_a 'libchdr-static.a') $(find_a 'libchdr-lzma.a') $(find_a 'libminiz.a')"
ZSTD_A="$(find_a 'libzstd.a')"; [ -n "$ZSTD_A" ] && CHD_LIBS="$CHD_LIBS $ZSTD_A" || CHD_LIBS="$CHD_LIBS $(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)"
[ -n "$(find_a 'libchdr-static.a')" ] || die "libchdr not built (re-run; check the CMake step)"

MED=vendor/beetle-psx/mednafen
# libchdr headers live in the source tree; its compiled .a is under build/ (CHDR_DIR above).
# -Igenerated: the recompiled shards include "rec_decls.h".
INC="-I$RT -Igenerated -I$MED -I$MED/psx -Ivendor/beetle-psx/libretro-common/include -Ivendor/beetle-psx -Ivendor/beetle-psx/deps/libchdr/include"
# _XOPEN_SOURCE: makecontext/swapcontext (native threads) need it on macOS/glibc.
CFLAGS="-O2 -w -D_XOPEN_SOURCE=700 $INC $(pkg-config --cflags sdl2) -DPSXPORT_SDL"
# All TUs: the recompiled core is split into generated/shard_*.c so they compile in parallel.
SRC="$(ls generated/shard_*.c) \
  $RT/mem.c $RT/stubs.c $RT/hle.c $RT/threads.c $RT/interp.c $RT/gpu_native.c $RT/spu_audio.c \
  $MED/psx/gte.c $RT/gte_beetle.c $MED/psx/mdec.c $RT/mdec_beetle.c $MED/psx/spu.c $RT/spu_beetle.c \
  $RT/disc.c $RT/cd_override.c $RT/timing.c $RT/games_tomba2.c $RT/boot.c"

say "building the native port in parallel (-j$JOBS; first time compiles the recompiled core)…"
OBJ=scratch/obj; mkdir -p "$OBJ"
compile_one() { o="$OBJ/$(echo "$1" | tr '/.' '__').o"; $CC $CFLAGS -c "$1" -o "$o" || { echo "FAILED: $1" >&2; exit 1; }; }
export -f compile_one; export CC CFLAGS OBJ
# shellcheck disable=SC2086
printf '%s\n' $SRC | xargs -P"$JOBS" -I{} bash -c 'compile_one "$@"' _ {} || die "compile failed"
OBJS=""; for s in $SRC; do OBJS="$OBJS $OBJ/$(echo "$s" | tr '/.' '__').o"; done
# shellcheck disable=SC2086
$CC $OBJS $CHD_LIBS $(pkg-config --libs sdl2) -lpthread -o scratch/bin/tomba2_port || die "link failed"

# ---- 5. run ------------------------------------------------------------------------
say "launching Tomba! 2 (native PC port)…"
WIN=1; [ -n "${PSXPORT_NOWINDOW:-}" ] && WIN=0
PSXPORT_GPU_WINDOW=$WIN PSXPORT_TOMBA2_DISC="$DISC" exec ./scratch/bin/tomba2_port "$MAIN"
