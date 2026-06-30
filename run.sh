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
#   macOS:  brew install cmake sdl3 zstd zlib python3
#   Linux:  apt/dnf install cmake SDL3-devel libzstd-dev zlib1g-dev python3 build-essential
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
pkg-config --exists sdl3 || die "SDL3 not found (macOS: brew install sdl3; Linux: SDL3-devel / libsdl3-dev)"
CC="${CC:-cc}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ---- 0b. sync git submodules (vendor/beetle-psx = the GTE/MDEC/SPU/CHD backend) -----
# A plain `git pull` does NOT update submodules, so after a pull the beetle sources can be stale and
# the link fails with undefined GTE_BindState / MDEC_*State / SPU_*State. Sync them here so
# `git pull && ./run.sh` is self-sufficient. Guard: if a submodule has UNCOMMITTED edits (the dev
# works in the beetle fork in-tree), skip the auto-checkout and just warn — never clobber local work.
if command -v git >/dev/null && [ -f .gitmodules ]; then
  if git submodule status --recursive 2>/dev/null | grep -q '^-'; then
    say "initializing git submodules…"
    git submodule update --init --recursive || die "git submodule update failed"
  elif git submodule status --recursive 2>/dev/null | grep -q '^+'; then
    # gitlink moved (e.g. after a pull) but checkout differs. Update only the CLEAN ones.
    if [ -z "$(git -C vendor/beetle-psx status --porcelain 2>/dev/null)" ]; then
      say "updating git submodules to recorded commits…"
      git submodule update --recursive || die "git submodule update failed"
    else
      say "WARNING: vendor/beetle-psx has uncommitted changes; not auto-updating submodule (commit or stash to sync)."
    fi
  fi
fi

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

# ---- 3. extract MAIN.EXE (recompiled to the shard substrate by emit.py) + the stage overlays ----
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
# disc into RAM. Fed to emit.py --overlays so the recompiler seeds the resident MAIN functions the
# overlays call. (The overlay CODE itself is not yet recompiled — calls into it currently fail fast;
# overlay static recompilation is the next phase.) Game data — gitignored scratch.
OVL=scratch/bin/overlays
mkdir -p "$OVL"
for b in START DEMO GAME; do
  [ -f "$OVL/$b.BIN" ] || "$DISCDUMP" get "BIN/$b.BIN" "$DISC" "$OVL" >/dev/null 2>&1 || true
done

# ---- 4. recompile MAIN.EXE + boot stub -> C, then build the native runtime ----------
# The interpreter is GONE (2026-06-30): the statically-recompiled shards (generated/shard_*.c) ARE the
# execution substrate for every non-native guest function, so the build REQUIRES them. generated/ is
# gitignored (regenerable), so regenerate whenever it is missing or stale (MAIN/stub/emit.py changed).
GEN=generated/tomba2_rec.c
RT=runtime/recomp       # common PSX->PC platform (future psxport submodule)
ENG=engine              # Tomba2Engine — game-specific native engine + RE
mkdir -p generated scratch/bin
if [ ! -f generated/shard_disp.c ] || [ ! -f "$GEN" ] || [ "$MAIN" -nt "$GEN" ] || [ "$STUB" -nt "$GEN" ] || [ tools/recomp/emit.py -nt "$GEN" ]; then
  say "recompiling MAIN.EXE + boot stub -> C (the execution substrate)…"
  python3 tools/recomp/emit.py "$MAIN" "$GEN" --overlays "$OVL" --stub "$STUB" >/dev/null
fi

# ---- 4b. build the native port via CMake (single source of truth: cmake/tomba2_port.cmake) ----------
# CMake owns the whole port build: the source list, the vendored RmlUi static-lib subbuild + link, the
# SDL_GPU SPIR-V shader generation, the beetle/libchdr backend, and the SDL3/freetype link. It emits
# scratch/bin/tomba2_port (RUNTIME_OUTPUT_DIRECTORY). Configure is idempotent (fast when up to date); the
# build is incremental. (The old hand-rolled per-file g++ compile/link + tools/build_port.sh are retired.)
say "building the native port (CMake -j$JOBS)…"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null || die "cmake configure failed"
cmake --build build -j "$JOBS" --target tomba2_port || die "port build failed"

# ---- 5. run ------------------------------------------------------------------------
say "launching Tomba! 2 (native PC port)…"
# Windowed by default; PSXPORT_NOWINDOW selects offscreen-headless (the single mode discriminator is
# PSXPORT_VK_HEADLESS, read by the renderer — there is no PSXPORT_GPU_WINDOW behavior gate any more).
[ -n "${PSXPORT_NOWINDOW:-}" ] && export PSXPORT_VK_HEADLESS=1
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
PSXPORT_TOMBA2_DISC="$DISC" exec ./scratch/bin/tomba2_port "$MAIN"
