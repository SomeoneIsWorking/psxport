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

echo "[1/3] decode test"; python3 tools/recomp/test_decode.py >/dev/null && echo "  decoder ok"
echo "[2/3] emit"; python3 tools/recomp/emit.py "$MAIN" "$GEN"
echo "[3/3] compile + leaf test"
$CC $CFLAGS "$GEN" "$RT/mem.c" "$RT/stubs.c" "$RT/test_leaf.c" -o scratch/bin/test_leaf
./scratch/bin/test_leaf
