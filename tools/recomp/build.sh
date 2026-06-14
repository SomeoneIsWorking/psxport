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
CFLAGS="-O2 -w -I$RT"

mkdir -p generated scratch/bin

# Native CD backend links libchdr (prebuilt by the discdump CMake build).
CHDR=build/vendor/beetle-psx/deps/libchdr
CHD_INC="-Ivendor/beetle-psx/deps/libchdr/include"
CHD_LIBS="$CHDR/libchdr-static.a $CHDR/deps/lzma-25.01/libchdr-lzma.a \
          $CHDR/deps/miniz-3.1.1/libminiz.a $CHDR/deps/zstd-1.5.7/libzstd.a"

# GTE (COP2) lifted from the Beetle fork (compiled as-is via gte_beetle.c adapter).
MEDNAFEN=vendor/beetle-psx/mednafen
GTE_INC="-I$MEDNAFEN -I$MEDNAFEN/psx -Ivendor/beetle-psx/libretro-common/include"
GTE_SRC="$MEDNAFEN/psx/gte.c $RT/gte_beetle.c"

echo "[1/3] decode test"; python3 tools/recomp/test_decode.py >/dev/null && echo "  decoder ok"
echo "[2/3] emit"; python3 tools/recomp/emit.py "$MAIN" "$GEN"
RUNTIME="$RT/mem.c $RT/stubs.c $RT/hle.c $RT/threads.c $RT/interp.c $GTE_SRC"
CD="$RT/disc.c $RT/cd_override.c $RT/timing.c $RT/games_tomba2.c"
echo "[3/3] compile core + runtime, run leaf test + boot"
$CC $CFLAGS $GTE_INC "$GEN" $RUNTIME "$RT/test_leaf.c" -o scratch/bin/test_leaf
./scratch/bin/test_leaf
echo "--- boot (6s cap) ---"
$CC $CFLAGS $CHD_INC $GTE_INC "$GEN" $RUNTIME $CD "$RT/boot.c" $CHD_LIBS -lpthread -o scratch/bin/boot
timeout 6 ./scratch/bin/boot 2>&1 | awk '!seen[$0]++' | head -20 || true
