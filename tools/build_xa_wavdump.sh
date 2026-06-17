#!/usr/bin/env bash
# Build the standalone offline XA extractor (tools/xa_wavdump). Links libchdr statically (same .a the
# port build uses). No game/SDL deps.
set -e
cd "$(dirname "$0")/.."
INC="-Ivendor/beetle-psx/deps/libchdr/include"
find_a(){ find build -name "$1" 2>/dev/null | head -1; }
LIBS="$(find_a libchdr-static.a) $(find_a libchdr-lzma.a) $(find_a libminiz.a)"
ZSTD_A="$(find_a libzstd.a)"; [ -n "$ZSTD_A" ] && LIBS="$LIBS $ZSTD_A" || LIBS="$LIBS $(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)"
[ -n "$(find_a libchdr-static.a)" ] || { echo "libchdr not built — run ./run.sh once"; exit 1; }
cc -O2 -o tools/xa_wavdump tools/xa_wavdump.c $INC $LIBS -lpthread
echo "built tools/xa_wavdump"
