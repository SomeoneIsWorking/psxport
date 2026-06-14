#!/usr/bin/env bash
# Build the static-recompiled Tomba!2 core and run the S1 leaf tests.
#   tools/recomp/build.sh            # emit + compile core + runtime + run leaf tests
# Requires the extracted MAIN.EXE (tools/discdump get MAIN.EXE <disc> scratch/bin/tomba2).
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$repo"

MAIN=scratch/bin/tomba2/MAIN.EXE
GEN=generated/tomba2_rec.c
RT=runtime/recomp
CC=${CC:-gcc}
CFLAGS="-O2 -w -I$RT -Igenerated"   # -Igenerated: the recompiled shards include rec_decls.h

mkdir -p generated scratch/bin

# Native CD backend links libchdr (prebuilt by the discdump CMake build).
CHDR=build/vendor/beetle-psx/deps/libchdr
CHD_INC="-Ivendor/beetle-psx/deps/libchdr/include"
CHD_LIBS="$CHDR/libchdr-static.a $CHDR/deps/lzma-25.01/libchdr-lzma.a \
          $CHDR/deps/miniz-3.1.1/libminiz.a $CHDR/deps/zstd-1.5.7/libzstd.a"

# Peripherals lifted from the Beetle fork (compiled as-is via *_beetle.c adapters):
# GTE (COP2), MDEC (video), SPU (audio).
MEDNAFEN=vendor/beetle-psx/mednafen
MED_INC="-I$MEDNAFEN -I$MEDNAFEN/psx -Ivendor/beetle-psx/libretro-common/include -Ivendor/beetle-psx"
MED_SRC="$MEDNAFEN/psx/gte.c $RT/gte_beetle.c $MEDNAFEN/psx/mdec.c $RT/mdec_beetle.c \
         $MEDNAFEN/psx/spu.c $RT/spu_beetle.c"
# Live window (optional, PSXPORT_GPU_WINDOW=1). SDL is always linked; the window opens on demand.
SDL_CFLAGS="$(pkg-config --cflags sdl2) -DPSXPORT_SDL"
SDL_LIBS="$(pkg-config --libs sdl2)"

echo "[1/3] decode test"; python3 tools/recomp/test_decode.py >/dev/null && echo "  decoder ok"
echo "[2/3] emit"; python3 tools/recomp/emit.py "$MAIN" "$GEN"
GENSRC="$(ls generated/shard_*.c)"   # recompiled core, split into shards for parallel compile
RUNTIME="$RT/mem.c $RT/stubs.c $RT/hle.c $RT/threads.c $RT/interp.c $RT/gpu_native.c $RT/spu_audio.c $MED_SRC"
CD="$RT/disc.c $RT/cd_override.c $RT/timing.c $RT/games_tomba2.c"
echo "[3/3] compile core + runtime, run leaf test + boot"
$CC $CFLAGS $MED_INC $SDL_CFLAGS $GENSRC $RUNTIME "$RT/test_leaf.c" -o scratch/bin/test_leaf $SDL_LIBS
./scratch/bin/test_leaf
echo "--- boot (6s cap) ---"
$CC $CFLAGS $CHD_INC $MED_INC $SDL_CFLAGS $GENSRC $RUNTIME $CD "$RT/boot.c" $CHD_LIBS $SDL_LIBS -lpthread -o scratch/bin/boot
timeout 6 ./scratch/bin/boot 2>&1 | awk '!seen[$0]++' | head -20 || true
