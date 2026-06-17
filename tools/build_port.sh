#!/usr/bin/env bash
# Incremental build of the native port (scratch/bin/tomba2_port).
#
# WHY THIS EXISTS: `make -C runtime` builds the OTHER binary — the Beetle ORACLE
# (runtime/wide60rt). The native port is NOT built by any Makefile; it is built by run.sh
# (full build-extract-run) or by this script (incremental compile + relink, no run).
# Use this during a fix loop so you don't re-extract/re-recompile the whole game core.
#
#   tools/build_port.sh                      # recompile only sources newer than their .o, relink
#   tools/build_port.sh runtime/recomp/cdc_native.c   # force-recompile these file(s), relink
#   tools/build_port.sh all                  # recompile every source, relink
#
# Object naming mirrors run.sh: scratch/obj/<path-with-/.-as-_>.o
# Then drive it with tools/drive.py (which does NOT build — it runs the existing binary).
set -eu
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
RT=runtime/recomp       # common PSX->PC platform (future psxport submodule)
ENG=engine              # Tomba2Engine — the game-specific native engine + RE
MED=vendor/beetle-psx/mednafen
OBJ=scratch/obj
mkdir -p "$OBJ"

INC="-I$RT -I$ENG -Igenerated -I$MED -I$MED/psx -Ivendor/beetle-psx/libretro-common/include -Ivendor/beetle-psx -Ivendor/beetle-psx/deps/libchdr/include"
CFLAGS="-O2 -g -w -D_XOPEN_SOURCE=700 $INC $(pkg-config --cflags sdl2 vulkan 2>/dev/null) -DPSXPORT_SDL"
tools/gen_vk_shaders.sh   # compile+embed the Vulkan present shaders (gpu_vk_shaders.h) before gpu_vk.c

# Same source list as run.sh step 4 (keep in sync).
SRC="$(ls generated/shard_*.c) $(ls generated/stub_shard_*.c) generated/stub_disp.c \
  $RT/cfg.c $RT/mem.c $RT/stubs.c $RT/hle.c $RT/threads.c $RT/interp.c $RT/gpu_native.c $RT/gpu_trace.c $RT/gpu_debug.c $RT/spu_audio.c $RT/pad_input.c $RT/memcard.c $RT/native_fmv.c \
  $MED/psx/gte.c $RT/gte_beetle.c $MED/psx/mdec.c $RT/mdec_beetle.c $MED/psx/spu.c $RT/spu_beetle.c \
  $RT/disc.c $RT/cd_override.c $RT/cdc_native.c $RT/xa_stream.c $RT/timing.c $RT/gpu_vk.c $ENG/game_tomba2.c $ENG/wide60.c $ENG/engine_tomba2.c $RT/sync_overrides.c $RT/native_boot.c $RT/dbg_server.c $RT/native_stub.c $RT/watchdog.c $RT/boot.c"

objof() { echo "$OBJ/$(echo "$1" | tr '/.' '__').o"; }

FORCE=""
[ "${1:-}" = "all" ] && FORCE="all" && shift || true
for f in "$@"; do FORCE="$FORCE $f"; done

compile_one() { o="$(objof "$1")"; $CC $CFLAGS -c "$1" -o "$o" || { echo "FAILED: $1" >&2; exit 1; }; }
export -f compile_one objof; export CC CFLAGS OBJ

TODO=""
for s in $SRC; do
  o="$(objof "$s")"
  # gpu_vk.c embeds the generated SPIR-V header — recompile it when that header changes too, else a
  # shader-only edit relinks a STALE object with old bytecode (cost a long debug session once).
  hdr_dep=""; case "$s" in *gpu_vk.c) hdr_dep="$RT/gpu_vk_shaders.h";; esac
  if [ "$FORCE" = "all" ] || [ ! -f "$o" ] || [ "$s" -nt "$o" ] || \
     { [ -n "$hdr_dep" ] && [ "$hdr_dep" -nt "$o" ]; } || printf '%s' " $FORCE " | grep -qF " $s "; then
    TODO="$TODO $s"
  fi
done
if [ -n "$TODO" ]; then
  echo "[build] compiling:$TODO"
  printf '%s\n' $TODO | xargs -P"$JOBS" -I{} bash -c 'compile_one "$@"' _ {}
else
  echo "[build] nothing to recompile"
fi

CHDR_DIR=build/vendor/beetle-psx/deps/libchdr
find_a() { find "$CHDR_DIR" -name "$1" 2>/dev/null | head -1; }
CHD_LIBS="$(find_a 'libchdr-static.a') $(find_a 'libchdr-lzma.a') $(find_a 'libminiz.a')"
ZSTD_A="$(find_a 'libzstd.a')"; [ -n "$ZSTD_A" ] && CHD_LIBS="$CHD_LIBS $ZSTD_A" || CHD_LIBS="$CHD_LIBS $(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)"

OBJS=""; for s in $SRC; do OBJS="$OBJS $(objof "$s")"; done
# shellcheck disable=SC2086
$CC -rdynamic $OBJS $CHD_LIBS $(pkg-config --libs sdl2 vulkan) -lpthread -lm -o scratch/bin/tomba2_port || { echo "[build] link failed" >&2; exit 1; }
echo "[build] linked scratch/bin/tomba2_port"
